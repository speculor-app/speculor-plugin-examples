// Receives newline-terminated numbers over UDP and emits the latest as SCALAR.
//
// The networking is incidental. This example exists to show the two pieces of
// SDK surface a plugin with blocking I/O has to get right, and which no other
// example here demonstrates:
//
//   .data_source()   -> SPC_PLUGIN_DATA_SOURCE: the samples originate OUTSIDE
//                       the pipeline's dataflow, so the engine records them and
//                       replays them from the session instead of re-running
//                       this node. Any source reading a socket, device or file
//                       needs this or its data cannot be replayed.
//
//   request_stop()   -> Phase 1 of the two-phase shutdown. The engine calls it
//                       BEFORE draining the worker, on another thread, while
//                       process() may be in flight. Set atomic flags and kick
//                       interrupts only — never free what the worker is reading.
//                       stop() is Phase 2 and runs after the worker has left
//                       process(), which is where teardown belongs.
//
// The load-bearing detail is in process(): the wait on the socket is BOUNDED.
// Do not rely on closing or shutting down a socket to wake a blocked call —
// POSIX close() does not wake a blocked accept(), and Winsock shutdown() does
// not reliably wake a blocked recv(). A plugin that parks forever in recv() and
// trusts stop() to free it will hang engine shutdown, because the engine waits
// for the worker to leave process() before calling stop(). Bounding the wait is
// what makes shutdown terminate; request_stop() then makes it prompt rather
// than costing one full timeout.
//
// Try it with:  echo "42.5" | nc -u -w0 127.0.0.1 9100

#include <speculor/plugin_helpers.h>

#include <atomic>
#include <cstdlib>
#include <cstring>

#if defined(_WIN32)
#  include <winsock2.h>
#  include <ws2tcpip.h>
#  pragma comment(lib, "ws2_32.lib")
using socket_t = SOCKET;
constexpr socket_t kInvalidSocket = INVALID_SOCKET;
#  define spc_poll        WSAPoll
#  define spc_close_sock  closesocket
#else
#  include <arpa/inet.h>
#  include <netinet/in.h>
#  include <poll.h>
#  include <sys/socket.h>
#  include <unistd.h>
using socket_t = int;
constexpr socket_t kInvalidSocket = -1;
#  define spc_poll        poll
#  define spc_close_sock  ::close
#endif

// How long process() may block waiting for a datagram. This is the upper bound
// on how long engine shutdown can wait for this node, so it is a shutdown
// latency budget, not a tuning knob.
constexpr int kPollTimeoutMs = 200;

struct Params {
    int listen_port = 9100;
};

struct UdpScalarSourceState {
    spc::HostServices host;
    spc::SharedParams<Params> params;

    socket_t sock = kInvalidSocket;
    // Written by request_stop() on an arbitrary thread, read by process() on
    // the worker — hence atomic, and hence flags only.
    std::atomic<bool> stopping{false};
    float last_value = 0.0f;
#if defined(_WIN32)
    bool wsa_started = false;
#endif
};

SPC_PLUGIN(UdpScalarSourceState, host)

SPC_PLUGIN_DESCRIPTOR(
    spc::DescriptorBuilder("udp_scalar_source", "UDP Scalar Source", "Sources/General")
        .data_source()
        .author("Speculor").version("0.1.0")
        .description("Receives newline-terminated numbers over UDP and outputs the latest. "
                     "Demonstrates data_source + two-phase shutdown with a bounded wait.")
        .maturity(SPC_MATURITY_PREVIEW)
        .tags({"common"})
        .int_param("listen_port", "Listen Port", 1, 65535, 9100, 1, "Network")
            .param_description("UDP port to bind and receive datagrams on")
        .output("scalar_out", "Scalar", SPC_DATA_SCALAR)
)

static int set_parameter(SpcPluginInstance* inst, const char* name,
                         const SpcParameterDesc* value) {
    bool matched = state(inst)->params.update([&](Params& p) {
        return spc::try_set_int(name, value, "listen_port", p.listen_port);
    });
    return matched ? SPC_OK : SPC_ERR_NOT_FOUND;
}

static int get_parameter(SpcPluginInstance* inst, const char* name,
                         SpcParameterDesc* out) {
    const Params p = state(inst)->params.snapshot();
    if (spc::try_get_int(name, out, "listen_port", p.listen_port)) return SPC_OK;
    return SPC_ERR_NOT_FOUND;
}

static int start(SpcPluginInstance* inst)
{
    auto* s = state(inst);
    const Params p = s->params.snapshot();
    s->stopping.store(false, std::memory_order_release);

#if defined(_WIN32)
    WSADATA wsa;
    if (!s->wsa_started && WSAStartup(MAKEWORD(2, 2), &wsa) == 0) s->wsa_started = true;
#endif

    s->sock = ::socket(AF_INET, SOCK_DGRAM, 0);
    if (s->sock == kInvalidSocket) {
        SPC_LOG_ERROR(&s->host.cached_log, "UDP Scalar Source: socket() failed");
        return SPC_ERR;
    }

    sockaddr_in addr{};
    addr.sin_family      = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    addr.sin_port        = htons(static_cast<uint16_t>(p.listen_port));
    if (::bind(s->sock, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) {
        SPC_LOG_ERROR(&s->host.cached_log,
                      "UDP Scalar Source: bind to port %d failed", p.listen_port);
        spc_close_sock(s->sock);
        s->sock = kInvalidSocket;
        return SPC_ERR;
    }

    SPC_LOG_INFO(&s->host.cached_log, "UDP Scalar Source listening on port %d", p.listen_port);
    return SPC_OK;
}

// Phase 1. Runs on a thread other than the worker, concurrently with an
// in-flight process(). Flags and interrupts only — closing the socket here
// would pull it out from under a recvfrom() the worker is inside.
static int request_stop(SpcPluginInstance* inst)
{
    state(inst)->stopping.store(true, std::memory_order_release);
    return SPC_OK;
}

// Phase 2. The worker has left process(), so tearing down is safe.
static int stop(SpcPluginInstance* inst)
{
    auto* s = state(inst);
    if (s->sock != kInvalidSocket) {
        spc_close_sock(s->sock);
        s->sock = kInvalidSocket;
    }
#if defined(_WIN32)
    if (s->wsa_started) { WSACleanup(); s->wsa_started = false; }
#endif
    return SPC_OK;   // must be idempotent: the engine may stop an already-stopped node
}

static int process(SpcPluginInstance* inst, const SpcData* /*inputs*/,
                   uint32_t /*input_count*/, SpcData* outputs,
                   uint32_t output_count)
{
    auto* s = state(inst);
    if (output_count < 1) return SPC_ERR_INVALID;

    if (s->sock != kInvalidSocket && !s->stopping.load(std::memory_order_acquire)) {
        pollfd pfd{};
        pfd.fd     = s->sock;
        pfd.events = POLLIN;

        // The bounded timeout is the point. On expiry we fall through and emit
        // the previous value; the engine gets its tick either way and shutdown
        // can never wait longer than this.
        if (spc_poll(&pfd, 1, kPollTimeoutMs) > 0 && (pfd.revents & POLLIN)) {
            char buf[64];
            const auto n = ::recvfrom(s->sock, buf, sizeof(buf) - 1, 0, nullptr, nullptr);
            if (n > 0) {
                buf[n] = '\0';
                char* end = nullptr;
                const float v = std::strtof(buf, &end);
                if (end != buf) s->last_value = v;
            }
        }
    }

    spc::set_scalar_output(outputs[0], s->last_value);
    return SPC_OK;
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
    .set_host_services = set_host_services,
    .request_stop      = request_stop
)
