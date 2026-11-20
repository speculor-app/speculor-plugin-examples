// Generates a scrolling gradient test pattern at a target frame rate, drawn
// straight into frames the host lends from its pool.

#include <speculor/plugin.h>
#include <speculor/plugin_helpers.h>

#include <chrono>
#include <new>
#include <thread>
#include <vector>

// The parameters, in declaration order: values[P_...] is that parameter's value.
enum Param : uint32_t {
    P_WIDTH,
    P_HEIGHT,
    P_TARGET_FPS,
};

SPC_PLUGIN_DESCRIPTOR(
    spc::DescriptorBuilder("pattern_source", "Pattern Source", "Sources/Video")
        .author("Speculor").version("0.1.0")
        .help("Generates a scrolling gradient test pattern")
        .maturity(SPC_MATURITY_PREVIEW)
        .tags({"image", "common"})
        .output("image_out", "Image Output", SPC_DATA_FRAME)
        .int_param("width", "Width", 64, 3840, 640, 1, "Resolution")
            .param_help("Output frame width in pixels")
        .int_param("height", "Height", 64, 2160, 480, 1, "Resolution")
            .param_help("Output frame height in pixels")
        .float_param("target_fps", "Target FPS", 1.0f, 240.0f, 30.0f, 1.0f, "Timing")
            .param_help("Target output frame rate")
)

struct Params
{
    int32_t width;
    int32_t height;
    float target_fps;
};

static Params read_params(const SpcValue* values)
{
    Params p;
    p.width      = values[P_WIDTH].i;
    p.height     = values[P_HEIGHT].i;
    p.target_fps = values[P_TARGET_FPS].f;
    return p;
}

struct PatternSourceState
{
    SpcHost* host = nullptr;   // valid until stop() returns
    Params params{};
    uint64_t frame_count = 0;
    // the frame used when the host lends none
    std::vector<uint8_t> buffer;
    SpcFrame current_frame{};
    std::chrono::steady_clock::time_point last_frame_time;
};

static PatternSourceState* state(SpcPluginInstance* instance)
{
    return reinterpret_cast<PatternSourceState*>(instance);
}

static SpcPluginInstance* start(SpcHost* host, const SpcValue* values)
{
    auto* s = new (std::nothrow) PatternSourceState{};
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
    const uint32_t w = static_cast<uint32_t>(p.width);
    const uint32_t h = static_cast<uint32_t>(p.height);

    // pace to target FPS
    if (p.target_fps > 0.0f) {
        auto now = std::chrono::steady_clock::now();
        auto frame_duration = std::chrono::duration_cast<std::chrono::steady_clock::duration>(
            std::chrono::duration<double>(1.0 / p.target_fps));
        auto next_frame = s->last_frame_time + frame_duration;
        if (now < next_frame) {
            std::this_thread::sleep_until(next_frame);
        }
        s->last_frame_time = std::chrono::steady_clock::now();
    }

    // A frame from the host's pool is drawn in place and published without a
    // copy. When the host lends none, a frame of the plugin's own is copied.
    SpcFrame* out_frame = host->frame(host, 0, w, h, SPC_PIXEL_FORMAT_RGB24);
    if (!out_frame) s->buffer.resize(static_cast<size_t>(w) * h * 3);

    uint8_t* dst = out_frame ? out_frame->data : s->buffer.data();
    uint32_t stride = out_frame ? out_frame->stride : w * 3;

    // generate scrolling gradient
    uint32_t offset = static_cast<uint32_t>(s->frame_count % 256);
    for (uint32_t y = 0; y < h; ++y) {
        for (uint32_t x = 0; x < w; ++x) {
            size_t idx = static_cast<size_t>(y) * stride + x * 3;
            dst[idx + 0] = static_cast<uint8_t>((x + offset) % 256);     // R
            dst[idx + 1] = static_cast<uint8_t>((y + offset) % 256);     // G
            dst[idx + 2] = static_cast<uint8_t>((x + y + offset) % 256); // B
        }
    }

    SpcData& out = step->outputs[0];
    out.type = SPC_DATA_FRAME;
    if (out_frame) {
        out_frame->timestamp_ns = 0;   // 0: the engine stamps the time it publishes the frame
        out_frame->sequence = s->frame_count++;
        out.frame = out_frame;         // the engine's from here on, whatever process() returns
    } else {
        s->current_frame.data = s->buffer.data();
        s->current_frame.width = w;
        s->current_frame.height = h;
        s->current_frame.stride = w * 3;
        s->current_frame.format = SPC_PIXEL_FORMAT_RGB24;
        s->current_frame.sequence = s->frame_count++;
        out.frame = &s->current_frame;
    }
    return SPC_OK;
}

SPC_EXPORT_PLUGIN(
    .info           = info,
    .start          = start,
    .stop           = stop,
    .process        = process,
    .params_changed = params_changed
)
