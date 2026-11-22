// vtext.tmerge(USING PARAMETERS index, gen, segments, new_id [, reorder, cache_dir]) OVER()
//
// Merges segments of the active generation (from this node's cache) into one new segment and
// returns it as index rows (udx_common.h). Tombstoned documents are dropped. reorder (default
// true): number the documents by recursive graph bisection (src/engine/reorder.h). Called by
// vtext.merge (sql/procedures.sql) on the initiator node.
#include "udx_common.h"
#include "../engine/merge.h"

#include <set>
#include <sstream>

using namespace Vertica;
using namespace vtext_udx;

namespace {

class TMerge : public TransformFunction {
public:
    void processPartition(ServerInterface &srv, PartitionReader &, PartitionWriter &out) override
    {
        try {
            Settings s(srv);
            const std::string index = read_index_name(s);
            if (!s.has("gen") || !s.has("segments") || !s.has("new_id")) fail("parameters gen, segments and new_id are required");
            auto li = vtext::open_index(resolve_cache_dir(s), index, s.integer("gen", 0));
            if (li->gen != s.integer("gen", 0)) fail("the cache on " + srv.getCurrentNodeName() + " has generation " + std::to_string(li->gen) + ", not " + std::to_string(s.integer("gen", 0)));
            std::set<std::int64_t> want;
            std::istringstream list(s.text("segments", ""));
            std::string item;
            while (std::getline(list, item, ',')) if (!item.empty()) want.insert(std::stoll(item));
            std::vector<vtext::SegmentRef> inputs;
            for (const vtext::SegmentRef &r : li->refs) if (want.count(r.seg->id())) inputs.push_back(r);
            if (inputs.size() != want.size()) fail("some of the segments " + s.text("segments", "") + " are not in the active generation");
            vtext::MergeOptions mo;
            mo.reorder = s.boolean("reorder", true);
            const std::string merged = vtext::merge_segments(inputs, s.integer("new_id", 0), mo);
            vtext::CacheWriter(resolve_cache_dir(s), index).write_segment(merged);     // this node has it now
            write_segment_rows(out, merged);
        } catch (const std::exception &e) {
            vt_report_error(0, "tmerge: %s", e.what());
        }
    }
};

class TMergeFactory : public TransformFunctionFactory {
public:
    void getPrototype(ServerInterface &, ColumnTypes &, ColumnTypes &r) override { add_index_row_types(r); }
    void getReturnType(ServerInterface &, const SizedColumnTypes &, SizedColumnTypes &out) override { add_index_row_columns(out); }
    void getParameterType(ServerInterface &, SizedColumnTypes &p) override
    {
        p.addVarchar(128, "index");
        p.addVarchar(1024, "cache_dir");
        p.addInt("gen");
        p.addVarchar(65000, "segments");
        p.addInt("new_id");
        p.addBool("reorder");
    }
    TransformFunction *createTransformFunction(ServerInterface &srv) override { return vt_createFuncObject<TMerge>(srv.allocator); }
};

} // namespace

RegisterFactory(TMergeFactory);
