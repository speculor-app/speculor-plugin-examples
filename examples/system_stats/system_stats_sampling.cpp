#include "system_stats_state.h"

#include <cstring>
#include <format>

// platform headers
#ifdef _WIN32
    #define WIN32_LEAN_AND_MEAN
    #define NOMINMAX
    #include <winsock2.h>
    #include <ws2def.h>
    #include <windows.h>
    #include <psapi.h>
    #include <iphlpapi.h>
    #pragma comment(lib, "iphlpapi.lib")
    #pragma comment(lib, "psapi.lib")
#elif defined(__linux__)
    #include <unistd.h>
    #include <sys/statvfs.h>
    #include <sys/sysinfo.h>
    #include <fstream>
    #include <sstream>
#elif defined(__APPLE__)
    #include <unistd.h>
    #include <sys/statvfs.h>
    #include <sys/sysctl.h>
    #include <sys/types.h>
    #include <mach/mach.h>
    #include <mach/host_info.h>
    #include <mach/mach_host.h>
    #include <net/if.h>
    #include <ifaddrs.h>
    #include <net/if_dl.h>
    #include <net/route.h>
#endif

// ============================================================================
// platform: static info helpers (called once in start())
// ============================================================================

static void collect_cpu_info(SystemStatsState* s)
{
#ifdef _WIN32
    // cpu name + freq from registry
    {
        HKEY key{};
        if (RegOpenKeyExA(HKEY_LOCAL_MACHINE,
                          "HARDWARE\\DESCRIPTION\\System\\CentralProcessor\\0",
                          0, KEY_READ, &key) == ERROR_SUCCESS) {
            DWORD size = sizeof(s->cpu_name);
            RegQueryValueExA(key, "ProcessorNameString", nullptr, nullptr,
                             reinterpret_cast<LPBYTE>(s->cpu_name), &size);
            DWORD mhz = 0;
            size = sizeof(mhz);
            if (RegQueryValueExA(key, "~MHz", nullptr, nullptr,
                                 reinterpret_cast<LPBYTE>(&mhz), &size) == ERROR_SUCCESS) {
                s->cpu_freq_mhz = mhz;
            }
            RegCloseKey(key);
        }
    }

    // core/thread count
    {
        SYSTEM_INFO si{};
        GetSystemInfo(&si);
        s->thread_count = static_cast<uint8_t>(si.dwNumberOfProcessors);

        // physical cores via GetLogicalProcessorInformation
        DWORD len = 0;
        GetLogicalProcessorInformation(nullptr, &len);
        if (len > 0) {
            std::string buf(len, '\0');
            auto* info = reinterpret_cast<PSYSTEM_LOGICAL_PROCESSOR_INFORMATION>(buf.data());
            if (GetLogicalProcessorInformation(info, &len)) {
                uint8_t cores = 0;
                auto count = len / sizeof(SYSTEM_LOGICAL_PROCESSOR_INFORMATION);
                for (DWORD i = 0; i < count; ++i) {
                    if (info[i].Relationship == RelationProcessorCore)
                        ++cores;
                }
                s->core_count = cores;
            }
        }
        if (s->core_count == 0) s->core_count = s->thread_count;
    }

#elif defined(__linux__)
    // cpu name + freq
    {
        std::ifstream f("/proc/cpuinfo");
        std::string line;
        while (std::getline(f, line)) {
            if (line.rfind("model name", 0) == 0) {
                auto pos = line.find(':');
                if (pos != std::string::npos) {
                    auto name = line.substr(pos + 2);
                    std::strncpy(s->cpu_name, name.c_str(), sizeof(s->cpu_name) - 1);
                }
            }
            if (line.rfind("cpu MHz", 0) == 0 && s->cpu_freq_mhz == 0) {
                auto pos = line.find(':');
                if (pos != std::string::npos) {
                    s->cpu_freq_mhz = static_cast<uint32_t>(std::stof(line.substr(pos + 2)));
                }
            }
        }
    }

    // core/thread count
    s->thread_count = static_cast<uint8_t>(sysconf(_SC_NPROCESSORS_ONLN));
    {
        // count physical cores from /proc/cpuinfo unique core ids
        std::ifstream f("/proc/cpuinfo");
        std::string line;
        uint8_t cores = 0;
        while (std::getline(f, line)) {
            if (line.rfind("core id", 0) == 0)
                ++cores;
        }
        s->core_count = cores > 0 ? static_cast<uint8_t>(cores / (s->thread_count > 0 ? s->thread_count / std::max(cores, uint8_t(1)) : 1)) : s->thread_count;
        // simpler: just count unique "processor" entries for threads, "cpu cores" for cores
        // fallback
        if (s->core_count == 0) s->core_count = s->thread_count;
    }

#elif defined(__APPLE__)
    // cpu name
    {
        size_t len = sizeof(s->cpu_name);
        sysctlbyname("machdep.cpu.brand_string", s->cpu_name, &len, nullptr, 0);
    }

    // core/thread count
    {
        int val = 0;
        size_t len = sizeof(val);
        sysctlbyname("hw.physicalcpu", &val, &len, nullptr, 0);
        s->core_count = static_cast<uint8_t>(val);
        sysctlbyname("hw.logicalcpu", &val, &len, nullptr, 0);
        s->thread_count = static_cast<uint8_t>(val);
    }

    // cpu freq (macOS doesn't always expose this easily)
    {
        uint64_t freq = 0;
        size_t len = sizeof(freq);
        if (sysctlbyname("hw.cpufrequency", &freq, &len, nullptr, 0) == 0) {
            s->cpu_freq_mhz = static_cast<uint32_t>(freq / 1000000);
        }
    }
#endif
}

