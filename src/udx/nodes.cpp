// Functions that run once per node, over vfts.probe with OVER(PARTITION NODES):
//   vfts.tnode(k)        the smallest probe key stored on the node: lets the procedures pick one
//                         probe row per node (sql/procedures.sql)
//   vfts.tinfo()         the node's cache of an index: generation, segments, documents, bytes
//   vfts.tdrop()         removes the node's cache of an index
//   vfts.tcaches()       the index caches in the node's cache directory: list them, remove those
//                         of indexes that no longer exist, or take over those of vfts 0.4's /tmp/vfts
#include "udx_common.h"

#include <map>
#include <set>

using namespace Vertica;
using namespace vfts_udx;

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
            const vfts::CacheInfo ci = vfts::cache_info(resolve_cache_dir(s), index);
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

// vfts.thave(USING PARAMETERS index, segments='id:checksum,...') OVER(PARTITION NODES) FROM vfts.probe
// The listed segments that this node's cache holds (node_name, segment_id).
class THave : public TransformFunction {
public:
    void processPartition(ServerInterface &srv, PartitionReader &, PartitionWriter &out) override
    {
        try {
            Settings s(srv);
            vfts::CacheWriter w(resolve_cache_dir(s), read_index_name(s));
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
            vfts::drop_cache(resolve_cache_dir(s), read_index_name(s));
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

// vfts.tcaches(USING PARAMETERS action='list'|'clean'|'adopt' [, keep='name:id,...'] [, cache_dir])
//   OVER(PARTITION NODES) FROM vfts.probe
// One row per index cache in the node's cache directory: (node_name, cache_dir, index_name,
// index_id, gen, bytes, action). clean: removes every cache whose index is not in keep (a name
// with its index id; a cache written by vfts 0.4 has no id and is kept when its name is);
// action says 'removed' or 'kept'. adopt: moves the caches of /tmp/vfts (vfts 0.4 and older) into
// the cache directory, action 'adopted'.
class TCaches : public TransformFunction {
public:
    void processPartition(ServerInterface &srv, PartitionReader &, PartitionWriter &out) override
    {
        try {
            Settings s(srv);
            const std::string dir = resolve_cache_dir(s), action = s.text("action", "list");
            if (!one_of(action, {"list", "clean", "adopt"})) fail("action must be list, clean or adopt");
            if (dir.empty() || dir[0] != '/') fail("cache_dir must be an absolute path");
            std::set<std::string> adopted;
            if (action == "adopt")
                for (const std::string &n : vfts::adopt_caches(vfts::DEFAULT_CACHE_DIR, dir)) adopted.insert(n);
            std::map<std::string, std::int64_t> keep;
            const std::string list = s.text("keep", "");
            for (std::size_t i = 0; i < list.size();) {
                std::size_t j = list.find(',', i);
                if (j == std::string::npos) j = list.size();
                const std::string item = list.substr(i, j - i);
                const std::size_t colon = item.find(':');
                if (colon != std::string::npos) keep[item.substr(0, colon)] = std::stoll(item.substr(colon + 1));
                i = j + 1;
            }
            for (const vfts::CacheDirEntry &c : vfts::list_caches(dir)) {
                std::string what = adopted.count(c.index) ? "adopted" : "";
                if (action == "clean") {
                    auto k = keep.find(c.index);
                    const bool live = k != keep.end() && (c.index_id < 0 || c.index_id == k->second);
                    if (!live) vfts::drop_cache(dir, c.index);
                    what = live ? "kept" : "removed";
                }
                out.getStringRef(0).copy(srv.getCurrentNodeName());
                out.getStringRef(1).copy(dir);
                out.getStringRef(2).copy(c.index);
                if (c.index_id >= 0) out.setInt(3, c.index_id); else out.setNull(3);
                if (c.gen >= 0) out.setInt(4, c.gen); else out.setNull(4);
                out.setInt(5, static_cast<vint>(c.bytes));
                out.getStringRef(6).copy(what);
                out.next();
            }
        } catch (const std::exception &e) {
            vt_report_error(0, "tcaches on %s: %s", srv.getCurrentNodeName().c_str(), e.what());
        }
    }
};

class TCachesFactory : public TransformFunctionFactory {
public:
    void getPrototype(ServerInterface &, ColumnTypes &, ColumnTypes &r) override
    {
        r.addVarchar(); r.addVarchar(); r.addVarchar(); r.addInt(); r.addInt(); r.addInt(); r.addVarchar();
    }
    void getReturnType(ServerInterface &, const SizedColumnTypes &, SizedColumnTypes &out) override
    {
        out.addVarchar(128, "node_name");
        out.addVarchar(1024, "cache_dir");
        out.addVarchar(64, "index_name");
        out.addInt("index_id");
        out.addInt("gen");
        out.addInt("bytes");
        out.addVarchar(16, "action");
    }
    void getParameterType(ServerInterface &, SizedColumnTypes &p) override
    {
        p.addVarchar(16, "action");
        p.addVarchar(65000, "keep");
        p.addVarchar(1024, "cache_dir");
    }
    TransformFunction *createTransformFunction(ServerInterface &srv) override { return vt_createFuncObject<TCaches>(srv.allocator); }
};

} // namespace

RegisterFactory(TCachesFactory);
RegisterFactory(TNodeFactory);
RegisterFactory(TInfoFactory);
RegisterFactory(TDropFactory);
RegisterFactory(THaveFactory);
