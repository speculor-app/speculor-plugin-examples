#include <speculor/plugin_helpers.h>
#include <cstdlib>
#include <cmath>
#include <chrono>
#include <thread>
#include <vector>

// GUI-thread-set parameters, snapshotted on the worker (H6)
struct Params
{
    int32_t width = 640;
    int32_t height = 480;
    float target_fps = 30.0f;
};

struct PatternSourceState
{
    spc::SharedParams<Params> params;
    // dimensions the owned buffer was last sized for; the worker reallocates
    // in process() on change instead of from set_parameter (which would race)
    int32_t applied_w = -1;
    int32_t applied_h = -1;
    uint64_t frame_count;
    std::vector<uint8_t> buffer;
    SpcFrame current_frame;
    std::chrono::steady_clock::time_point last_frame_time;
    spc::HostServices host;
};

SPC_PLUGIN_CAST(PatternSourceState)
SPC_PLUGIN_HOST_SERVICES(PatternSourceState, host)

SPC_PLUGIN_DESCRIPTOR(
    spc::DescriptorBuilder("pattern_source", "Pattern Source", "Sources/Video")
        .author("Speculor").version("0.1.0")
        .description("Generates a scrolling gradient test pattern")
        .maturity(SPC_MATURITY_PREVIEW)
        .tags({"image", "common"})
        .output("image_out", "Image Output", SPC_DATA_FRAME)
        .int_param("width", "Width", 64, 3840, 640, 1, "Resolution")
            .param_description("Output frame width in pixels")
        .int_param("height", "Height", 64, 2160, 480, 1, "Resolution")
            .param_description("Output frame height in pixels")
        .float_param("target_fps", "Target FPS", 1.0f, 240.0f, 30.0f, 1.0f, "Timing")
            .param_description("Target output frame rate")
        .frame_alloc()
)

static SpcPluginInstance* create_instance()
{
    auto* s = new PatternSourceState{};
    s->frame_count = 0;
    std::memset(&s->current_frame, 0, sizeof(SpcFrame));
    return reinterpret_cast<SpcPluginInstance*>(s);  // buffer sized in process()
}

static void destroy_instance(SpcPluginInstance* inst) {
    delete state(inst);
}

static int set_parameter(SpcPluginInstance* inst, const char* name, const SpcParameterDesc* value) {
    // Mutate the shared block only; the worker reallocates the buffer in
    // process() when it observes a dimension change (was a data race).
    bool matched = state(inst)->params.update([&](Params& p) {
        return spc::try_set_int  (name, value, "width", p.width)
            || spc::try_set_int  (name, value, "height", p.height)
            || spc::try_set_float(name, value, "target_fps", p.target_fps);
    });
    return matched ? SPC_OK : SPC_ERR_NOT_FOUND;
}

static int get_parameter(SpcPluginInstance* inst, const char* name, SpcParameterDesc* out) {
    const Params p = state(inst)->params.snapshot();
    if (spc::try_get_int  (name, out, "width", p.width)) return SPC_OK;
    if (spc::try_get_int  (name, out, "height", p.height)) return SPC_OK;
    if (spc::try_get_float(name, out, "target_fps", p.target_fps)) return SPC_OK;
    return SPC_ERR_NOT_FOUND;
}

static int process(SpcPluginInstance* inst, const SpcData* /*inputs*/, uint32_t /*input_count*/,
                   SpcData* outputs, uint32_t output_count) {
    auto* s = state(inst);
    if (output_count < 1) return -1;

    const Params p = s->params.snapshot();  // one consistent view per frame
    const uint32_t w = static_cast<uint32_t>(p.width);
    const uint32_t h = static_cast<uint32_t>(p.height);

    // reallocate the owned fallback buffer on the worker when dimensions change
    if (p.width != s->applied_w || p.height != s->applied_h) {
        s->applied_w = p.width;
        s->applied_h = p.height;
        s->buffer.resize(static_cast<size_t>(w) * h * 3);
    }

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

    // try to allocate directly into the pool
    SpcFrame* out_frame = s->host.acquire_frame(0, w, h, SPC_PIXEL_FORMAT_RGB24);

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

    if (out_frame) {
        out_frame->timestamp_ns = 0;
        out_frame->frame_number = s->frame_count++;
        outputs[0].type = SPC_DATA_FRAME;
        outputs[0].frame = out_frame;
    } else {
        s->current_frame.data = s->buffer.data();
        s->current_frame.width = w;
        s->current_frame.height = h;
        s->current_frame.stride = w * 3;
        s->current_frame.format = SPC_PIXEL_FORMAT_RGB24;
        s->current_frame.frame_number = s->frame_count++;
        outputs[0].type = SPC_DATA_FRAME;
        outputs[0].frame = &s->current_frame;
    }
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
