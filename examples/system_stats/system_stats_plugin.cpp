// Samples system statistics once per interval: a one-row TABLE of metrics, and
// a JSON output with the machine's static description.

#include "system_stats_state.h"

#include <new>
#include <thread>

#ifdef _WIN32
static constexpr const char* SPC_DEFAULT_DISK_PATH = "C:\\";
#else
static constexpr const char* SPC_DEFAULT_DISK_PATH = "/";
#endif

// The parameters, in declaration order: values[P_...] is that parameter's value.
enum Param : uint32_t {
    P_UPDATE_INTERVAL_SEC,
    P_DISK_PATH,
};

// the output ports, in declaration order
enum Output : uint32_t {
    OUT_STATS,
    OUT_INFO,
};

SPC_PLUGIN_DESCRIPTOR(
    spc::DescriptorBuilder("system_stats", "System Stats", "Sources/General")
        .author("Speculor").version("0.1.0")
        .help("Collects cross-platform system statistics: CPU, memory, disk, network, process info")
        .maturity(SPC_MATURITY_EXPERIMENTAL)
        .tags({"common"})
        .output_table("stats_out", "System Stats", {
            {"cpu_usage_pct",    SPC_FIELD_FLOAT},
            {"cpu_core_count",   SPC_FIELD_UINT32},
            {"cpu_thread_count", SPC_FIELD_UINT32},
            {"mem_total_mb",     SPC_FIELD_UINT32},
            {"mem_used_mb",      SPC_FIELD_UINT32},
            {"mem_available_mb", SPC_FIELD_UINT32},
            {"mem_usage_pct",    SPC_FIELD_FLOAT},
            {"swap_total_mb",    SPC_FIELD_UINT32},
            {"swap_used_mb",     SPC_FIELD_UINT32},
            {"disk_total_gb",    SPC_FIELD_FLOAT},
            {"disk_used_gb",     SPC_FIELD_FLOAT},
            {"disk_free_gb",     SPC_FIELD_FLOAT},
            {"disk_usage_pct",   SPC_FIELD_FLOAT},
            {"net_bytes_sent",   SPC_FIELD_UINT32},
            {"net_bytes_recv",   SPC_FIELD_UINT32},
            {"net_connected",    SPC_FIELD_BOOL},
            {"proc_cpu_pct",     SPC_FIELD_FLOAT},
            {"proc_mem_mb",      SPC_FIELD_UINT32},
            {"uptime_sec",       SPC_FIELD_UINT32},
            {"cpu_freq_mhz",     SPC_FIELD_UINT32},
        })
        .output("info_out", "System Info", SPC_DATA_JSON)
        .int_param("update_interval_sec", "Update Interval (sec)", 1, 60, 1, 1, "Sampling")
            .param_help("Seconds between system statistics samples")
        .string_param("disk_path", "Disk Path", SPC_DEFAULT_DISK_PATH, "Disk")
            .param_help("Filesystem path to monitor for disk usage")
)

static Params read_params(const SpcValue* values)
{
    Params p;
    p.update_interval_sec = values[P_UPDATE_INTERVAL_SEC].i;
    // a STRING value lives only during the call that hands it over: copy it
    spc::copy_str(p.disk_path, sizeof(p.disk_path), values[P_DISK_PATH].str.ptr);
    return p;
}

static SystemStatsState* state(SpcPluginInstance* instance)
{
    return reinterpret_cast<SystemStatsState*>(instance);
}

static SpcPluginInstance* start(SpcHost* host, const SpcValue* values)
{
    auto* s = new (std::nothrow) SystemStatsState{};
    if (!s) return nullptr;
    s->host = host;
    s->log = spc::log_context(host);   // a convenience over host->log, for the SPC_LOG_* macros
    s->params = read_params(values);

    // collect static info once
    collect_static_info(s);

    // prime cpu usage deltas (first reading is always inaccurate)
    sample_cpu_usage(s);
    sample_proc_cpu(s);

    // prime network deltas
    auto net = sample_network();
    s->prev_net_sent = net.sent;
    s->prev_net_recv = net.recv;

    s->last_emit = std::chrono::steady_clock::now();

    SPC_LOG_INFO(&s->log, "System Stats started (interval: %d sec, disk: %s)",
                 s->params.update_interval_sec, s->params.disk_path);
    return reinterpret_cast<SpcPluginInstance*>(s);
}

