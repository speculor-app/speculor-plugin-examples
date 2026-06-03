#include <speculor/plugin_helpers.h>
#include <cstdlib>
#include <cmath>
#include <chrono>
#include <thread>
#include <vector>

struct PatternSourceState 
{
    uint32_t width;
    uint32_t height;
    float target_fps;
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
    s->width = 640;
    s->height = 480;
    s->target_fps = 30.0f;
    s->frame_count = 0;
    s->buffer.resize(s->width * s->height * 3);
    std::memset(&s->current_frame, 0, sizeof(SpcFrame));
    return reinterpret_cast<SpcPluginInstance*>(s);
}

static void destroy_instance(SpcPluginInstance* inst) {
    delete state(inst);
}

static int set_parameter(SpcPluginInstance* inst, const char* name, const SpcParameterDesc* value) {
    auto* s = state(inst);
    if (spc::try_set_float(name, value, "target_fps", s->target_fps)) return 0;

    // width/height trigger buffer reallocation
    int32_t w = static_cast<int32_t>(s->width), h = static_cast<int32_t>(s->height);
    bool resized = spc::try_set_int(name, value, "width", w)
                || spc::try_set_int(name, value, "height", h);
    if (!resized) return -1;
    s->width = static_cast<uint32_t>(w);
    s->height = static_cast<uint32_t>(h);
    s->buffer.resize(s->width * s->height * 3);
    return 0;
}

static int get_parameter(SpcPluginInstance* inst, const char* name, SpcParameterDesc* out) {
    auto* s = state(inst);
    if (spc::try_get_int(name, out, "width", static_cast<int32_t>(s->width))) return 0;
    if (spc::try_get_int(name, out, "height", static_cast<int32_t>(s->height))) return 0;
    if (spc::try_get_float(name, out, "target_fps", s->target_fps)) return 0;
    return -1;
}

static int process(SpcPluginInstance* inst, const SpcData* /*inputs*/, uint32_t /*input_count*/,
                   SpcData* outputs, uint32_t output_count) {
    auto* s = state(inst);
    if (output_count < 1) return -1;

    // pace to target FPS
    if (s->target_fps > 0.0f) {
        auto now = std::chrono::steady_clock::now();
        auto frame_duration = std::chrono::duration_cast<std::chrono::steady_clock::duration>(
            std::chrono::duration<double>(1.0 / s->target_fps));
        auto next_frame = s->last_frame_time + frame_duration;
        if (now < next_frame) {
            std::this_thread::sleep_until(next_frame);
        }
        s->last_frame_time = std::chrono::steady_clock::now();
    }

    // try to allocate directly into the pool
    SpcFrame* out_frame = s->host.acquire_frame(0, s->width, s->height, SPC_PIXEL_FORMAT_RGB24);

    uint8_t* dst = out_frame ? out_frame->data : s->buffer.data();
    uint32_t stride = out_frame ? out_frame->stride : s->width * 3;

    // generate scrolling gradient
    uint32_t offset = static_cast<uint32_t>(s->frame_count % 256);
    for (uint32_t y = 0; y < s->height; ++y) {
        for (uint32_t x = 0; x < s->width; ++x) {
            size_t idx = static_cast<size_t>(y) * stride + x * 3;
            dst[idx + 0] = static_cast<uint8_t>((x + offset) % 256);     // R
            dst[idx + 1] = static_cast<uint8_t>((y + offset) % 256);     // G
            dst[idx + 2] = static_cast<uint8_t>((x + y + offset) % 256); // B
        }
    }

    if (out_frame) {
        out_frame->timestamp_us = 0;
        out_frame->frame_number = s->frame_count++;
        outputs[0].type = SPC_DATA_FRAME;
        outputs[0].frame = out_frame;
    } else {
        s->current_frame.data = s->buffer.data();
        s->current_frame.width = s->width;
        s->current_frame.height = s->height;
        s->current_frame.stride = s->width * 3;
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
