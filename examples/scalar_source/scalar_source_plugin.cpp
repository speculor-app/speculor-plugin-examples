#include <speculor/plugin_helpers.h>

// GUI-thread-set parameters, snapshotted on the worker (H6)
struct Params {
    float value = 0.0f;
};

struct ScalarSourceState {
    spc::HostServices host;
    spc::SharedParams<Params> params;
};

SPC_PLUGIN(ScalarSourceState, host)

SPC_PLUGIN_DESCRIPTOR(
    spc::DescriptorBuilder("scalar_source", "Scalar Source", "Sources/General")
        .static_source()
        .author("Speculor").version("0.1.0")
        .description("Outputs a constant scalar value")
        .maturity(SPC_MATURITY_STABLE)
        .tags({"common"})
        .float_param("value", "Value", -1e6f, 1e6f, 0.0f, 0.1f)
            .param_description("Constant scalar value to output each frame")
        .output("scalar_out", "Scalar", SPC_DATA_SCALAR)
)

static int set_parameter(SpcPluginInstance* inst, const char* name,
                         const SpcParameterDesc* value) {
    bool matched = state(inst)->params.update([&](Params& p) {
        return spc::try_set_float(name, value, "value", p.value);
    });
    return matched ? 0 : -1;
}

static int get_parameter(SpcPluginInstance* inst, const char* name,
                         SpcParameterDesc* out) {
    const Params p = state(inst)->params.snapshot();
    if (spc::try_get_float(name, out, "value", p.value)) return 0;
    return -1;
}

static int process(SpcPluginInstance* inst, const SpcData* /*inputs*/,
                   uint32_t /*input_count*/, SpcData* outputs,
                   uint32_t output_count) {
    if (output_count < 1) return -1;
    spc::set_scalar_output(outputs[0], state(inst)->params.snapshot().value);
    return 0;
}

SPC_PLUGIN_VTABLE(
    .get_descriptor    = get_descriptor,
    .create_instance   = create_instance,
    .destroy_instance  = destroy_instance,
    .set_parameter     = set_parameter,
    .get_parameter     = get_parameter,
    .process           = process,
    .set_host_services = set_host_services
)