static void collect_hostname(SystemStatsState* s)
{
#ifdef _WIN32
    DWORD size = sizeof(s->hostname);
    GetComputerNameExA(ComputerNameDnsHostname, s->hostname, &size);

#elif defined(__linux__)
    gethostname(s->hostname, sizeof(s->hostname));

#elif defined(__APPLE__)
    gethostname(s->hostname, sizeof(s->hostname));
#endif
}

static void collect_os_info(SystemStatsState* s)
{
#ifdef _WIN32
    // use RtlGetVersion to get accurate version (GetVersionEx is deprecated/lies)
    using RtlGetVersionFn = LONG(WINAPI*)(PRTL_OSVERSIONINFOW);
    auto ntdll = GetModuleHandleW(L"ntdll.dll");
    if (ntdll) {
        auto fn = reinterpret_cast<RtlGetVersionFn>(
            GetProcAddress(ntdll, "RtlGetVersion"));
        if (fn) {
            RTL_OSVERSIONINFOW vi{};
            vi.dwOSVersionInfoSize = sizeof(vi);
            fn(&vi);
            auto ver = std::format("{}.{}.{}",
                vi.dwMajorVersion, vi.dwMinorVersion, vi.dwBuildNumber);
            std::strncpy(s->os_version, ver.c_str(), sizeof(s->os_version) - 1);
            s->os_version[sizeof(s->os_version) - 1] = '\0';

            const char* name = "Windows";
            if (vi.dwMajorVersion == 10 && vi.dwBuildNumber >= 22000)
                name = "Windows 11";
            else if (vi.dwMajorVersion == 10)
                name = "Windows 10";
            std::strncpy(s->os_name, name, sizeof(s->os_name) - 1);
            s->os_name[sizeof(s->os_name) - 1] = '\0';
        }
    }

#elif defined(__linux__)
    {
        std::ifstream f("/etc/os-release");
        std::string line;
        while (std::getline(f, line)) {
            if (line.rfind("PRETTY_NAME=", 0) == 0) {
                auto val = line.substr(13);
                if (!val.empty() && val.front() == '"') val.erase(0, 1);
                if (!val.empty() && val.back() == '"') val.pop_back();
                std::strncpy(s->os_name, val.c_str(), sizeof(s->os_name) - 1);
            }
            if (line.rfind("VERSION_ID=", 0) == 0) {
                auto val = line.substr(11);
                if (!val.empty() && val.front() == '"') val.erase(0, 1);
                if (!val.empty() && val.back() == '"') val.pop_back();
                std::strncpy(s->os_version, val.c_str(), sizeof(s->os_version) - 1);
            }
        }
    }

#elif defined(__APPLE__)
    {
        char ver[64]{};
        size_t len = sizeof(ver);
        sysctlbyname("kern.osproductversion", ver, &len, nullptr, 0);
        std::strncpy(s->os_version, ver, sizeof(s->os_version) - 1);
        std::strncpy(s->os_name, "macOS", sizeof(s->os_name) - 1);
        s->os_name[sizeof(s->os_name) - 1] = '\0';
    }
#endif
}

