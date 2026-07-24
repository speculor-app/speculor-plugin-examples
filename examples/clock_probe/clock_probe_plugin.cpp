#include <speculor/plugin_helpers.h>
#include <speculor_common/spc_clock.h>   // spc::clock:: — host disciplined clock

#include <chrono>
#include <cstdio>
#include <string>
#include <thread>

// Reports the host's disciplined nanosecond clock once per interval as a JSON
// RECORD. Demonstrates the clock ABI (SDK 0.14): spc::clock::get_time() returns
// an SpcTimeInfo {utc_ns, mono_ns, est_error_ns, sync_source, locked} read from
// the host's disciplined clock — NTP / PTP / GPS-PPS aware — and degrades to
// std::chrono on hosts that don't expose it. This is the timeline the whole
// pipeline shares for .timestamp_ns, distinct from an unsynchronized
// std::chrono::system_clock read.

struct Params {
    int32_t interval_sec = 1;
};

struct ClockProbeState {
    spc::HostServices host;
    spc::SharedParams<Params> params;

    SpcRecord   output_record{};
    std::string record_json;
    std::chrono::steady_clock::time_point last_emit{};
};

SPC_PLUGIN(ClockProbeState, host)

SPC_PLUGIN_DESCRIPTOR(
    spc::DescriptorBuilder("clock_probe", "Clock Probe", "Sources/General")
        .author("Speculor").version("0.1.0")
        .description("Reports the host disciplined clock (UTC ns, sync source, lock state) as a JSON record.")
        .maturity(SPC_MATURITY_PREVIEW)
        .tags({"common"})
        .output("clock_out", "Clock", SPC_DATA_RECORD)
        .int_param("interval_sec", "Interval (sec)", 1, 60, 1, 1, "Sampling")
            .param_description("Seconds between clock readings")
        .streaming()
)

static int set_parameter(SpcPluginInstance* inst, const char* name,
                         const SpcParameterDesc* value)
{
    bool matched = state(inst)->params.update([&](Params& p) {
        return spc::try_set_int(name, value, "interval_sec", p.interval_sec);
    });
    return matched ? 0 : -1;
}

static int get_parameter(SpcPluginInstance* inst, const char* name,
                         SpcParameterDesc* out)
{
    const Params p = state(inst)->params.snapshot();
    if (spc::try_get_int(name, out, "interval_sec", p.interval_sec)) return 0;
    return -1;
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

static int process(SpcPluginInstance* inst, const SpcData* /*inputs*/, uint32_t /*input_count*/,
                   SpcData* outputs, uint32_t output_count)
{
    auto* s = state(inst);
    if (output_count < 1) return -1;
    const Params p = s->params.snapshot();

    // pace to the configured interval (skip the sleep on the very first frame)
    const auto now = std::chrono::steady_clock::now();
    if (s->last_emit.time_since_epoch().count() != 0) {
        const auto interval = std::chrono::seconds(p.interval_sec);
        const auto elapsed  = now - s->last_emit;
        if (elapsed < interval)
            std::this_thread::sleep_for(
                std::chrono::duration_cast<std::chrono::milliseconds>(interval - elapsed));
    }
    s->last_emit = std::chrono::steady_clock::now();

    const SpcTimeInfo t = spc::clock::get_time(s->host);

    char buf[256];
    std::snprintf(buf, sizeof(buf),
        "{\"utc_ns\":%lld,\"mono_ns\":%lld,\"est_error_ns\":%lld,\"sync_source\":\"%s\",\"locked\":%s}",
        static_cast<long long>(t.utc_ns), static_cast<long long>(t.mono_ns),
        static_cast<long long>(t.est_error_ns), sync_name(t.sync_source),
        t.locked ? "true" : "false");

    s->record_json = buf;
    s->output_record.json   = s->record_json.c_str();
    s->output_record.length = static_cast<uint32_t>(s->record_json.size());

    outputs[0].type   = SPC_DATA_RECORD;
    outputs[0].record = &s->output_record;
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
