// vfts.tload(<index rows> USING PARAMETERS index, cache_dir [, gen, options]) OVER(PARTITION NODES)
//
// Runs on every node (the procedures feed it through vfts.probe, see sql/procedures.sql) and
// writes the node's cache (cache.h):
//   without gen: D, T, P, Q and S rows of one or more segments -> the segment files. Each file is
//                assembled from the rows, its checksum compared with the S row, verified and
//                written atomically. A segment that is already in the cache is not written again.
//   with gen:    G rows (active segments and their checksums), X rows (tombstones) and K rows
//                (term: the access mode, stem: a key; segment_id 0 the current key, 1 the previous
//                one during a key change) -> the generation file, then ACTIVE. Every G segment
//                must be in the cache. The procedures read K rows from vfts.index_keys, so a
//                key never appears in a statement's text.
// Returns one row per node: node_name, status ('loaded' or 'activated'), segments, bytes.
#include "udx_common.h"
#include "../engine/table_codec.h"

#include <atomic>
#include <map>
#include <mutex>
#include <thread>

using namespace Vertica;
using namespace vfts_udx;

namespace {

class TLoad : public TransformFunction {
public:
    void processPartition(ServerInterface &srv, PartitionReader &in, PartitionWriter &out) override
    {
        try {
            run(srv, in, out);
        } catch (const std::exception &e) {
            vt_report_error(0, "tload on %s: %s", srv.getCurrentNodeName().c_str(), e.what());
        }
    }

private:
    void run(ServerInterface &srv, PartitionReader &in, PartitionWriter &out)
    {
        Settings s(srv);
        const std::string index = read_index_name(s);
        const std::string cache_dir = resolve_cache_dir(s);
        const bool activate = s.has("gen");
        vfts::CacheWriter w(cache_dir, index);
        if (s.has("index_id")) w.claim(s.integer("index_id", 0));
        std::map<std::int64_t, vfts::SegmentAssembler> asm_;
        std::map<std::int64_t, std::uint64_t> expect;           // segment -> checksum (S rows)
        std::map<std::int64_t, std::int64_t> max_ver;
        vfts::Generation g;
        std::map<std::int64_t, std::vector<std::uint32_t>> dead;
        std::uint64_t rows = 0;
        do {
            if (in.isNull(IC_KIND) || in.isNull(IC_SEGMENT)) continue;
            const std::string kind = in.getStringRef(IC_KIND).str();
            const std::int64_t sid = in.getIntRef(IC_SEGMENT);
            if (++rows % 100000 == 0 && isCanceled()) fail("canceled");
            if (kind == "G") {
                vfts::GenSegment gs;
                gs.segment_id = sid;
                gs.checksum = static_cast<std::uint64_t>(in.getIntRef(IC_DOC_ID));
                g.segs.push_back(gs);
            } else if (kind == "K") {
                if (in.isNull(IC_TERM) || in.isNull(IC_STEM)) fail("K row without access mode or key");
                const std::string mode = in.getStringRef(IC_TERM).str();
                if (!g.access.empty() && g.access != mode) fail("K rows with different access modes");
                g.access = mode;
                const std::string key = in.getStringRef(IC_STEM).str();
                if (key.size() < 32 || key.size() > 64) fail("access key must have 32 to 64 characters");
                if (sid == 0) g.keys.insert(g.keys.begin(), key);
                else g.keys.push_back(key);
            } else if (kind == "X") {
                dead[sid].push_back(static_cast<std::uint32_t>(in.getIntRef(IC_DOCNO)));
            } else if (kind == "S") {
                expect[sid] = static_cast<std::uint64_t>(in.getIntRef(IC_DOC_ID));
            } else if (kind == "D") {
                asm_[sid].add(vfts::DocRow{static_cast<std::uint32_t>(in.getIntRef(IC_DOCNO)), in.getIntRef(IC_DOC_ID),
                                            static_cast<std::uint32_t>(in.getIntRef(IC_N))});
            } else if (kind == "T") {
                vfts::TermDictRow r;
                r.term_id = static_cast<std::uint32_t>(in.getIntRef(IC_TERM_ID));
                r.term = in.getStringRef(IC_TERM).str();
                r.stem = in.isNull(IC_STEM) ? std::string() : in.getStringRef(IC_STEM).str();
                r.lang = in.isNull(IC_LANG) ? std::string() : in.getStringRef(IC_LANG).str();
                r.doc_freq = static_cast<std::uint32_t>(in.getIntRef(IC_N));
                asm_[sid].add(r);
            } else if (kind == "P") {
                vfts::PostingRow r;
                r.term_id = static_cast<std::uint32_t>(in.getIntRef(IC_TERM_ID));
                r.chunk_id = static_cast<std::uint32_t>(in.getIntRef(IC_CHUNK));
                const VString &b = in.getStringRef(IC_BIN);
                r.docs.assign(b.data(), b.length());
                asm_[sid].add(std::move(r));
            } else if (kind == "Q") {
                const VString &b = in.getStringRef(IC_BIN);
                asm_[sid].add_part(static_cast<std::uint32_t>(in.getIntRef(IC_TERM_ID)), static_cast<std::uint32_t>(in.getIntRef(IC_CHUNK)),
                                   in.isNull(IC_DOCNO) ? 0u : static_cast<std::uint32_t>(in.getIntRef(IC_DOCNO)), std::string(b.data(), b.length()));
            } else {
                fail("unknown row kind '" + kind + "'");
            }
        } while (in.next());

        std::uint64_t written = 0, bytes = 0;
        if (activate) {
            if (!asm_.empty()) fail("segment rows and gen together: load segments first, then activate");
            g.gen = s.integer("gen", 0);
            if (g.access.empty() || g.keys.empty()) fail("activation without a K row (access mode and key)");
            g.options = s.text("options", "");
            for (auto &gs : g.segs) {
                auto it = dead.find(gs.segment_id);
                if (it != dead.end()) gs.dead = std::move(it->second);
            }
            for (auto &kv : dead)
                if (!kv.second.empty()) fail("tombstones for segment " + std::to_string(kv.first) + ", which is not in the generation");
            w.activate(g);
        } else {
            // assemble, verify and write the segments in parallel (engine work only, no SDK calls)
            std::vector<std::pair<std::int64_t, vfts::SegmentAssembler *>> todo;
            for (auto &kv : asm_) {
                auto e = expect.find(kv.first);
                if (e == expect.end()) fail("no summary row for segment " + std::to_string(kv.first));
                if (!w.has_segment(kv.first, e->second)) todo.emplace_back(kv.first, &kv.second);
            }
            std::atomic<std::size_t> next{0};
            std::mutex mu;
            std::string error;
            auto work = [&]() {
                for (;;) {
                    const std::size_t i = next.fetch_add(1);
                    if (i >= todo.size()) return;
                    try {
                        const std::int64_t sid = todo[i].first;
                        const std::string seg = todo[i].second->finish(sid, 0);
                        vfts::Segment view(seg.data(), seg.size());
                        if (view.header().checksum != expect[sid])
                            throw std::runtime_error("segment " + std::to_string(sid) + " assembled from the tables has another checksum than its summary row");
                        w.write_segment(seg);
                        std::lock_guard<std::mutex> l(mu);
                        ++written;
                        bytes += seg.size();
                    } catch (const std::exception &ex) {
                        std::lock_guard<std::mutex> l(mu);
                        if (error.empty()) error = ex.what();
                        next = todo.size();
                    }
                }
            };
            const std::size_t nt = std::max<std::size_t>(1, std::min<std::size_t>(todo.size(), std::thread::hardware_concurrency()));
            std::vector<std::thread> pool;
            for (std::size_t t = 0; t < nt; ++t) pool.emplace_back(work);
            for (auto &t : pool) t.join();
            if (!error.empty()) fail(error);
        }
        out.getStringRef(0).copy(srv.getCurrentNodeName());
        out.getStringRef(1).copy(activate ? "activated" : "loaded");
        out.setInt(2, activate ? static_cast<vint>(g.segs.size()) : static_cast<vint>(written));
        out.setInt(3, static_cast<vint>(bytes));
        out.next();
    }
};

class TLoadFactory : public TransformFunctionFactory {
public:
    void getPrototype(ServerInterface &, ColumnTypes &argTypes, ColumnTypes &returnType) override
    {
        add_index_row_types(argTypes);
        returnType.addVarchar();
        returnType.addVarchar();
        returnType.addInt();
        returnType.addInt();
    }
    void getReturnType(ServerInterface &, const SizedColumnTypes &, SizedColumnTypes &out) override
    {
        out.addVarchar(128, "node_name");
        out.addVarchar(16, "status");
        out.addInt("segments");
        out.addInt("bytes");
    }
    void getParameterType(ServerInterface &, SizedColumnTypes &p) override
    {
        p.addVarchar(128, "index");
        p.addVarchar(1024, "cache_dir");
        p.addInt("gen");
        p.addVarchar(256, "options");
        p.addInt("index_id");
    }
    TransformFunction *createTransformFunction(ServerInterface &srv) override { return vt_createFuncObject<TLoad>(srv.allocator); }
};

} // namespace

RegisterFactory(TLoadFactory);