static void collect_network_info(SystemStatsState* s)
{
#ifdef _WIN32
    ULONG buf_len = 15000;
    std::string buf(buf_len, '\0');
    auto* addrs = reinterpret_cast<PIP_ADAPTER_ADDRESSES>(buf.data());
    ULONG flags = GAA_FLAG_SKIP_ANYCAST | GAA_FLAG_SKIP_MULTICAST | GAA_FLAG_SKIP_DNS_SERVER;

    if (GetAdaptersAddresses(AF_UNSPEC, flags, nullptr, addrs, &buf_len) == ERROR_SUCCESS) {
        for (auto* a = addrs; a; a = a->Next) {
            if (a->OperStatus != IfOperStatusUp) continue;
            if (a->IfType == IF_TYPE_SOFTWARE_LOOPBACK) continue;

            // convert wide name to narrow
            char name_buf[64]{};
            WideCharToMultiByte(CP_UTF8, 0, a->FriendlyName, -1,
                                name_buf, static_cast<int>(sizeof(name_buf)), nullptr, nullptr);
            std::strncpy(s->net_interface, name_buf, sizeof(s->net_interface) - 1);

            if (a->IfType == IF_TYPE_IEEE80211)
                std::strncpy(s->net_type, "wifi", sizeof(s->net_type));
            else if (a->IfType == IF_TYPE_ETHERNET_CSMACD)
                std::strncpy(s->net_type, "ethernet", sizeof(s->net_type));
            else
                std::strncpy(s->net_type, "unknown", sizeof(s->net_type));
            break;
        }
    }

#elif defined(__linux__)
    {
        std::ifstream f("/proc/net/dev");
        std::string line;
        std::getline(f, line); // skip header
        std::getline(f, line); // skip header
        while (std::getline(f, line)) {
            auto colon = line.find(':');
            if (colon == std::string::npos) continue;
            auto iface = line.substr(0, colon);
            // trim whitespace
            auto start = iface.find_first_not_of(' ');
            if (start != std::string::npos) iface = iface.substr(start);
            if (iface == "lo") continue;

            // check if up
            std::string state_path = "/sys/class/net/" + iface + "/operstate";
            std::ifstream sf(state_path);
            std::string state;
            if (sf >> state) {
                if (state != "up") continue;
            }

            std::strncpy(s->net_interface, iface.c_str(), sizeof(s->net_interface) - 1);

            // detect type
            std::string type_path = "/sys/class/net/" + iface + "/wireless";
            std::ifstream wf(type_path);
            if (wf.good())
                std::strncpy(s->net_type, "wifi", sizeof(s->net_type));
            else
                std::strncpy(s->net_type, "ethernet", sizeof(s->net_type));
            break;
        }
    }

#elif defined(__APPLE__)
    {
        struct ifaddrs* ifa_list = nullptr;
        if (getifaddrs(&ifa_list) == 0) {
            for (auto* ifa = ifa_list; ifa; ifa = ifa->ifa_next) {
                if (!ifa->ifa_addr) continue;
                if (ifa->ifa_addr->sa_family != AF_LINK) continue;
                if (ifa->ifa_flags & IFF_LOOPBACK) continue;
                if (!(ifa->ifa_flags & IFF_UP)) continue;

                std::strncpy(s->net_interface, ifa->ifa_name, sizeof(s->net_interface) - 1);
                // en0 is typically wifi on macOS, en1+ can be ethernet
                if (std::strncmp(ifa->ifa_name, "en0", 3) == 0)
                    std::strncpy(s->net_type, "wifi", sizeof(s->net_type));
                else
                    std::strncpy(s->net_type, "ethernet", sizeof(s->net_type));
                break;
            }
            freeifaddrs(ifa_list);
        }
    }
#endif
}

// ============================================================================
// platform: static info (called once in start())
// ============================================================================

void collect_static_info(SystemStatsState* s)
{
    collect_cpu_info(s);
    collect_hostname(s);
    collect_os_info(s);
    collect_network_info(s);
}

// ============================================================================
// platform: cpu usage (delta-based)
// ============================================================================

