// Measurement probes (not installed by sql/install.sql; bench/plan_c/shapes.sh creates them):
// scalar functions that return their argument, with each volatility, to measure the cost of a
// statement that calls a scalar UDx (and whether the planner folds a STABLE one with constant
// arguments into a constant).
#include "udx_common.h"

using namespace Vertica;

namespace {

class TProbe : public ScalarFunction {
public:
    void processBlock(ServerInterface &, BlockReader &in, BlockWriter &out) override
    {
        do {
            out.setInt(in.getIntRef(0) + 1);
            out.next();
        } while (in.next());
    }
};

template <volatility V> class TProbeFactory : public ScalarFunctionFactory {
public:
    TProbeFactory() { vol = V; }
    void getPrototype(ServerInterface &, ColumnTypes &a, ColumnTypes &r) override { a.addInt(); r.addInt(); }
    ScalarFunction *createScalarFunction(ServerInterface &srv) override { return vt_createFuncObject<TProbe>(srv.allocator); }
};

class TProbeVolatileFactory : public TProbeFactory<VOLATILE> {};
class TProbeStableFactory : public TProbeFactory<STABLE> {};
class TProbeImmutableFactory : public TProbeFactory<IMMUTABLE> {};

} // namespace

RegisterFactory(TProbeVolatileFactory);
RegisterFactory(TProbeStableFactory);
RegisterFactory(TProbeImmutableFactory);