static void stop(SpcPluginInstance* instance)
{
    auto* s = state(instance);
    SPC_LOG_INFO(&s->log, "System Stats stopped (%llu frames emitted)",
                 static_cast<unsigned long long>(s->sequence));
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
    SpcHost* host = s->host;
    const Params& p = s->params;

    // pacing: sleep until next interval
    auto now = std::chrono::steady_clock::now();
    auto interval = std::chrono::seconds(p.update_interval_sec);
    auto elapsed = now - s->last_emit;

    if (elapsed < interval) {
        auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(interval - elapsed);
        std::this_thread::sleep_for(remaining);
        now = std::chrono::steady_clock::now();
    }
    s->last_emit = now;

    // --- sample all metrics ---
    float cpu_pct = sample_cpu_usage(s);
    auto mem = sample_memory();
    auto disk = sample_disk(p.disk_path);
    auto net = sample_network();
    float proc_cpu = sample_proc_cpu(s);
    uint32_t proc_mem = sample_proc_mem_mb();
    uint32_t uptime = sample_uptime_sec();

    // compute network deltas
    uint32_t net_sent_delta = static_cast<uint32_t>(net.sent - s->prev_net_sent);
    uint32_t net_recv_delta = static_cast<uint32_t>(net.recv - s->prev_net_recv);
    s->prev_net_sent = net.sent;
    s->prev_net_recv = net.recv;

    // --- one row, in a table the host lends until process() returns ---
    SpcTable* out = host->table(host, OUT_STATS, 1);
    if (!out) return SPC_FAILED;
    // where each field of the port is in a record, in declaration order
    const uint32_t* off = out->field_offsets;

    // spc_table_set_* (table_helpers.h) write a field at its offset in record 0
    spc_table_set_float(out,  0, off[F_CPU_USAGE_PCT],    cpu_pct);
    spc_table_set_uint32(out, 0, off[F_CPU_CORE_COUNT],   s->core_count);
    spc_table_set_uint32(out, 0, off[F_CPU_THREAD_COUNT], s->thread_count);
    spc_table_set_uint32(out, 0, off[F_MEM_TOTAL_MB],     mem.total_mb);
    spc_table_set_uint32(out, 0, off[F_MEM_USED_MB],      mem.used_mb);
    spc_table_set_uint32(out, 0, off[F_MEM_AVAILABLE_MB], mem.available_mb);
    spc_table_set_float(out,  0, off[F_MEM_USAGE_PCT],    mem.usage_pct);
    spc_table_set_uint32(out, 0, off[F_SWAP_TOTAL_MB],    mem.swap_total_mb);
    spc_table_set_uint32(out, 0, off[F_SWAP_USED_MB],     mem.swap_used_mb);
    spc_table_set_float(out,  0, off[F_DISK_TOTAL_GB],    disk.total_gb);
    spc_table_set_float(out,  0, off[F_DISK_USED_GB],     disk.used_gb);
    spc_table_set_float(out,  0, off[F_DISK_FREE_GB],     disk.free_gb);
    spc_table_set_float(out,  0, off[F_DISK_USAGE_PCT],   disk.usage_pct);
    spc_table_set_uint32(out, 0, off[F_NET_BYTES_SENT],   net_sent_delta);
    spc_table_set_uint32(out, 0, off[F_NET_BYTES_RECV],   net_recv_delta);
    spc_table_set_bool(out,   0, off[F_NET_CONNECTED],    net.connected ? 1 : 0);
    spc_table_set_float(out,  0, off[F_PROC_CPU_PCT],     proc_cpu);
    spc_table_set_uint32(out, 0, off[F_PROC_MEM_MB],      proc_mem);
    spc_table_set_uint32(out, 0, off[F_UPTIME_SEC],       uptime);
    spc_table_set_uint32(out, 0, off[F_CPU_FREQ_MHZ],     s->cpu_freq_mhz);

    out->sequence = s->sequence++;
    out->timestamp_ns = host->now_utc_ns(host);   // the engine's UTC clock

    step->outputs[OUT_STATS].type = SPC_DATA_TABLE;
    step->outputs[OUT_STATS].table = out;

    // --- build the JSON output ---
    // escape backslashes in disk_path for JSON
    std::string escaped_disk;
    for (const char* dp = p.disk_path; *dp; ++dp) {
        if (*dp == '\\') escaped_disk += "\\\\";
        else escaped_disk += *dp;
    }

    s->json_text = "{";
    s->json_text += "\"cpu_name\":\"" + std::string(s->cpu_name) + "\"";
    s->json_text += ",\"os_name\":\"" + std::string(s->os_name) + "\"";
    s->json_text += ",\"os_version\":\"" + std::string(s->os_version) + "\"";
    s->json_text += ",\"hostname\":\"" + std::string(s->hostname) + "\"";
    s->json_text += ",\"net_interface\":\"" + std::string(s->net_interface) + "\"";
    s->json_text += ",\"net_type\":\"" + std::string(s->net_type) + "\"";
    s->json_text += ",\"disk_path\":\"" + escaped_disk + "\"";
    s->json_text += "}";

    s->output_json.json = s->json_text.c_str();
    s->output_json.length = static_cast<uint32_t>(s->json_text.size());

    step->outputs[OUT_INFO].type = SPC_DATA_JSON;
    step->outputs[OUT_INFO].json = &s->output_json;

    return SPC_OK;
}

SPC_EXPORT_PLUGIN(
    .info           = info,
    .start          = start,
    .stop           = stop,
    .process        = process,
    .params_changed = params_changed
)