float sample_cpu_usage(SystemStatsState* s)
{
#ifdef _WIN32
    FILETIME idle{}, kernel{}, user{};
    if (!GetSystemTimes(&idle, &kernel, &user)) return 0.0f;

    auto to_u64 = [](const FILETIME& ft) -> uint64_t {
        return (static_cast<uint64_t>(ft.dwHighDateTime) << 32) | ft.dwLowDateTime;
    };

    uint64_t idle_val = to_u64(idle);
    uint64_t total_val = to_u64(kernel) + to_u64(user); // kernel includes idle on Windows

    uint64_t idle_delta = idle_val - s->prev_cpu_idle;
    uint64_t total_delta = total_val - s->prev_cpu_total;

    s->prev_cpu_idle = idle_val;
    s->prev_cpu_total = total_val;

    if (total_delta == 0) return 0.0f;
    return static_cast<float>(100.0 * (1.0 - static_cast<double>(idle_delta) / static_cast<double>(total_delta)));

#elif defined(__linux__)
    std::ifstream f("/proc/stat");
    std::string line;
    std::getline(f, line);

    // cpu  user nice system idle iowait irq softirq steal
    uint64_t user_t = 0, nice_t = 0, system_t = 0, idle_t = 0, iowait = 0, irq = 0, softirq = 0, steal = 0;
    std::sscanf(line.c_str(), "cpu %lu %lu %lu %lu %lu %lu %lu %lu",
                &user_t, &nice_t, &system_t, &idle_t, &iowait, &irq, &softirq, &steal);

    uint64_t idle_val = idle_t + iowait;
    uint64_t total_val = user_t + nice_t + system_t + idle_t + iowait + irq + softirq + steal;

    uint64_t idle_delta = idle_val - s->prev_cpu_idle;
    uint64_t total_delta = total_val - s->prev_cpu_total;

    s->prev_cpu_idle = idle_val;
    s->prev_cpu_total = total_val;

    if (total_delta == 0) return 0.0f;
    return static_cast<float>(100.0 * (1.0 - static_cast<double>(idle_delta) / static_cast<double>(total_delta)));

#elif defined(__APPLE__)
    host_cpu_load_info_data_t cpu_info{};
    mach_msg_type_number_t count = HOST_CPU_LOAD_INFO_COUNT;
    if (host_statistics(mach_host_self(), HOST_CPU_LOAD_INFO,
                        reinterpret_cast<host_info_t>(&cpu_info), &count) != KERN_SUCCESS)
        return 0.0f;

    uint64_t idle_val = cpu_info.cpu_ticks[CPU_STATE_IDLE];
    uint64_t total_val = cpu_info.cpu_ticks[CPU_STATE_USER] + cpu_info.cpu_ticks[CPU_STATE_SYSTEM]
                       + cpu_info.cpu_ticks[CPU_STATE_IDLE] + cpu_info.cpu_ticks[CPU_STATE_NICE];

    uint64_t idle_delta = idle_val - s->prev_cpu_idle;
    uint64_t total_delta = total_val - s->prev_cpu_total;

    s->prev_cpu_idle = idle_val;
    s->prev_cpu_total = total_val;

    if (total_delta == 0) return 0.0f;
    return static_cast<float>(100.0 * (1.0 - static_cast<double>(idle_delta) / static_cast<double>(total_delta)));
#else
    return 0.0f;
#endif
}

// ============================================================================
// platform: memory
// ============================================================================

