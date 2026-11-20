// Outputs a constant scalar, set by its one parameter. The smallest plugin:
// each function below fills a slot of SpcPlugin (speculor/plugin.h), and
// SPC_EXPORT_PLUGIN at the bottom defines spc_plugin, all the library exports.

#include <speculor/plugin.h>          // the boundary: every type and call that crosses it
#include <speculor/plugin_helpers.h>  // conveniences compiled into the plugin

#include <new>

// The parameters in the order the node info declares them. start() and
// params_changed() get the values as one array in this order, so a value's
// position is its identity.
enum Param : uint32_t {
    P_VALUE,
};

// The info slot. SPC_PLUGIN_DESCRIPTOR defines info(), which builds
// the SpcNodeInfo on the first call and keeps it, and all it points at, while
// the library is loaded. DescriptorBuilder is a convenience for filling it.
SPC_PLUGIN_DESCRIPTOR(
    spc::DescriptorBuilder("scalar_source", "Scalar Source", "Sources/General")
        .run_on_change()
        .author("Speculor").version("0.1.0")
        .help("Outputs a constant scalar value")
        .maturity(SPC_MATURITY_STABLE)
        .tags({"common"})
        .float_param("value", "Value", -1e6f, 1e6f, 0.0f, 0.1f)   // P_VALUE
            .param_help("Constant scalar value to output each frame")
        .output("scalar_out", "Scalar", SPC_DATA_SCALAR)
)

// An instance exists only while its node runs. The engine holds it as an
// opaque SpcPluginInstance* and calls it one call at a time, so it needs no
// lock.
struct ScalarSourceState {
    float value = 0.0f;
};

static ScalarSourceState* state(SpcPluginInstance* instance)
{
    return reinterpret_cast<ScalarSourceState*>(instance);
}

// `values` are the host's, valid during the call. `host` stays valid until
// stop() returns; this plugin makes no host call, so it does not keep it.
// Null means the node does not run.
static SpcPluginInstance* start(SpcHost* /*host*/, const SpcValue* values)
{
    auto* s = new (std::nothrow) ScalarSourceState{};
    if (!s) return nullptr;
    s->value = values[P_VALUE].f;   // a value is in the member its declared type names: FLOAT is .f
    return reinterpret_cast<SpcPluginInstance*>(s);
}

// Nothing is called on the instance after this.
static void stop(SpcPluginInstance* instance)
{
    delete state(instance);
}

// Every value again, with bit i of `changed` set for each parameter that
// differs from what this instance last had.
static void params_changed(SpcPluginInstance* instance, const SpcValue* values,
                           uint64_t /*changed*/)
{
    state(instance)->value = values[P_VALUE].f;
}

// The step holds one SpcData per declared input and output, in port order.
static int process(SpcPluginInstance* instance, SpcStep* step)
{
    SpcData& out = step->outputs[0];
    out.type = SPC_DATA_SCALAR;
    out.scalar = {};   // timestamp_ns and sequence 0: the engine stamps them
    out.scalar.type = SPC_FIELD_FLOAT;
    out.scalar.f32 = state(instance)->value;
    return SPC_OK;
}

// spc_plugin, the one symbol the engine looks up, with its `abi` set to
// SPC_ABI: the engine loads the plugin only when that equals its own. A slot
// left out is null, a call the plugin does without.
SPC_EXPORT_PLUGIN(
    .info           = info,
    .start          = start,
    .stop           = stop,
    .process        = process,
    .params_changed = params_changed
)
