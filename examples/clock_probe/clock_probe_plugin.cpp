// Reports the host's disciplined clock once per interval as JSON. host->time
// fills an SpcTimeInfo {utc_ns, mono_ns, est_error_ns, sync_source, locked}
// from the clock every node of the pipeline shares, disciplined by NTP, PTP or
// GPS+PPS when one is configured: the timeline of every .timestamp_ns, distinct
// from an unsynchronized std::chrono::system_clock read.

#include <speculor/plugin.h>
#include <speculor/plugin_helpers.h>

#include <chrono>
#include <cstdio>
#include <new>
#include <string>
#include <thread>

// The parameters, in declaration order: values[P_...] is that parameter's value.
enum Param : uint32_t {
    P_INTERVAL_SEC,
};

SPC_PLUGIN_DESCRIPTOR(
    spc::DescriptorBuilder("clock_probe", "Clock Probe", "Sources/General")
        .author("Speculor").version("0.1.0")
        .help("Reports the host disciplined clock (UTC ns, sync source, lock state) as a JSON record.")
        .maturity(SPC_MATURITY_PREVIEW)
        .tags({"common"})
        .output("clock_out", "Clock", SPC_DATA_JSON)
        .int_param("interval_sec", "Interval (sec)", 1, 60, 1, 1, "Sampling")
            .param_help("Seconds between clock readings")
)

struct ClockProbeState {
    SpcHost* host = nullptr;   // valid until stop() returns
    int32_t interval_sec = 1;

    // The JSON output is the plugin's own: it must stay valid until process()
    // returns, and the engine copies it.
    SpcJson output_json{};
    std::string json_text;
    std::chrono::steady_clock::time_point last_emit{};
};

static ClockProbeState* state(SpcPluginInstance* instance)
{
    return reinterpret_cast<ClockProbeState*>(instance);
}

static SpcPluginInstance* start(SpcHost* host, const SpcValue* values)
{
    auto* s = new (std::nothrow) ClockProbeState{};
    if (!s) return nullptr;
    s->host = host;
    s->interval_sec = values[P_INTERVAL_SEC].i;
    return reinterpret_cast<SpcPluginInstance*>(s);
}

static void stop(SpcPluginInstance* instance)
{
    delete state(instance);
}

static void params_changed(SpcPluginInstance* instance, const SpcValue* values,
                           uint64_t /*changed*/)
{
    state(instance)->interval_sec = values[P_INTERVAL_SEC].i;
}

static const char* sync_name(uint32_t source)
{
    switch (source) {
        case SPC_SYNC_SYSTEM:  return "system";
        case SPC_SYNC_NTP:     return "ntp";
        case SPC_SYNC_PTP:     return "ptp";
        case SPC_SYNC_GPS_PPS: return "gps_pps";
        default:               return "none";
    }
}

static int process(SpcPluginInstance* instance, SpcStep* step)
{
    auto* s = state(instance);
    SpcHost* host = s->host;

    // pace to the configured interval (skip the sleep on the very first step)
    const auto now = std::chrono::steady_clock::now();
    if (s->last_emit.time_since_epoch().count() != 0) {
        const auto interval = std::chrono::seconds(s->interval_sec);
        const auto elapsed  = now - s->last_emit;
        if (elapsed < interval)
            std::this_thread::sleep_for(
                std::chrono::duration_cast<std::chrono::milliseconds>(interval - elapsed));
    }
    s->last_emit = std::chrono::steady_clock::now();

    // both timelines and the clock's quality in one reading; all zero from a
    // host that has no clock to lend
    SpcTimeInfo t{};
    host->time(host, &t);

    char buf[256];
    std::snprintf(buf, sizeof(buf),
        "{\"utc_ns\":%lld,\"mono_ns\":%lld,\"est_error_ns\":%lld,\"sync_source\":\"%s\",\"locked\":%s}",
        static_cast<long long>(t.utc_ns), static_cast<long long>(t.mono_ns),
        static_cast<long long>(t.est_error_ns), sync_name(t.sync_source),
        t.locked ? "true" : "false");

    s->json_text = buf;
    s->output_json.json   = s->json_text.c_str();
    s->output_json.length = static_cast<uint32_t>(s->json_text.size());

    step->outputs[0].type = SPC_DATA_JSON;
    step->outputs[0].json = &s->output_json;
    return SPC_OK;
}

SPC_EXPORT_PLUGIN(
    .info           = info,
    .start          = start,
    .stop           = stop,
    .process        = process,
    .params_changed = params_changed
)