MemInfo sample_memory()
{
    MemInfo m;
#ifdef _WIN32
    MEMORYSTATUSEX ms{};
    ms.dwLength = sizeof(ms);
    if (GlobalMemoryStatusEx(&ms)) {
        m.total_mb = static_cast<uint32_t>(ms.ullTotalPhys / (1024 * 1024));
        m.available_mb = static_cast<uint32_t>(ms.ullAvailPhys / (1024 * 1024));
        m.used_mb = m.total_mb - m.available_mb;
        m.usage_pct = static_cast<float>(ms.dwMemoryLoad);
        m.swap_total_mb = static_cast<uint32_t>(ms.ullTotalPageFile / (1024 * 1024));
        auto swap_avail = static_cast<uint32_t>(ms.ullAvailPageFile / (1024 * 1024));
        m.swap_used_mb = m.swap_total_mb - swap_avail;
    }

#elif defined(__linux__)
    std::ifstream f("/proc/meminfo");
    std::string line;
    uint64_t mem_total = 0, mem_available = 0, swap_total = 0, swap_free = 0;
    while (std::getline(f, line)) {
        uint64_t val = 0;
        if (std::sscanf(line.c_str(), "MemTotal: %lu kB", &val) == 1) mem_total = val;
        else if (std::sscanf(line.c_str(), "MemAvailable: %lu kB", &val) == 1) mem_available = val;
        else if (std::sscanf(line.c_str(), "SwapTotal: %lu kB", &val) == 1) swap_total = val;
        else if (std::sscanf(line.c_str(), "SwapFree: %lu kB", &val) == 1) swap_free = val;
    }
    m.total_mb = static_cast<uint32_t>(mem_total / 1024);
    m.available_mb = static_cast<uint32_t>(mem_available / 1024);
    m.used_mb = m.total_mb - m.available_mb;
    m.usage_pct = m.total_mb > 0 ? 100.0f * static_cast<float>(m.used_mb) / static_cast<float>(m.total_mb) : 0.0f;
    m.swap_total_mb = static_cast<uint32_t>(swap_total / 1024);
    m.swap_used_mb = static_cast<uint32_t>((swap_total - swap_free) / 1024);

#elif defined(__APPLE__)
    // total
    {
        uint64_t mem = 0;
        size_t len = sizeof(mem);
        sysctlbyname("hw.memsize", &mem, &len, nullptr, 0);
        m.total_mb = static_cast<uint32_t>(mem / (1024 * 1024));
    }
    // vm stats for used/available
    {
        vm_statistics64_data_t vm{};
        mach_msg_type_number_t count = HOST_VM_INFO64_COUNT;
        if (host_statistics64(mach_host_self(), HOST_VM_INFO64,
                              reinterpret_cast<host_info64_t>(&vm), &count) == KERN_SUCCESS) {
            uint64_t page_size = vm_page_size;
            uint64_t used = (vm.active_count + vm.wire_count) * page_size;
            m.used_mb = static_cast<uint32_t>(used / (1024 * 1024));
            m.available_mb = m.total_mb - m.used_mb;
            m.usage_pct = m.total_mb > 0 ? 100.0f * static_cast<float>(m.used_mb) / static_cast<float>(m.total_mb) : 0.0f;
        }
    }
    // swap
    {
        struct xsw_usage sw{};
        size_t len = sizeof(sw);
        if (sysctlbyname("vm.swapusage", &sw, &len, nullptr, 0) == 0) {
            m.swap_total_mb = static_cast<uint32_t>(sw.xsu_total / (1024 * 1024));
            m.swap_used_mb = static_cast<uint32_t>(sw.xsu_used / (1024 * 1024));
        }
    }
#endif
    return m;
}

// ============================================================================
// platform: disk space
// ============================================================================

DiskInfo sample_disk(const char* path)
{
    DiskInfo d;
#ifdef _WIN32
    ULARGE_INTEGER free_avail{}, total{}, total_free{};
    if (GetDiskFreeSpaceExA(path, &free_avail, &total, &total_free)) {
        constexpr double gb = 1024.0 * 1024.0 * 1024.0;
        d.total_gb = static_cast<float>(static_cast<double>(total.QuadPart) / gb);
        d.free_gb = static_cast<float>(static_cast<double>(total_free.QuadPart) / gb);
        d.used_gb = d.total_gb - d.free_gb;
        d.usage_pct = d.total_gb > 0.0f ? 100.0f * d.used_gb / d.total_gb : 0.0f;
    }
#else
    struct statvfs st{};
    if (statvfs(path, &st) == 0) {
        constexpr double gb = 1024.0 * 1024.0 * 1024.0;
        double total_bytes = static_cast<double>(st.f_blocks) * static_cast<double>(st.f_frsize);
        double free_bytes = static_cast<double>(st.f_bfree) * static_cast<double>(st.f_frsize);
        d.total_gb = static_cast<float>(total_bytes / gb);
        d.free_gb = static_cast<float>(free_bytes / gb);
        d.used_gb = d.total_gb - d.free_gb;
        d.usage_pct = d.total_gb > 0.0f ? 100.0f * d.used_gb / d.total_gb : 0.0f;
    }
#endif
    return d;
}

// ============================================================================
// platform: network bytes (returns absolute totals; caller computes delta)
// ============================================================================

