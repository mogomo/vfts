// vfts.tversion() OVER(): library version, build flags, ICU version and the Snowball stemmers
// linked in (sb_stemmer_list()), one row.
#include "udx_common.h"
#include "../engine/stemmer.h"
#include "../engine/version.h"

using namespace Vertica;
using namespace vfts_udx;

namespace {

class TVersion : public TransformFunction {
public:
    void processPartition(ServerInterface &, PartitionReader &, PartitionWriter &out) override
    {
        try {
            std::string stemmers;
            for (const std::string &s : vfts::snowball_algorithms()) stemmers += (stemmers.empty() ? "" : " ") + s;
            out.getStringRef(0).copy(vfts::LIBRARY_VERSION);
            out.getStringRef(1).copy(VFTS_BUILD_FLAGS);
            out.getStringRef(2).copy(vfts::icu_version());
            out.getStringRef(3).copy(stemmers);
            out.next();
        } catch (const std::exception &e) {
            vt_report_error(0, "tversion: %s", e.what());
        }
    }
};

class TVersionFactory : public TransformFunctionFactory {
public:
    void getPrototype(ServerInterface &, ColumnTypes &, ColumnTypes &returnType) override
    {
        returnType.addVarchar();
        returnType.addVarchar();
        returnType.addVarchar();
        returnType.addVarchar();
    }
    void getReturnType(ServerInterface &, const SizedColumnTypes &, SizedColumnTypes &out) override
    {
        out.addVarchar(64, "version");
        out.addVarchar(256, "build");
        out.addVarchar(32, "icu");
        out.addVarchar(2048, "snowball");
    }
    TransformFunction *createTransformFunction(ServerInterface &srv) override
    {
        return vt_createFuncObject<TVersion>(srv.allocator);
    }
};

} // namespace

RegisterFactory(TVersionFactory);
