#include <speculor/plugin_helpers.h>

#include <cstdio>

// Emits an SpcEvent when a scalar input crosses a threshold: a BEGIN event when
// the value rises above `threshold`, and an END event carrying the same id when
// it falls back below. Demonstrates the event subsystem (SDK 0.20, the
// SpcMarker -> SpcEvent rename): filling an SpcEvent, phases (BEGIN/END),
// severity, correlation ids that pair a span, and data_json extras — then
// host.emit_event(). The host copies the event onto its /events stream and
// delivers it to the GUI with no wiring. Chain after scalar_source or logic_gate.

// GUI-thread-set parameters, snapshotted on the worker (H6)
struct Params {
    float   threshold = 0.5f;
    int32_t severity  = SPC_EVENT_WARNING;   // SpcEventSeverity
};

struct ThresholdEventState {
    spc::HostServices host;
    spc::SharedParams<Params> params;

    // worker-only runtime state
    bool     above   = false;   // hysteresis latch — only crossings emit
    uint64_t open_id = 0;       // id of the currently-open interval (0 = none)
    uint64_t next_id = 1;       // next correlation id to hand out
};

SPC_PLUGIN(ThresholdEventState, host)

SPC_PLUGIN_DESCRIPTOR(
    spc::DescriptorBuilder("threshold_event", "Threshold Event", "Analysis/General")
        .author("Speculor").version("0.1.0")
        .description("Emits a BEGIN/END SpcEvent when a scalar input crosses a threshold.")
        .maturity(SPC_MATURITY_PREVIEW)
        .tags({"events"})
        .input("value_in", "Value", SPC_DATA_SCALAR)
        .float_param("threshold", "Threshold", -1e6f, 1e6f, 0.5f, 0.1f)
            .param_description("A BEGIN event fires when the input rises above this level")
        .enum_param("severity", "Severity",
                    {"Info", "Notice", "Warning", "Critical"}, SPC_EVENT_WARNING, "Events")
            .param_description("Severity carried on the emitted events")
        .streaming()
)

static int set_parameter(SpcPluginInstance* inst, const char* name,
                         const SpcParameterDesc* value)
{
    bool matched = state(inst)->params.update([&](Params& p) {
        return spc::try_set_float(name, value, "threshold", p.threshold)
            || spc::try_set_enum (name, value, "severity", p.severity);
    });
    return matched ? 0 : -1;
}

static int get_parameter(SpcPluginInstance* inst, const char* name,
                         SpcParameterDesc* out)
{
    const Params p = state(inst)->params.snapshot();
    if (spc::try_get_float(name, out, "threshold", p.threshold)) return 0;
    if (spc::try_get_enum (name, out, "severity", p.severity)) return 0;
    return -1;
}

static int process(SpcPluginInstance* inst, const SpcData* inputs, uint32_t input_count,
                   SpcData* /*outputs*/, uint32_t /*output_count*/)
{
    auto* s = state(inst);
    const Params p = s->params.snapshot();

    const float value = spc::input_scalar(inputs, input_count, 0);
    const bool  above = value > p.threshold;
    if (above == s->above) return 0;   // no crossing this frame — nothing to report
    s->above = above;

    SpcEvent e{};
    e.struct_size = sizeof(e);
    e.category    = SPC_FOURCC('T', 'H', 'R', 'S');
    e.severity    = static_cast<uint32_t>(p.severity);
    // timestamp_ns left 0 -> the host stamps the event with its disciplined clock

    if (above) {
        e.phase = SPC_EVENT_BEGIN;
        e.id    = s->open_id = s->next_id++;   // open a span
        std::snprintf(e.label,  sizeof(e.label),  "Above threshold");
        std::snprintf(e.detail, sizeof(e.detail), "value %.3f rose above %.3f", value, p.threshold);
    } else {
        e.phase = SPC_EVENT_END;
        e.id    = s->open_id;                  // close the span opened above
        std::snprintf(e.label,  sizeof(e.label),  "Back below threshold");
        std::snprintf(e.detail, sizeof(e.detail), "value %.3f fell below %.3f", value, p.threshold);
    }
    std::snprintf(e.data_json, sizeof(e.data_json),
                  "{\"value\":%.6f,\"threshold\":%.6f}", value, p.threshold);

    s->host.emit_event(e);
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