NetBytes sample_network()
{
    NetBytes n;
#ifdef _WIN32
    ULONG buf_len = 0;
    GetIfTable(nullptr, &buf_len, FALSE);
    if (buf_len == 0) return n;

    std::string buf(buf_len, '\0');
    auto* table = reinterpret_cast<PMIB_IFTABLE>(buf.data());
    if (GetIfTable(table, &buf_len, FALSE) != NO_ERROR) return n;

    for (DWORD i = 0; i < table->dwNumEntries; ++i) {
        auto& row = table->table[i];
        if (row.dwType == IF_TYPE_SOFTWARE_LOOPBACK) continue;
        if (row.dwOperStatus != IF_OPER_STATUS_OPERATIONAL) continue;
        n.sent += row.dwOutOctets;
        n.recv += row.dwInOctets;
        n.connected = true;
    }

#elif defined(__linux__)
    std::ifstream f("/proc/net/dev");
    std::string line;
    std::getline(f, line); // header
    std::getline(f, line); // header
    while (std::getline(f, line)) {
        auto colon = line.find(':');
        if (colon == std::string::npos) continue;
        auto iface = line.substr(0, colon);
        auto start = iface.find_first_not_of(' ');
        if (start != std::string::npos) iface = iface.substr(start);
        if (iface == "lo") continue;

        uint64_t r_bytes = 0, t_bytes = 0;
        uint64_t dummy = 0;
        const char* data = line.c_str() + colon + 1;
        // format: recv_bytes recv_packets ... (8 fields) ... send_bytes send_packets ...
        std::sscanf(data, " %lu %lu %lu %lu %lu %lu %lu %lu %lu",
                    &r_bytes, &dummy, &dummy, &dummy, &dummy, &dummy, &dummy, &dummy, &t_bytes);
        n.recv += r_bytes;
        n.sent += t_bytes;
        n.connected = true;
    }

#elif defined(__APPLE__)
    struct ifaddrs* ifa_list = nullptr;
    if (getifaddrs(&ifa_list) == 0) {
        for (auto* ifa = ifa_list; ifa; ifa = ifa->ifa_next) {
            if (!ifa->ifa_addr) continue;
            if (ifa->ifa_addr->sa_family != AF_LINK) continue;
            if (ifa->ifa_flags & IFF_LOOPBACK) continue;
            if (!(ifa->ifa_flags & IFF_UP)) continue;

            auto* dl = reinterpret_cast<const struct if_data*>(ifa->ifa_data);
            if (dl) {
                n.sent += dl->ifi_obytes;
                n.recv += dl->ifi_ibytes;
                n.connected = true;
            }
        }
        freeifaddrs(ifa_list);
    }
#endif
    return n;
}

// ============================================================================
// platform: process stats
// ============================================================================

float sample_proc_cpu(SystemStatsState* s)
{
#ifdef _WIN32
    FILETIME creation{}, exit_t{}, kernel{}, user{};
    if (!GetProcessTimes(GetCurrentProcess(), &creation, &exit_t, &kernel, &user))
        return 0.0f;

    auto to_u64 = [](const FILETIME& ft) -> uint64_t {
        return (static_cast<uint64_t>(ft.dwHighDateTime) << 32) | ft.dwLowDateTime;
    };

    uint64_t k = to_u64(kernel);
    uint64_t u = to_u64(user);
    uint64_t now = 0;
    {
        FILETIME ft{};
        GetSystemTimeAsFileTime(&ft);
        now = to_u64(ft);
    }

    uint64_t proc_delta = (k + u) - (s->prev_proc_kernel + s->prev_proc_user);
    uint64_t time_delta = now - s->prev_proc_time;

    s->prev_proc_kernel = k;
    s->prev_proc_user = u;
    s->prev_proc_time = now;

    if (time_delta == 0) return 0.0f;
    return static_cast<float>(100.0 * static_cast<double>(proc_delta) / static_cast<double>(time_delta));

#elif defined(__linux__)
    // read /proc/self/stat for utime + stime (in clock ticks)
    std::ifstream f("/proc/self/stat");
    std::string line;
    std::getline(f, line);

    // skip past the comm field (in parentheses) to get to field 14 (utime) and 15 (stime)
    auto close_paren = line.rfind(')');
    if (close_paren == std::string::npos) return 0.0f;

    uint64_t utime = 0, stime = 0;
    // fields after ')' are: state, ppid, pgrp, session, tty, tpgid, flags, minflt, cminflt, majflt, cmajflt, utime, stime
    const char* p = line.c_str() + close_paren + 2; // skip ') '
    uint64_t dummy = 0;
    char state = ' ';
    std::sscanf(p, "%c %lu %lu %lu %lu %lu %lu %lu %lu %lu %lu %lu %lu",
                &state, &dummy, &dummy, &dummy, &dummy, &dummy, &dummy,
                &dummy, &dummy, &dummy, &dummy, &utime, &stime);

    uint64_t proc_ticks = utime + stime;
    auto now = std::chrono::steady_clock::now();
    uint64_t now_us = static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::microseconds>(now.time_since_epoch()).count());

    uint64_t tick_delta = proc_ticks - (s->prev_proc_kernel + s->prev_proc_user);
    uint64_t time_delta = now_us - s->prev_proc_time;

    s->prev_proc_kernel = utime;
    s->prev_proc_user = stime;
    s->prev_proc_time = now_us;

    if (time_delta == 0) return 0.0f;
    long ticks_per_sec = sysconf(_SC_CLK_TCK);
    double cpu_secs = static_cast<double>(tick_delta) / static_cast<double>(ticks_per_sec);
    return static_cast<float>(100.0 * cpu_secs / (static_cast<double>(time_delta) / 1e6));

