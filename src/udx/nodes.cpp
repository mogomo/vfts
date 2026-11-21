// Functions that run once per node, over vtext.probe with OVER(PARTITION NODES):
//   vtext.tnode(k)        the smallest probe key stored on the node: lets the procedures pick one
//                         probe row per node (sql/procedures.sql)
//   vtext.tinfo()         the node's cache of an index: generation, segments, documents, bytes
//   vtext.tdrop()         removes the node's cache of an index
#include "udx_common.h"

using namespace Vertica;
using namespace vtext_udx;

namespace {

class TNode : public TransformFunction {
public:
    void processPartition(ServerInterface &srv, PartitionReader &in, PartitionWriter &out) override
    {
        bool any = false;
        vint m = 0;
        do {
            if (in.isNull(0)) continue;
            const vint k = in.getIntRef(0);
            if (!any || k < m) m = k;
            any = true;
        } while (in.next());
        if (!any) return;
        out.getStringRef(0).copy(srv.getCurrentNodeName());
        out.setInt(1, m);
        out.next();
    }
};

class TNodeFactory : public TransformFunctionFactory {
public:
    void getPrototype(ServerInterface &, ColumnTypes &a, ColumnTypes &r) override { a.addInt(); r.addVarchar(); r.addInt(); }
    void getReturnType(ServerInterface &, const SizedColumnTypes &, SizedColumnTypes &out) override
    {
        out.addVarchar(128, "node_name");
        out.addInt("k");
    }
    TransformFunction *createTransformFunction(ServerInterface &srv) override { return vt_createFuncObject<TNode>(srv.allocator); }
};

class TInfo : public TransformFunction {
public:
    void processPartition(ServerInterface &srv, PartitionReader &, PartitionWriter &out) override
    {
        try {
            Settings s(srv);
            const std::string index = read_index_name(s);
            const vtext::CacheInfo ci = vtext::cache_info(resolve_cache_dir(s), index);
            out.getStringRef(0).copy(srv.getCurrentNodeName());
            if (ci.present) {
                out.setInt(1, ci.gen);
                out.setInt(2, static_cast<vint>(ci.segments));
                out.setInt(3, static_cast<vint>(ci.docs));
                out.setInt(4, static_cast<vint>(ci.dead));
                out.setInt(5, static_cast<vint>(ci.bytes));
                out.setNull(6);
            } else {
                for (int c = 1; c <= 5; ++c) out.setNull(c);
                out.getStringRef(6).copy(ci.error.substr(0, 1000));
            }
            out.next();
        } catch (const std::exception &e) {
            vt_report_error(0, "tinfo: %s", e.what());
        }
    }
};

class TInfoFactory : public TransformFunctionFactory {
public:
    void getPrototype(ServerInterface &, ColumnTypes &, ColumnTypes &r) override
    {
        r.addVarchar(); r.addInt(); r.addInt(); r.addInt(); r.addInt(); r.addInt(); r.addVarchar();
    }
    void getReturnType(ServerInterface &, const SizedColumnTypes &, SizedColumnTypes &out) override
    {
        out.addVarchar(128, "node_name");
        out.addInt("gen");
        out.addInt("segments");
        out.addInt("docs");
        out.addInt("deleted");
        out.addInt("bytes");
        out.addVarchar(1000, "problem");
    }
    void getParameterType(ServerInterface &, SizedColumnTypes &p) override
    {
        p.addVarchar(128, "index");
        p.addVarchar(1024, "cache_dir");
    }
    TransformFunction *createTransformFunction(ServerInterface &srv) override { return vt_createFuncObject<TInfo>(srv.allocator); }
};

// vtext.thave(USING PARAMETERS index, segments='id:checksum,...') OVER(PARTITION NODES) FROM vtext.probe
// The listed segments that this node's cache holds (node_name, segment_id).
class THave : public TransformFunction {
public:
    void processPartition(ServerInterface &srv, PartitionReader &, PartitionWriter &out) override
    {
        try {
            Settings s(srv);
            vtext::CacheWriter w(resolve_cache_dir(s), read_index_name(s));
            const std::string list = s.text("segments", "");
            std::size_t i = 0;
            while (i < list.size()) {
                std::size_t j = list.find(',', i);
                if (j == std::string::npos) j = list.size();
                const std::string item = list.substr(i, j - i);
                const std::size_t colon = item.find(':');
                if (colon != std::string::npos) {
                    const std::int64_t id = std::stoll(item.substr(0, colon));
                    const std::uint64_t sum = static_cast<std::uint64_t>(std::stoll(item.substr(colon + 1)));
                    if (w.has_segment(id, sum)) {
                        out.getStringRef(0).copy(srv.getCurrentNodeName());
                        out.setInt(1, id);
                        out.next();
                    }
                }
                i = j + 1;
            }
        } catch (const std::exception &e) {
            vt_report_error(0, "thave: %s", e.what());
        }
    }
};

class THaveFactory : public TransformFunctionFactory {
public:
    void getPrototype(ServerInterface &, ColumnTypes &, ColumnTypes &r) override { r.addVarchar(); r.addInt(); }
    void getReturnType(ServerInterface &, const SizedColumnTypes &, SizedColumnTypes &out) override
    {
        out.addVarchar(128, "node_name");
        out.addInt("segment_id");
    }
    void getParameterType(ServerInterface &, SizedColumnTypes &p) override
    {
        p.addVarchar(128, "index");
        p.addVarchar(1024, "cache_dir");
        p.addVarchar(65000, "segments");
    }
    TransformFunction *createTransformFunction(ServerInterface &srv) override { return vt_createFuncObject<THave>(srv.allocator); }
};

class TDrop : public TransformFunction {
public:
    void processPartition(ServerInterface &srv, PartitionReader &, PartitionWriter &out) override
    {
        try {
            Settings s(srv);
            vtext::drop_cache(resolve_cache_dir(s), read_index_name(s));
            out.getStringRef(0).copy(srv.getCurrentNodeName());
            out.next();
        } catch (const std::exception &e) {
            vt_report_error(0, "tdrop: %s", e.what());
        }
    }
};

class TDropFactory : public TransformFunctionFactory {
public:
    void getPrototype(ServerInterface &, ColumnTypes &, ColumnTypes &r) override { r.addVarchar(); }
    void getReturnType(ServerInterface &, const SizedColumnTypes &, SizedColumnTypes &out) override { out.addVarchar(128, "node_name"); }
    void getParameterType(ServerInterface &, SizedColumnTypes &p) override
    {
        p.addVarchar(128, "index");
        p.addVarchar(1024, "cache_dir");
    }
    TransformFunction *createTransformFunction(ServerInterface &srv) override { return vt_createFuncObject<TDrop>(srv.allocator); }
};

} // namespace

RegisterFactory(TNodeFactory);
RegisterFactory(TInfoFactory);
RegisterFactory(TDropFactory);
RegisterFactory(THaveFactory);
