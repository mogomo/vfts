// vtext.tload(<index rows> USING PARAMETERS index, cache_dir [, gen, options]) OVER(PARTITION NODES)
//
// Runs on every node (the procedures feed it through vtext.probe, see sql/procedures.sql) and
// writes the node's cache (cache.h):
//   without gen: D, T, P, Q and S rows of one or more segments -> the segment files. Each file is
//                assembled from the rows, its checksum compared with the S row, verified and
//                written atomically. A segment that is already in the cache is not written again.
//   with gen:    G rows (active segments and their checksums) and X rows (tombstones) -> the
//                generation file, then ACTIVE. Every G segment must be in the cache.
// Returns one row per node: node_name, status ('loaded' or 'activated'), segments, bytes.
#include "udx_common.h"
#include "../engine/table_codec.h"

#include <map>

using namespace Vertica;
using namespace vtext_udx;

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
        vtext::CacheWriter w(cache_dir, index);
        std::map<std::int64_t, vtext::SegmentAssembler> asm_;
        std::map<std::int64_t, std::uint64_t> expect;           // segment -> checksum (S rows)
        std::map<std::int64_t, std::int64_t> max_ver;
        vtext::Generation g;
        std::map<std::int64_t, std::vector<std::uint32_t>> dead;
        std::uint64_t rows = 0;
        do {
            if (in.isNull(IC_KIND) || in.isNull(IC_SEGMENT)) continue;
            const std::string kind = in.getStringRef(IC_KIND).str();
            const std::int64_t sid = in.getIntRef(IC_SEGMENT);
            if (++rows % 100000 == 0 && isCanceled()) fail("canceled");
            if (kind == "G") {
                vtext::GenSegment gs;
                gs.segment_id = sid;
                gs.checksum = static_cast<std::uint64_t>(in.getIntRef(IC_DOC_ID));
                g.segs.push_back(gs);
            } else if (kind == "X") {
                dead[sid].push_back(static_cast<std::uint32_t>(in.getIntRef(IC_DOCNO)));
            } else if (kind == "S") {
                expect[sid] = static_cast<std::uint64_t>(in.getIntRef(IC_DOC_ID));
            } else if (kind == "D") {
                asm_[sid].add(vtext::DocRow{static_cast<std::uint32_t>(in.getIntRef(IC_DOCNO)), in.getIntRef(IC_DOC_ID),
                                            static_cast<std::uint32_t>(in.getIntRef(IC_N))});
            } else if (kind == "T") {
                vtext::TermDictRow r;
                r.term_id = static_cast<std::uint32_t>(in.getIntRef(IC_TERM_ID));
                r.term = in.getStringRef(IC_TERM).str();
                r.stem = in.isNull(IC_STEM) ? std::string() : in.getStringRef(IC_STEM).str();
                r.lang = in.isNull(IC_LANG) ? std::string() : in.getStringRef(IC_LANG).str();
                r.doc_freq = static_cast<std::uint32_t>(in.getIntRef(IC_N));
                asm_[sid].add(r);
            } else if (kind == "P" || kind == "Q") {
                vtext::PostingRow r;
                r.term_id = static_cast<std::uint32_t>(in.getIntRef(IC_TERM_ID));
                r.chunk_id = static_cast<std::uint32_t>(in.getIntRef(IC_CHUNK));
                const VString &b = in.getStringRef(IC_BIN);
                (kind == "P" ? r.docs : r.payload).assign(b.data(), b.length());
                asm_[sid].add(std::move(r));
            } else {
                fail("unknown row kind '" + kind + "'");
            }
        } while (in.next());

        std::uint64_t written = 0, bytes = 0;
        if (activate) {
            if (!asm_.empty()) fail("segment rows and gen together: load segments first, then activate");
            g.gen = s.integer("gen", 0);
            g.options = s.text("options", "");
            for (auto &gs : g.segs) {
                auto it = dead.find(gs.segment_id);
                if (it != dead.end()) gs.dead = std::move(it->second);
            }
            for (auto &kv : dead)
                if (!kv.second.empty()) fail("tombstones for segment " + std::to_string(kv.first) + ", which is not in the generation");
            w.activate(g);
        } else {
            for (auto &kv : asm_) {
                auto e = expect.find(kv.first);
                if (e == expect.end()) fail("no summary row for segment " + std::to_string(kv.first));
                if (w.has_segment(kv.first, e->second)) continue;
                const std::string seg = kv.second.finish(kv.first, 0);
                vtext::Segment view(seg.data(), seg.size());
                if (view.header().checksum != e->second)
                    fail("segment " + std::to_string(kv.first) + " assembled from the tables has checksum " +
                         std::to_string(static_cast<std::int64_t>(view.header().checksum)) + ", the summary says " +
                         std::to_string(static_cast<std::int64_t>(e->second)));
                w.write_segment(seg);
                ++written;
                bytes += seg.size();
                if (isCanceled()) fail("canceled");
            }
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
    }
    TransformFunction *createTransformFunction(ServerInterface &srv) override { return vt_createFuncObject<TLoad>(srv.allocator); }
};

} // namespace

RegisterFactory(TLoadFactory);
