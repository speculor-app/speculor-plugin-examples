#pragma once

#include <speculor/plugin_helpers.h>
#include <speculor/table_helpers.h>

#include <chrono>
#include <cstring>
#include <string>

// output schema field indices
enum {
    F_CPU_USAGE_PCT = 0,
    F_CPU_CORE_COUNT,
    F_CPU_THREAD_COUNT,
    F_MEM_TOTAL_MB,
    F_MEM_USED_MB,
    F_MEM_AVAILABLE_MB,
    F_MEM_USAGE_PCT,
    F_SWAP_TOTAL_MB,
    F_SWAP_USED_MB,
    F_DISK_TOTAL_GB,
    F_DISK_USED_GB,
    F_DISK_FREE_GB,
    F_DISK_USAGE_PCT,
    F_NET_BYTES_SENT,
    F_NET_BYTES_RECV,
    F_NET_CONNECTED,
    F_PROC_CPU_PCT,
    F_PROC_MEM_MB,
    F_UPTIME_SEC,
    F_CPU_FREQ_MHZ,
    FIELD_COUNT
};

struct SystemStatsState
{
    spc::HostServices host;

    // outputs
    SpcTable output_table{};
    uint32_t offsets[FIELD_COUNT]{};
    uint32_t stride{};
    SpcRecord output_record{};
    std::string record_json;

    // parameters
    int32_t update_interval_sec = 1;
#ifdef _WIN32
    char disk_path[SPC_PARAM_STRING_MAX] = "C:\\";
#else
    char disk_path[SPC_PARAM_STRING_MAX] = "/";
#endif

    // static info (read once in start())
    char cpu_name[128]{};
    char os_name[128]{};
    char os_version[64]{};
    char hostname[64]{};
    uint32_t core_count = 0;
    uint32_t thread_count = 0;
    uint32_t cpu_freq_mhz = 0;

    // cpu usage tracking (delta between ticks)
    uint64_t prev_cpu_idle = 0;
    uint64_t prev_cpu_total = 0;

    // network tracking (delta between ticks)
    uint64_t prev_net_sent = 0;
    uint64_t prev_net_recv = 0;
    char net_interface[64]{};
    char net_type[16]{};  // "ethernet", "wifi", "unknown"

    // process cpu tracking
    uint64_t prev_proc_kernel = 0;
    uint64_t prev_proc_user = 0;
    uint64_t prev_proc_time = 0;

    // pacing
    std::chrono::steady_clock::time_point last_emit;
    uint64_t frame_number = 0;
};

// sampling result types
struct MemInfo {
    uint32_t total_mb = 0;
    uint32_t used_mb = 0;
    uint32_t available_mb = 0;
    float usage_pct = 0.0f;
    uint32_t swap_total_mb = 0;
    uint32_t swap_used_mb = 0;
};

struct DiskInfo {
    float total_gb = 0.0f;
    float used_gb = 0.0f;
    float free_gb = 0.0f;
    float usage_pct = 0.0f;
};

struct NetBytes {
    uint64_t sent = 0;
    uint64_t recv = 0;
    bool connected = false;
};

// sampling functions (implemented in system_stats_sampling.cpp)
void collect_static_info(SystemStatsState* s);
float sample_cpu_usage(SystemStatsState* s);
MemInfo sample_memory();
DiskInfo sample_disk(const char* path);
NetBytes sample_network();
float sample_proc_cpu(SystemStatsState* s);
uint32_t sample_proc_mem_mb();
uint32_t sample_uptime_sec();
