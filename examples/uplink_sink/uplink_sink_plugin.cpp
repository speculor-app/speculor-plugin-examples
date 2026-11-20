// A pure sink that "uplinks" each scalar sample to an external endpoint. The
// transmit here is a stand-in (it counts and logs), but the node info carries
// two declarations that matter in a recorded pipeline:
//
//   .live_only()    -> SPC_NODE_LIVE_ONLY: process() has an irreversible effect
//                      outside the pipeline, so the engine skips the node
//                      during reinjection replay (a real uplink would send twice).
//   .license_tier() -> the minimum license tier that may use the node.
//
// A sink has an input and no outputs.

#include <speculor/plugin.h>
#include <speculor/plugin_helpers.h>

#include <new>

// The parameters, in declaration order: values[P_...] is that parameter's value.
enum Param : uint32_t {
    P_ENDPOINT,
};

SPC_PLUGIN_DESCRIPTOR(
    spc::DescriptorBuilder("uplink_sink", "Uplink Sink", "Sinks/General")
        .author("Speculor").version("0.1.0")
        .help("Transmits each scalar sample to an external endpoint (stand-in). "
              "Flagged live-only so replay never re-sends; gated to Personal tier.")
        .maturity(SPC_MATURITY_PREVIEW)
        .tags({"common"})
        .live_only()
        .license_tier(SPC_LICENSE_PERSONAL)
        .input("value_in", "Value", SPC_DATA_SCALAR)
        .string_param("endpoint", "Endpoint", "udp://localhost:9000", "Uplink")
            .param_help("Destination the sample would be transmitted to")
)

struct Params {
    char endpoint[SPC_PARAM_STRING_MAX];
};

static Params read_params(const SpcValue* values)
{
    Params p;
    // a STRING value lives only during the call that hands it over: copy it
    spc::copy_str(p.endpoint, sizeof(p.endpoint), values[P_ENDPOINT].str.ptr);
    return p;
}

struct UplinkSinkState {
    SpcLogContext log{};   // the host's log; the host stays valid until stop() returns
    Params params{};
    uint64_t sent = 0;
};

static UplinkSinkState* state(SpcPluginInstance* instance)
{
    return reinterpret_cast<UplinkSinkState*>(instance);
}

static SpcPluginInstance* start(SpcHost* host, const SpcValue* values)
{
    auto* s = new (std::nothrow) UplinkSinkState{};
    if (!s) return nullptr;
    s->log = spc::log_context(host);   // a convenience over host->log, for the SPC_LOG_* macros
    s->params = read_params(values);
    SPC_LOG_INFO(&s->log, "Uplink Sink started -> %s", s->params.endpoint);
    return reinterpret_cast<SpcPluginInstance*>(s);
}

static void stop(SpcPluginInstance* instance)
{
    auto* s = state(instance);
    SPC_LOG_INFO(&s->log, "Uplink Sink stopped (%llu samples sent)",
                 static_cast<unsigned long long>(s->sent));
    delete s;
}

static void params_changed(SpcPluginInstance* instance, const SpcValue* values,
                           uint64_t /*changed*/)
{
    state(instance)->params = read_params(values);
}

static int process(SpcPluginInstance* instance, SpcStep* step)
{
    auto* s = state(instance);
    // spc::input_scalar is a convenience: input 0 as a float whatever scalar
    // type the producer sends, 0 when the input carries no scalar
    const float value = spc::input_scalar(step->inputs, step->input_count, 0);

    // A real uplink would transmit `value` to the endpoint here. The node is
    // live-only, so the engine skips process() during reinjection replay and
    // that send never happens twice for the same recorded sample.
    if ((++s->sent % 100) == 1) {
        SPC_LOG_INFO(&s->log, "uplink -> %s : value=%.3f (#%llu)",
                     s->params.endpoint, value, static_cast<unsigned long long>(s->sent));
    }
    return SPC_OK;
}

SPC_EXPORT_PLUGIN(
    .info           = info,
    .start          = start,
    .stop           = stop,
    .process        = process,
    .params_changed = params_changed
)