#elif defined(__APPLE__)
    task_basic_info_data_t info{};
    mach_msg_type_number_t count = TASK_BASIC_INFO_COUNT;
    if (task_info(mach_task_self(), TASK_BASIC_INFO,
                  reinterpret_cast<task_info_t>(&info), &count) != KERN_SUCCESS)
        return 0.0f;

    // user + system time in seconds
    uint64_t user_us = static_cast<uint64_t>(info.user_time.seconds) * 1000000 + info.user_time.microseconds;
    uint64_t sys_us = static_cast<uint64_t>(info.system_time.seconds) * 1000000 + info.system_time.microseconds;
    uint64_t total = user_us + sys_us;

    auto now = std::chrono::steady_clock::now();
    uint64_t now_us = static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::microseconds>(now.time_since_epoch()).count());

    uint64_t proc_delta = total - (s->prev_proc_kernel + s->prev_proc_user);
    uint64_t time_delta = now_us - s->prev_proc_time;

    s->prev_proc_kernel = user_us;
    s->prev_proc_user = sys_us;
    s->prev_proc_time = now_us;

    if (time_delta == 0) return 0.0f;
    return static_cast<float>(100.0 * static_cast<double>(proc_delta) / static_cast<double>(time_delta));
#else
    return 0.0f;
#endif
}

uint32_t sample_proc_mem_mb()
{
#ifdef _WIN32
    PROCESS_MEMORY_COUNTERS pmc{};
    pmc.cb = sizeof(pmc);
    if (GetProcessMemoryInfo(GetCurrentProcess(), &pmc, sizeof(pmc))) {
        return static_cast<uint32_t>(pmc.WorkingSetSize / (1024 * 1024));
    }
    return 0;

#elif defined(__linux__)
    std::ifstream f("/proc/self/status");
    std::string line;
    while (std::getline(f, line)) {
        uint64_t val = 0;
        if (std::sscanf(line.c_str(), "VmRSS: %lu kB", &val) == 1) {
            return static_cast<uint32_t>(val / 1024);
        }
    }
    return 0;

#elif defined(__APPLE__)
    mach_task_basic_info_data_t info{};
    mach_msg_type_number_t count = MACH_TASK_BASIC_INFO_COUNT;
    if (task_info(mach_task_self(), MACH_TASK_BASIC_INFO,
                  reinterpret_cast<task_info_t>(&info), &count) == KERN_SUCCESS) {
        return static_cast<uint32_t>(info.resident_size / (1024 * 1024));
    }
    return 0;
#else
    return 0;
#endif
}

// ============================================================================
// platform: system uptime
// ============================================================================

uint32_t sample_uptime_sec()
{
#ifdef _WIN32
    return static_cast<uint32_t>(GetTickCount64() / 1000);

#elif defined(__linux__)
    std::ifstream f("/proc/uptime");
    double up = 0.0;
    f >> up;
    return static_cast<uint32_t>(up);

#elif defined(__APPLE__)
    struct timeval boot{};
    size_t len = sizeof(boot);
    int mib[2] = {CTL_KERN, KERN_BOOTTIME};
    if (sysctl(mib, 2, &boot, &len, nullptr, 0) == 0) {
        auto now = std::chrono::system_clock::now();
        auto boot_tp = std::chrono::system_clock::from_time_t(boot.tv_sec);
        auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(now - boot_tp);
        return static_cast<uint32_t>(elapsed.count());
    }
    return 0;
#else
    return 0;
#endif
}
