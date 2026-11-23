// vfts.tattr(doc_id, value USING PARAMETERS index, name, type, version [, cache_dir] [, remove]) OVER(PARTITION NODES)
//
// Writes the attribute file of an index on this node (attribute.h) from the rows it gets: every
// (doc_id, value) of the attribute, as vfts.refresh_attributes sends them to each node (a local
// join with the replicated copy in vfts.attr_values). Rows with a NULL doc_id or value are skipped
// (the procedure adds one such row per node, so that a node runs even when the table is empty).
// value: INT (types int, bool as 0/1, date as days since 2000-01-01) or VARCHAR (type text).
// remove=true: removes the attribute's file instead (the rows are ignored).
// Output: one row per node: (node_name, status 'loaded' or 'removed', rows).
#include "udx_common.h"
#include "../engine/attribute.h"
#include "../engine/cache.h"

using namespace Vertica;
using namespace vfts_udx;

namespace {

class TAttr : public TransformFunction {
    bool text_;

public:
    explicit TAttr(bool text) : text_(text) {}

    void processPartition(ServerInterface &srv, PartitionReader &in, PartitionWriter &out) override
    {
        try {
            run(srv, in, out);
        } catch (const std::exception &e) {
            vt_report_error(0, "tattr on %s: %s", srv.getCurrentNodeName().c_str(), e.what());
        }
    }

private:
    void run(ServerInterface &srv, PartitionReader &in, PartitionWriter &out)
    {
        Settings s(srv);
        const std::string index = read_index_name(s);
        const std::string name = s.text("name", "");
        if (!vfts::valid_index_name(name)) fail("parameter name: 1 to 64 letters, digits or underscores");
        vfts::CacheWriter w(resolve_cache_dir(s), index);
        std::uint64_t rows = 0;
        if (s.boolean("remove", false)) {
            w.remove_attribute(name);
            vfts::forget_attributes(resolve_cache_dir(s), index);
        } else {
            vfts::AttrType type;
            if (!vfts::parse_attr_type(s.text("type", ""), type)) fail("parameter type must be int, bool, date or text");
            if ((type == vfts::AttrType::Text) != text_) fail("type " + s.text("type", "") + " does not fit the value column");
            const std::int64_t version = s.integer("version", 0);
            if (version < 1) fail("parameter version must be at least 1");
            std::vector<std::pair<std::int64_t, std::int64_t>> ints;
            std::vector<std::pair<std::int64_t, std::string>> texts;
            do {
                if (in.isNull(0) || in.isNull(1)) continue;
                if (++rows % 1000000 == 0 && isCanceled()) fail("canceled");
                if (text_) texts.emplace_back(in.getIntRef(0), in.getStringRef(1).str());
                else ints.emplace_back(in.getIntRef(0), in.getIntRef(1));
            } while (in.next());
            w.write_attribute(name, text_ ? vfts::encode_attribute_text(version, std::move(texts))
                                          : vfts::encode_attribute(type, version, std::move(ints)));
        }
        out.getStringRef(0).copy(srv.getCurrentNodeName());
        out.getStringRef(1).copy(s.boolean("remove", false) ? "removed" : "loaded");
        out.setInt(2, static_cast<vint>(rows));
        out.next();
    }
};

class TAttrFactory : public TransformFunctionFactory {
    bool text_;

public:
    explicit TAttrFactory(bool text) : text_(text) {}
    void getPrototype(ServerInterface &, ColumnTypes &a, ColumnTypes &r) override
    {
        a.addInt();
        if (text_) a.addVarchar(); else a.addInt();
        r.addVarchar();
        r.addVarchar();
        r.addInt();
    }
    void getReturnType(ServerInterface &, const SizedColumnTypes &, SizedColumnTypes &out) override
    {
        out.addVarchar(128, "node_name");
        out.addVarchar(16, "status");
        out.addInt("rows");
    }
    void getParameterType(ServerInterface &, SizedColumnTypes &p) override
    {
        p.addVarchar(128, "index");
        p.addVarchar(64, "name");
        p.addVarchar(8, "type");
        p.addInt("version");
        p.addBool("remove");
        p.addVarchar(1024, "cache_dir");
    }
    TransformFunction *createTransformFunction(ServerInterface &srv) override { return vt_createFuncObject<TAttr>(srv.allocator, text_); }
};

class TAttrIntFactory : public TAttrFactory { public: TAttrIntFactory() : TAttrFactory(false) {} };
class TAttrTextFactory : public TAttrFactory { public: TAttrTextFactory() : TAttrFactory(true) {} };

} // namespace

RegisterFactory(TAttrIntFactory);
RegisterFactory(TAttrTextFactory);
