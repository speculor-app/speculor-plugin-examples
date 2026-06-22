#include "system_stats_state.h"

#include <thread>

SPC_PLUGIN_CAST(SystemStatsState)
SPC_PLUGIN_HOST_SERVICES(SystemStatsState, host)

static int set_parameter(SpcPluginInstance* inst, const char* name,
                         const SpcParameterDesc* value)
{
    bool matched = state(inst)->params.update([&](Params& p) {
        return spc::try_set_int   (name, value, "update_interval_sec", p.update_interval_sec)
            || spc::try_set_string(name, value, "disk_path", p.disk_path);
    });
    return matched ? 0 : -1;
}

static int get_parameter(SpcPluginInstance* inst, const char* name,
                         SpcParameterDesc* out)
{
    const Params p = state(inst)->params.snapshot();
    if (spc::try_get_int   (name, out, "update_interval_sec", p.update_interval_sec)) return 0;
    if (spc::try_get_string(name, out, "disk_path", p.disk_path)) return 0;
    return -1;
}

#ifdef _WIN32
static constexpr const char* SPC_DEFAULT_DISK_PATH = "C:\\";
#else
static constexpr const char* SPC_DEFAULT_DISK_PATH = "/";
#endif

// ============================================================================
// plugin descriptor
// ============================================================================

SPC_PLUGIN_DESCRIPTOR(
    spc::DescriptorBuilder("system_stats", "System Stats", "Sources/General")
        .author("Speculor").version("0.1.0")
        .description("Collects cross-platform system statistics: CPU, memory, disk, network, process info")
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
        .output("info_out", "System Info", SPC_DATA_RECORD)
        .int_param("update_interval_sec", "Update Interval (sec)", 1, 60, 1, 1, "Sampling")
            .param_description("Seconds between system statistics samples")
        .string_param("disk_path", "Disk Path", SPC_DEFAULT_DISK_PATH, "Disk")
            .param_description("Filesystem path to monitor for disk usage")
        .streaming()
)

// ============================================================================
// lifecycle
// ============================================================================

static SpcPluginInstance* create_instance()
{
    auto* s = new SystemStatsState{};

    auto* desc = get_descriptor();
    spc_schema_compute_offsets(&desc->ports[0].schema, s->offsets, &s->stride);
    spc_table_init(&s->output_table, s->stride, &desc->ports[0].schema);

    return reinterpret_cast<SpcPluginInstance*>(s);
}

static void destroy_instance(SpcPluginInstance* inst)
{
    auto* s = state(inst);
    spc_table_free(&s->output_table);
    delete s;
}

static int start(SpcPluginInstance* inst)
{
    auto* s = state(inst);

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
    s->frame_number = 0;

    const Params p = s->params.snapshot();
    SPC_LOG_INFO(&s->host.cached_log, "System Stats started (interval: %d sec, disk: %s)",
                 p.update_interval_sec, p.disk_path);
    return 0;
}

static int stop(SpcPluginInstance* inst)
{
    auto* s = state(inst);
    SPC_LOG_INFO(&s->host.cached_log, "System Stats stopped (%llu frames emitted)",
                 static_cast<unsigned long long>(s->frame_number));
    return 0;
}

// ============================================================================
// process
// ============================================================================

static int process(SpcPluginInstance* inst, const SpcData* /*inputs*/, uint32_t /*input_count*/,
                   SpcData* outputs, uint32_t output_count)
{
    auto* s = state(inst);
    if (output_count < 2) return -1;

    const Params p = s->params.snapshot();  // one consistent view per frame

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

    // --- fill table (single row) ---
    if (spc_table_resize(&s->output_table, 1) != 0) return -1;

    spc_table_set_float(&s->output_table,  0, s->offsets[F_CPU_USAGE_PCT],    cpu_pct);
    spc_table_set_uint32(&s->output_table, 0, s->offsets[F_CPU_CORE_COUNT],   s->core_count);
    spc_table_set_uint32(&s->output_table, 0, s->offsets[F_CPU_THREAD_COUNT], s->thread_count);
    spc_table_set_uint32(&s->output_table, 0, s->offsets[F_MEM_TOTAL_MB],     mem.total_mb);
    spc_table_set_uint32(&s->output_table, 0, s->offsets[F_MEM_USED_MB],      mem.used_mb);
    spc_table_set_uint32(&s->output_table, 0, s->offsets[F_MEM_AVAILABLE_MB], mem.available_mb);
    spc_table_set_float(&s->output_table,  0, s->offsets[F_MEM_USAGE_PCT],    mem.usage_pct);
    spc_table_set_uint32(&s->output_table, 0, s->offsets[F_SWAP_TOTAL_MB],    mem.swap_total_mb);
    spc_table_set_uint32(&s->output_table, 0, s->offsets[F_SWAP_USED_MB],     mem.swap_used_mb);
    spc_table_set_float(&s->output_table,  0, s->offsets[F_DISK_TOTAL_GB],    disk.total_gb);
    spc_table_set_float(&s->output_table,  0, s->offsets[F_DISK_USED_GB],     disk.used_gb);
    spc_table_set_float(&s->output_table,  0, s->offsets[F_DISK_FREE_GB],     disk.free_gb);
    spc_table_set_float(&s->output_table,  0, s->offsets[F_DISK_USAGE_PCT],   disk.usage_pct);
    spc_table_set_uint32(&s->output_table, 0, s->offsets[F_NET_BYTES_SENT],   net_sent_delta);
    spc_table_set_uint32(&s->output_table, 0, s->offsets[F_NET_BYTES_RECV],   net_recv_delta);
    spc_table_set_bool(&s->output_table,   0, s->offsets[F_NET_CONNECTED],    net.connected ? 1 : 0);
    spc_table_set_float(&s->output_table,  0, s->offsets[F_PROC_CPU_PCT],     proc_cpu);
    spc_table_set_uint32(&s->output_table, 0, s->offsets[F_PROC_MEM_MB],      proc_mem);
    spc_table_set_uint32(&s->output_table, 0, s->offsets[F_UPTIME_SEC],       uptime);
    spc_table_set_uint32(&s->output_table, 0, s->offsets[F_CPU_FREQ_MHZ],     s->cpu_freq_mhz);

    s->output_table.frame_number = s->frame_number++;
    s->output_table.timestamp_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(
        now.time_since_epoch()).count();

    outputs[0].type = SPC_DATA_TABLE;
    outputs[0].table = &s->output_table;

    // --- build JSON record ---
    // escape backslashes in disk_path for JSON
    std::string escaped_disk;
    for (const char* dp = p.disk_path; *dp; ++dp) {
        if (*dp == '\\') escaped_disk += "\\\\";
        else escaped_disk += *dp;
    }

    s->record_json = "{";
    s->record_json += "\"cpu_name\":\"" + std::string(s->cpu_name) + "\"";
    s->record_json += ",\"os_name\":\"" + std::string(s->os_name) + "\"";
    s->record_json += ",\"os_version\":\"" + std::string(s->os_version) + "\"";
    s->record_json += ",\"hostname\":\"" + std::string(s->hostname) + "\"";
    s->record_json += ",\"net_interface\":\"" + std::string(s->net_interface) + "\"";
    s->record_json += ",\"net_type\":\"" + std::string(s->net_type) + "\"";
    s->record_json += ",\"disk_path\":\"" + escaped_disk + "\"";
    s->record_json += "}";

    s->output_record.json = s->record_json.c_str();
    s->output_record.length = static_cast<uint32_t>(s->record_json.size());

    outputs[1].type = SPC_DATA_RECORD;
    outputs[1].record = &s->output_record;

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
