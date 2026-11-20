// vtext.tbuild(part, doc_id, body, lang, ver USING PARAMETERS seg_base, lang, accent_fold,
//              on_unsupported, threads, max_segment_mb) OVER(PARTITION BY part)
//
// Builds segments from documents and returns them as index rows (udx_common.h), which the
// refresh procedure stores in the index tables. Every instance (one per partition) runs `threads`
// worker threads; each owns a segment builder, so an instance writes up to `threads` segments,
// and more when a builder grows past max_segment_mb. Segment ids: seg_base + part * 4096 + n.
//
// The calling thread reads the rows and writes the result; worker threads never call the SDK.
#include "udx_common.h"
#include "../engine/builder.h"
#include "../engine/table_codec.h"

#include <condition_variable>
#include <deque>
#include <mutex>
#include <thread>

using namespace Vertica;
using namespace vtext_udx;

namespace {

struct DocIn {
    std::int64_t doc_id;
    std::string text;
    vtext::Lang lang;
    std::int64_t ver;
};

class TBuild : public TransformFunction {
    vtext::AnalyzerOptions aopts_;
    std::int64_t seg_base_ = 0;
    int threads_ = 4;
    std::size_t max_bytes_ = 0;

public:
    void setup(ServerInterface &srv, const SizedColumnTypes &) override
    {
        try {
            Settings s(srv);
            aopts_ = analyzer_options(s);
            if (!s.has("seg_base")) fail("parameter seg_base is required");
            seg_base_ = s.integer("seg_base", 0);
            threads_ = static_cast<int>(s.integer("threads", 4));
            if (threads_ < 1 || threads_ > 64) fail("threads must be 1 to 64");
            const std::int64_t mb = s.integer("max_segment_mb", 2048);
            if (mb < 1) fail("max_segment_mb must be at least 1");
            max_bytes_ = static_cast<std::size_t>(mb) << 20;
        } catch (const std::exception &e) {
            vt_report_error(0, "tbuild: %s", e.what());
        }
    }

