// Emits an SpcEvent when a scalar input crosses a threshold: a BEGIN event when
// the value rises above `threshold`, and an END event carrying the same id when
// it falls back below. Shows the event stream: filling an SpcEvent, its phases
// (BEGIN/END), severity, a correlation id that pairs a span, and data_json
// extras, handed to host->emit_event. The host puts the event on the session's
// /events stream and in the GUI with no wiring. Chain after scalar_source or
// logic_gate.

#include <speculor/plugin.h>
#include <speculor/plugin_helpers.h>

#include <cstdio>
#include <new>

// The parameters, in declaration order: values[P_...] is that parameter's value.
enum Param : uint32_t {
    P_THRESHOLD,
    P_SEVERITY,
};

SPC_PLUGIN_DESCRIPTOR(
    spc::DescriptorBuilder("threshold_event", "Threshold Event", "Analysis/General")
        .author("Speculor").version("0.1.0")
        .help("Emits a BEGIN/END SpcEvent when a scalar input crosses a threshold.")
        .maturity(SPC_MATURITY_PREVIEW)
        .tags({"events"})
        .input("value_in", "Value", SPC_DATA_SCALAR)
        .float_param("threshold", "Threshold", -1e6f, 1e6f, 0.5f, 0.1f)
            .param_help("A BEGIN event fires when the input rises above this level")
        .enum_param("severity", "Severity",
                    {"Info", "Notice", "Warning", "Critical"}, SPC_EVENT_WARNING, "Events")
            .param_help("Severity carried on the emitted events")
)

struct Params {
    float   threshold;
    int32_t severity;   // SpcEventSeverity: the options are in its order
};

static Params read_params(const SpcValue* values)
{
    Params p;
    p.threshold = values[P_THRESHOLD].f;
    p.severity  = values[P_SEVERITY].choice.index;
    return p;
}

struct ThresholdEventState {
    SpcHost* host = nullptr;   // valid until stop() returns
    Params params{};

    bool     above   = false;  // hysteresis latch: only crossings emit
    uint64_t open_id = 0;      // id of the currently open interval (0 = none)
    uint64_t next_id = 1;      // next correlation id to hand out
};

static ThresholdEventState* state(SpcPluginInstance* instance)
{
    return reinterpret_cast<ThresholdEventState*>(instance);
}

static SpcPluginInstance* start(SpcHost* host, const SpcValue* values)
{
    auto* s = new (std::nothrow) ThresholdEventState{};
    if (!s) return nullptr;
    s->host = host;
    s->params = read_params(values);
    return reinterpret_cast<SpcPluginInstance*>(s);
}

static void stop(SpcPluginInstance* instance)
{
    delete state(instance);
}

static void params_changed(SpcPluginInstance* instance, const SpcValue* values,
                           uint64_t /*changed*/)
{
    state(instance)->params = read_params(values);
}

static int process(SpcPluginInstance* instance, SpcStep* step)
{
    auto* s = state(instance);
    SpcHost* host = s->host;
    const Params& p = s->params;

    // spc::input_scalar is a convenience: input 0 as a float whatever scalar
    // type the producer sends, 0 when the input carries no scalar
    const float value = spc::input_scalar(step->inputs, step->input_count, 0);
    const bool  above = value > p.threshold;
    if (above == s->above) return SPC_OK;   // no crossing this step: nothing to report
    s->above = above;

    SpcEvent e{};
    e.category = SPC_FOURCC('T', 'H', 'R', 'S');
    e.severity = static_cast<uint32_t>(p.severity);
    // timestamp_ns and sequence left 0: the host stamps them with its clock
    // and the sequence of the input being processed

    char detail[SPC_EVENT_DETAIL_MAX];
    char data_json[SPC_EVENT_DATA_MAX];
    if (above) {
        e.phase = SPC_EVENT_BEGIN;
        e.id    = s->open_id = s->next_id++;   // open a span
        e.label = "Above threshold";
        std::snprintf(detail, sizeof(detail), "value %.3f rose above %.3f", value, p.threshold);
    } else {
        e.phase = SPC_EVENT_END;
        e.id    = s->open_id;                  // close the span opened above
        e.label = "Back below threshold";
        std::snprintf(detail, sizeof(detail), "value %.3f fell below %.3f", value, p.threshold);
    }
    std::snprintf(data_json, sizeof(data_json),
                  "{\"value\":%.6f,\"threshold\":%.6f}", value, p.threshold);
    // The host copies the event and its strings during the call, so stack
    // buffers are enough. Sized at SPC_EVENT_*_MAX, a string always fits: the
    // host refuses one that does not, rather than cut it.
    e.detail    = detail;
    e.data_json = data_json;

    if (host->emit_event) host->emit_event(host, &e);
    return SPC_OK;
}

extern "C" {
SPC_API SpcPlugin spc_plugin = {
    .abi            = SPC_ABI,
    .info           = get_descriptor,
    .start          = start,
    .stop           = stop,
    .process        = process,
    .params_changed = params_changed,
};
}
