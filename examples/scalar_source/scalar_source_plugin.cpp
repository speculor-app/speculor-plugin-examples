#include <speculor/plugin_helpers.h>

struct ScalarSourceState {
    spc::HostServices host;
    float value = 0.0f;
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

SPC_PLUGIN_AUTO_PARAMS(ScalarSourceState,
    SPC_BIND_FLOAT(ScalarSourceState, "value", value)
)

static int process(SpcPluginInstance* inst, const SpcData* /*inputs*/,
                   uint32_t /*input_count*/, SpcData* outputs,
                   uint32_t output_count) {
    if (output_count < 1) return -1;
    spc::set_scalar_output(outputs[0], state(inst)->value);
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
