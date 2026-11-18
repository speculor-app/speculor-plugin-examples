// Receives newline-terminated numbers over UDP and emits the latest as SCALAR.
//
// The networking is incidental. This example shows what a plugin with blocking
// I/O has to get right:
//
//   .source()     -> SPC_NODE_SOURCE: the samples come from OUTSIDE the
//                    pipeline, so a recording captures them and replay feeds
//                    them from the session instead of running this node. Any
//                    source reading a socket, device or file needs it, or its
//                    data cannot be replayed.
//
//   interrupt()   -> Phase 1 of the two-phase shutdown. The engine calls it on
//                    another thread, BEFORE it waits for the node's thread to
//                    leave the plugin, so a process() may be running. It sets
//                    flags and cuts blocking I/O, nothing more: never free what
//                    process() is reading. stop() is Phase 2 and comes after
//                    the node's thread has left the plugin, which is where
//                    teardown belongs.
//
// The load-bearing detail is in process(): the wait on the socket is BOUNDED.
// Do not rely on closing or shutting down a socket to wake a blocked call:
// POSIX close() does not wake a blocked accept(), and Winsock shutdown() does
// not reliably wake a blocked recv(). A plugin that parks in recv() and trusts
// stop() to free it hangs engine shutdown, because the engine waits for
// process() to return before it calls stop(). Bounding the wait is what makes
// shutdown terminate; interrupt() makes it prompt rather than cost one timeout.
//
// Try it with:  echo "42.5" | nc -u -w0 127.0.0.1 9100

#include <speculor/plugin.h>
#include <speculor/plugin_helpers.h>

#include <atomic>
#include <cstdlib>
#include <cstring>
#include <new>

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

// The parameters, in declaration order: values[P_...] is that parameter's value.
enum Param : uint32_t {
    P_LISTEN_PORT,
};

SPC_PLUGIN_DESCRIPTOR(
    spc::DescriptorBuilder("udp_scalar_source", "UDP Scalar Source", "Sources/General")
        .source()
        .author("Speculor").version("0.1.0")
        .help("Receives newline-terminated numbers over UDP and outputs the latest. "
              "Demonstrates a source with two-phase shutdown and a bounded wait.")
        .maturity(SPC_MATURITY_PREVIEW)
        .tags({"common"})
        .int_param("listen_port", "Listen Port", 1, 65535, 9100, 1, "Network")
            .param_help("UDP port to bind and receive datagrams on")
        .output("scalar_out", "Scalar", SPC_DATA_SCALAR)
)

struct UdpScalarSourceState {
    SpcLogContext log{};   // the host's log; the host stays valid until stop() returns
    socket_t sock = kInvalidSocket;
    // Written by interrupt() on another thread while process() may be
    // running: hence atomic, and hence a flag only.
    std::atomic<bool> stopping{false};
    float last_value = 0.0f;
#if defined(_WIN32)
    bool wsa_started = false;
#endif
};

static UdpScalarSourceState* state(SpcPluginInstance* instance)
{
    return reinterpret_cast<UdpScalarSourceState*>(instance);
}

// Phase 2 of shutdown, and the undo of a start() that fails part way: nothing
// of the plugin is running any more.
static void close_and_free(UdpScalarSourceState* s)
{
    if (s->sock != kInvalidSocket) spc_close_sock(s->sock);
#if defined(_WIN32)
    if (s->wsa_started) WSACleanup();
#endif
    delete s;
}

// The port is bound here, once. There is no params_changed: the host keeps a
// value set while the node runs, and the next start() gets it.
static SpcPluginInstance* start(SpcHost* host, const SpcValue* values)
{
    auto* s = new (std::nothrow) UdpScalarSourceState{};
    if (!s) return nullptr;
    s->log = spc::log_context(host);   // a convenience over host->log, for the SPC_LOG_* macros
    const int port = values[P_LISTEN_PORT].i;

#if defined(_WIN32)
    WSADATA wsa;
    s->wsa_started = WSAStartup(MAKEWORD(2, 2), &wsa) == 0;
#endif

    s->sock = ::socket(AF_INET, SOCK_DGRAM, 0);
    if (s->sock == kInvalidSocket) {
        SPC_LOG_ERROR(&s->log, "UDP Scalar Source: socket() failed");
        close_and_free(s);
        return nullptr;   // the node does not run, and stop() is not called
    }

    sockaddr_in addr{};
    addr.sin_family      = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    addr.sin_port        = htons(static_cast<uint16_t>(port));
    if (::bind(s->sock, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) {
        SPC_LOG_ERROR(&s->log, "UDP Scalar Source: bind to port %d failed", port);
        close_and_free(s);
        return nullptr;
    }

    SPC_LOG_INFO(&s->log, "UDP Scalar Source listening on port %d", port);
    return reinterpret_cast<SpcPluginInstance*>(s);
}

// Phase 1, on a thread other than the node's, while a process() may be in
// flight. Closing the socket here would pull it out from under a recvfrom()
// that process() is inside.
static void interrupt(SpcPluginInstance* instance)
{
    state(instance)->stopping.store(true, std::memory_order_release);
}

// Phase 2: the node's thread has left the plugin, so tearing down is safe.
static void stop(SpcPluginInstance* instance)
{
    close_and_free(state(instance));
}

static int process(SpcPluginInstance* instance, SpcStep* step)
{
    auto* s = state(instance);

    if (!s->stopping.load(std::memory_order_acquire)) {
        pollfd pfd{};
        pfd.fd     = s->sock;
        pfd.events = POLLIN;

        // The bounded timeout is the point. On expiry we fall through and emit
        // the previous value; the engine gets its step either way and shutdown
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

    SpcData& out = step->outputs[0];
    out.type = SPC_DATA_SCALAR;
    out.scalar = {};   // timestamp_ns and sequence 0: the engine stamps them
    out.scalar.type = SPC_FIELD_FLOAT;
    out.scalar.f32 = s->last_value;
    return SPC_OK;
}

extern "C" {
SPC_API SpcPlugin spc_plugin = {
    .abi       = SPC_ABI,
    .info      = get_descriptor,
    .start     = start,
    .stop      = stop,
    .process   = process,
    .interrupt = interrupt,
};
}