    void processPartition(ServerInterface &srv, PartitionReader &in, PartitionWriter &out) override
    {
        try {
            run(srv, in, out);
        } catch (const std::exception &e) {
            vt_report_error(0, "tbuild: %s", e.what());
        }
    }

private:
    void run(ServerInterface &srv, PartitionReader &in, PartitionWriter &out)
    {
        std::mutex mu;
        std::condition_variable cv_work, cv_room;
        std::deque<std::vector<DocIn>> queue;
        bool done = false;
        std::exception_ptr error;
        std::vector<std::string> segments;
        std::int64_t part = -1;
        std::uint32_t next_local = 0;

        auto next_id = [&]() {
            std::lock_guard<std::mutex> l(mu);
            if (next_local >= 4096) throw std::runtime_error("more than 4096 segments in one partition: raise max_segment_mb");
            return seg_base_ + part * 4096 + next_local++;
        };

        auto worker = [&]() {
            try {
                vtext::SegmentBuilder b(aopts_);
                for (;;) {
                    std::vector<DocIn> batch;
                    {
                        std::unique_lock<std::mutex> l(mu);
                        cv_work.wait(l, [&] { return !queue.empty() || done || error; });
                        if (error) return;
                        if (queue.empty()) break;
                        batch = std::move(queue.front());
                        queue.pop_front();
                    }
                    cv_room.notify_one();
                    for (DocIn &d : batch) {
                        try {
                            b.add(d.doc_id, d.text, d.lang, 0);     // versions are kept in vtext.segments, not in the file
                        } catch (const std::exception &e) {
                            throw std::runtime_error("doc_id " + std::to_string(d.doc_id) + ": " + e.what());
                        }
                        if (b.memory() > max_bytes_) {
                            std::string seg = b.finish(next_id());
                            std::lock_guard<std::mutex> l(mu);
                            segments.push_back(std::move(seg));
                        }
                    }
                }
                if (b.docs() > 0) {
                    std::string seg = b.finish(next_id());
                    std::lock_guard<std::mutex> l(mu);
                    segments.push_back(std::move(seg));
                }
            } catch (...) {
                std::lock_guard<std::mutex> l(mu);
                if (!error) error = std::current_exception();
                cv_work.notify_all();
                cv_room.notify_all();
            }
        };

        // Read the rows on this thread; hand batches of documents to the workers.
        std::vector<std::thread> pool;
        std::vector<DocIn> batch;
        std::size_t batch_bytes = 0;
        auto push = [&]() {
            if (batch.empty()) return;
            std::unique_lock<std::mutex> l(mu);
            cv_room.wait(l, [&] { return queue.size() < static_cast<std::size_t>(threads_) * 4 || error; });
            if (error) std::rethrow_exception(error);
            queue.push_back(std::move(batch));
            batch.clear();
            batch_bytes = 0;
            cv_work.notify_one();
        };
        try {
            do {
                if (in.isNull(1)) continue;
                if (part < 0) {
                    part = in.isNull(0) ? 0 : in.getIntRef(0);
                    if (part < 0 || part > 100000) fail("part must be 0 to 100000");
                    for (int t = 0; t < threads_; ++t) pool.emplace_back(worker);
                }
                DocIn d;
                d.doc_id = in.getIntRef(1);
                if (!in.isNull(2)) { const VString &v = in.getStringRef(2); d.text.assign(v.data(), v.length()); }
                d.lang = in.isNull(3) ? vtext::LANG_NONE : vtext::parse_lang(in.getStringRef(3).str());
                d.ver = in.isNull(4) ? 0 : in.getIntRef(4);
                batch_bytes += d.text.size() + 64;
                batch.push_back(std::move(d));
                if (batch.size() >= 512 || batch_bytes >= (4u << 20)) push();
                if (isCanceled()) fail("canceled");
            } while (in.next());
            push();
        } catch (...) {
            {
                std::lock_guard<std::mutex> l(mu);
                done = true;
                if (!error) error = std::current_exception();
            }
            cv_work.notify_all();
            cv_room.notify_all();
            for (auto &t : pool) t.join();
            throw;
        }
        {
            std::lock_guard<std::mutex> l(mu);
            done = true;
        }
        cv_work.notify_all();
        for (auto &t : pool) t.join();
        if (error) std::rethrow_exception(error);

        // Write every segment as index rows (this thread only).
        for (std::string &bytes : segments) {
            const vtext::Segment seg(bytes.data(), bytes.size());
            const std::int64_t sid = seg.id();
            vtext::RowSink sink;
            sink.doc = [&](const vtext::DocRow &r) {
                begin(out, "D", sid);
                out.setInt(IC_DOCNO, r.docno);
                out.setInt(IC_DOC_ID, r.doc_id);
                out.setInt(IC_N, r.length);
                out.next();
            };
            sink.term = [&](const vtext::TermDictRow &r) {
                begin(out, "T", sid);
                out.setInt(IC_TERM_ID, r.term_id);
                out.setInt(IC_N, r.doc_freq);
                out.getStringRef(IC_TERM).copy(r.term.data(), r.term.size());
                out.getStringRef(IC_STEM).copy(r.stem.data(), r.stem.size());
                out.getStringRef(IC_LANG).copy(r.lang.data(), r.lang.size());
                out.next();
            };
            sink.posting = [&](const vtext::PostingRow &r) {
                begin(out, "P", sid);
                out.setInt(IC_TERM_ID, r.term_id);
                out.setInt(IC_CHUNK, r.chunk_id);
                out.getStringRef(IC_BIN).copy(r.docs.data(), r.docs.size());
                out.next();
                begin(out, "Q", sid);
                out.setInt(IC_TERM_ID, r.term_id);
                out.setInt(IC_CHUNK, r.chunk_id);
                if (r.payload.size() > 32000000) fail("positions of one term and chunk exceed 32 MB");
                out.getStringRef(IC_BIN).copy(r.payload.data(), r.payload.size());
                out.next();
            };
            vtext::segment_to_rows(seg, sink);
            const vtext::SegmentHeader &h = seg.header();
            begin(out, "S", sid);
            out.setInt(IC_N, h.n_docs);
            out.setInt(IC_TERM_ID, h.n_terms);
            out.setInt(IC_DOCNO, static_cast<vint>(h.sum_len));
            out.setInt(IC_CHUNK, static_cast<vint>(h.total_bytes));
            out.setInt(IC_DOC_ID, static_cast<vint>(h.checksum));
            out.next();
            std::string().swap(bytes);
        }
    }

    // Starts a row: kind and segment set, every other column NULL.
    static void begin(PartitionWriter &out, const char *kind, std::int64_t sid)
    {
        out.getStringRef(IC_KIND).copy(kind);
        out.setInt(IC_SEGMENT, sid);
        for (int c : {IC_TERM_ID, IC_CHUNK, IC_DOCNO, IC_DOC_ID, IC_N}) out.setNull(c);
        for (int c : {IC_TERM, IC_STEM, IC_LANG, IC_BIN}) out.setNull(c);
    }
};

class TBuildFactory : public TransformFunctionFactory {
public:
    void getPrototype(ServerInterface &, ColumnTypes &argTypes, ColumnTypes &returnType) override
    {
        argTypes.addInt();              // part
        argTypes.addInt();              // doc_id
        argTypes.addLongVarchar();      // body
        argTypes.addVarchar();          // lang
        argTypes.addInt();              // ver, microseconds
        add_index_row_types(returnType);
    }
    void getReturnType(ServerInterface &, const SizedColumnTypes &, SizedColumnTypes &out) override { add_index_row_columns(out); }
    void getParameterType(ServerInterface &, SizedColumnTypes &p) override
    {
        add_analyzer_parameters(p);
        p.addInt("seg_base");
        p.addInt("threads");
        p.addInt("max_segment_mb");
    }
    TransformFunction *createTransformFunction(ServerInterface &srv) override { return vt_createFuncObject<TBuild>(srv.allocator); }
};

} // namespace

RegisterFactory(TBuildFactory);
