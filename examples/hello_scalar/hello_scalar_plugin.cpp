// hello_scalar — Simplest possible Speculor plugin.
// Outputs a user-configurable scalar value on every tick.
//
// Demonstrates:
//   - Plugin state struct with HostServices
//   - SPC_PLUGIN macro (generates create/destroy/state/set_host_services)
//   - DescriptorBuilder for metadata, parameters, and ports
//   - SPC_PLUGIN_AUTO_PARAMS for zero-boilerplate parameter handling
//   - spc::set_scalar_output for typed output
//   - SPC_DECLARE_PLUGIN_FILTER for the vtable export

#include <speculor/plugin_helpers.h>

// every plugin has a state struct — this is your instance data
struct HelloState {
    spc::HostServices host;   // required: engine injects logging + frame allocation
    float value = 42.0f;      // our one parameter
};

// generates: state(), create_instance(), destroy_instance(), set_host_services()
SPC_PLUGIN(HelloState, host)

// generates: get_descriptor() returning a static descriptor
SPC_PLUGIN_DESCRIPTOR(
    spc::DescriptorBuilder("hello_scalar", "Hello Scalar", "Examples")
        .author("Speculor").version("1.0.0")
        .description("Outputs a constant scalar value — the simplest possible plugin")
        .output("value", "Value", SPC_DATA_SCALAR)
        .float_param("value", "Value", -1e6f, 1e6f, 42.0f, 0.1f)
        .build()
)

// generates: set_parameter() and get_parameter() by binding struct fields
SPC_PLUGIN_AUTO_PARAMS(HelloState,
    SPC_BIND_FLOAT(HelloState, "value", value)
)

// the engine calls this on every tick — read inputs, write outputs
static int process(SpcPluginInstance* inst, const SpcData* /*inputs*/,
                   uint32_t /*input_count*/, SpcData* outputs,
                   uint32_t output_count) {
    if (output_count < 1) return -1;
    spc::set_scalar_output(outputs[0], state(inst)->value);
    return 0;
}

// export the vtable — this is what the engine loads
SPC_DECLARE_PLUGIN_FILTER(get_descriptor, create_instance, destroy_instance,
                          set_parameter, get_parameter, process)
