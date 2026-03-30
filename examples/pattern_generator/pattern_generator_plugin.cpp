// pattern_generator — Generates a scrolling color gradient frame.
//
// Demonstrates:
//   - Frame output with host-allocated buffers (zero-copy)
//   - Custom create/destroy (SPC_PLUGIN_CAST + SPC_PLUGIN_HOST_SERVICES)
//   - Manual set_parameter/get_parameter with side effects
//   - Frame pacing with target FPS
//   - .frame_alloc() capability for pool-allocated output frames

#include <speculor/plugin_helpers.h>
#include <vector>
#include <cstring>
#include <chrono>
#include <thread>

struct PatternState {
    spc::HostServices host;
    uint32_t width = 640;
    uint32_t height = 480;
    float target_fps = 30.0f;
    uint64_t frame_count = 0;
    std::vector<uint8_t> fallback_buffer;
    SpcFrame fallback_frame{};
    std::chrono::steady_clock::time_point last_frame;
};

// manual cast — we need custom create to initialize the buffer
SPC_PLUGIN_CAST(PatternState)
SPC_PLUGIN_HOST_SERVICES(PatternState, host)

SPC_PLUGIN_DESCRIPTOR(
    spc::DescriptorBuilder("pattern_generator", "Pattern Generator", "Examples")
        .author("Speculor").version("1.0.0")
        .description("Generates a scrolling RGB gradient test pattern")
        .output("frame", "Frame", SPC_DATA_FRAME)
        .int_param("width", "Width", 64, 3840, 640, 1, "Resolution")
        .int_param("height", "Height", 64, 2160, 480, 1, "Resolution")
        .float_param("target_fps", "Target FPS", 1.0f, 240.0f, 30.0f, 1.0f)
        .frame_alloc()
        .build()
)

static SpcPluginInstance* create_instance() {
    auto* s = new PatternState{};
    s->fallback_buffer.resize(s->width * s->height * 3);
    return reinterpret_cast<SpcPluginInstance*>(s);
}

static void destroy_instance(SpcPluginInstance* inst) {
    delete state(inst);
}

// custom set_parameter — resizes the buffer when resolution changes
static int set_parameter(SpcPluginInstance* inst, const char* name,
                         const SpcParameterDesc* value) {
    auto* s = state(inst);
    if (spc::try_set_float(name, value, "target_fps", s->target_fps)) return 0;

    int32_t w = static_cast<int32_t>(s->width);
    int32_t h = static_cast<int32_t>(s->height);
    bool changed = spc::try_set_int(name, value, "width", w)
                || spc::try_set_int(name, value, "height", h);
    if (!changed) return -1;

    s->width = static_cast<uint32_t>(w);
    s->height = static_cast<uint32_t>(h);
    s->fallback_buffer.resize(s->width * s->height * 3);
    return 0;
}

static int get_parameter(SpcPluginInstance* inst, const char* name,
                         SpcParameterDesc* out) {
    auto* s = state(inst);
    if (spc::try_get_int(name, out, "width", static_cast<int32_t>(s->width))) return 0;
    if (spc::try_get_int(name, out, "height", static_cast<int32_t>(s->height))) return 0;
    if (spc::try_get_float(name, out, "target_fps", s->target_fps)) return 0;
    return -1;
}

static int process(SpcPluginInstance* inst, const SpcData* /*inputs*/,
                   uint32_t /*input_count*/, SpcData* outputs,
                   uint32_t output_count) {
    auto* s = state(inst);
    if (output_count < 1) return -1;

    // pacing: sleep until next frame is due
    if (s->target_fps > 0.0f) {
        auto now = std::chrono::steady_clock::now();
        auto interval = std::chrono::duration_cast<std::chrono::steady_clock::duration>(
            std::chrono::duration<double>(1.0 / s->target_fps));
        auto next = s->last_frame + interval;
        if (now < next) std::this_thread::sleep_until(next);
        s->last_frame = std::chrono::steady_clock::now();
    }

    // try pool-allocated frame (zero-copy, engine manages lifetime)
    SpcFrame* out = s->host.acquire_frame(0, s->width, s->height,
                                          SPC_PIXEL_FORMAT_RGB24);
    uint8_t* dst = out ? out->data : s->fallback_buffer.data();
    uint32_t stride = out ? out->stride : s->width * 3;

    // generate scrolling gradient
    auto offset = static_cast<uint8_t>(s->frame_count % 256);
    for (uint32_t y = 0; y < s->height; ++y) {
        uint8_t* row = dst + y * stride;
        for (uint32_t x = 0; x < s->width; ++x) {
            row[x * 3 + 0] = static_cast<uint8_t>((x + offset) & 0xFF);
            row[x * 3 + 1] = static_cast<uint8_t>((y + offset) & 0xFF);
            row[x * 3 + 2] = static_cast<uint8_t>((x + y + offset) & 0xFF);
        }
    }

    if (out) {
        out->frame_number = s->frame_count++;
        spc::set_frame_output(outputs[0], out);
    } else {
        std::memset(&s->fallback_frame, 0, sizeof(SpcFrame));
        s->fallback_frame.data = s->fallback_buffer.data();
        s->fallback_frame.width = s->width;
        s->fallback_frame.height = s->height;
        s->fallback_frame.stride = s->width * 3;
        s->fallback_frame.format = SPC_PIXEL_FORMAT_RGB24;
        s->fallback_frame.frame_number = s->frame_count++;
        spc::set_frame_output(outputs[0], &s->fallback_frame);
    }
    return 0;
}

SPC_DECLARE_PLUGIN_FILTER(get_descriptor, create_instance, destroy_instance,
                          set_parameter, get_parameter, process)
