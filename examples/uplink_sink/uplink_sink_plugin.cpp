#include <speculor/plugin_helpers.h>

// A pure sink that "uplinks" each scalar sample to an external endpoint. The
// transmit here is a stand-in (it just counts + logs), but the descriptor
// carries two flags that matter for correctness in a recorded pipeline:
//
//   .live_only()    -> SPC_PLUGIN_LIVE_ONLY: process() has an irreversible
//                      external effect, so the engine must NOT re-run it during
//                      reinjection replay (a real uplink would double-send).
//   .license_tier() -> gate the node behind a minimum license tier.
//
// Demonstrates SPC_PLUGIN_LIVE_ONLY + license_tier — SDK surface added after
// the examples were last refreshed. A sink has an input and no outputs.

struct Params {
    char endpoint[SPC_PARAM_STRING_MAX] = "udp://localhost:9000";
};

struct UplinkSinkState {
    spc::HostServices host;
    spc::SharedParams<Params> params;
    uint64_t sent = 0;   // worker-only transmit count
};

SPC_PLUGIN(UplinkSinkState, host)

SPC_PLUGIN_DESCRIPTOR(
    spc::DescriptorBuilder("uplink_sink", "Uplink Sink", "Sinks/General")
        .author("Speculor").version("0.1.0")
        .description("Transmits each scalar sample to an external endpoint (stand-in). "
                     "Flagged live-only so replay never re-sends; gated to Personal tier.")
        .maturity(SPC_MATURITY_PREVIEW)
        .tags({"common"})
        .live_only()
        .license_tier(SPC_LICENSE_PERSONAL)
        .input("value_in", "Value", SPC_DATA_SCALAR)
        .string_param("endpoint", "Endpoint", "udp://localhost:9000", "Uplink")
            .param_description("Destination the sample would be transmitted to")
        .streaming()
)

static int set_parameter(SpcPluginInstance* inst, const char* name,
                         const SpcParameterDesc* value)
{
    bool matched = state(inst)->params.update([&](Params& p) {
        return spc::try_set_string(name, value, "endpoint", p.endpoint);
    });
    return matched ? 0 : -1;
}

static int get_parameter(SpcPluginInstance* inst, const char* name,
                         SpcParameterDesc* out)
{
    const Params p = state(inst)->params.snapshot();
    if (spc::try_get_string(name, out, "endpoint", p.endpoint)) return 0;
    return -1;
}

static int start(SpcPluginInstance* inst)
{
    auto* s = state(inst);
    s->sent = 0;
    const Params p = s->params.snapshot();
    SPC_LOG_INFO(&s->host.cached_log, "Uplink Sink started -> %s", p.endpoint);
    return 0;
}

static int stop(SpcPluginInstance* inst)
{
    auto* s = state(inst);
    SPC_LOG_INFO(&s->host.cached_log, "Uplink Sink stopped (%llu samples sent)",
                 static_cast<unsigned long long>(s->sent));
    return 0;
}

static int process(SpcPluginInstance* inst, const SpcData* inputs, uint32_t input_count,
                   SpcData* /*outputs*/, uint32_t /*output_count*/)
{
    auto* s = state(inst);
    const float value = spc::input_scalar(inputs, input_count, 0);

    // A real uplink would transmit `value` to p.endpoint here. Because the node
    // is .live_only(), the engine skips process() during reinjection replay, so
    // that send never happens twice for the same recorded sample.
    if ((++s->sent % 100) == 1) {
        const Params p = s->params.snapshot();
        SPC_LOG_INFO(&s->host.cached_log, "uplink -> %s : value=%.3f (#%llu)",
                     p.endpoint, value, static_cast<unsigned long long>(s->sent));
    }
    return 0;
}

SPC_PLUGIN_VTABLE(
    .get_descriptor    = get_descriptor,
    .create_instance   = create_instance,
    .destroy_instance  = destroy_instance,
    .set_parameter     = set_parameter,
    .get_parameter     = get_parameter,
    .process           = process,
    .start             = start,
    .stop              = stop,
    .set_host_services = set_host_services
)
