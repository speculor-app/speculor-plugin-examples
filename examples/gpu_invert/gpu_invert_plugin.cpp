// gpu_invert — Inverts image colors, with optional GPU acceleration.
//
// Demonstrates:
//   - GPU compute pipeline (Vulkan GLSL shader)
//   - Conditional GPU compilation (#ifdef SPC_HAS_VULKAN)
//   - CPU fallback when Vulkan is unavailable
//   - GpuPipelineBase inheritance pattern
//   - spc_enable_gpu() and spc_add_gpu_shaders() CMake helpers

#include <speculor/plugin_helpers.h>
#include <vector>
#include <cstring>

#ifdef SPC_HAS_VULKAN
#include <gpu/vulkan_context.h>
#include "gpu_invert_pipeline.h"
#endif

struct InvertState {
    spc::HostServices host;
    std::vector<uint8_t> cpu_buffer;
    SpcFrame fallback_frame{};
#ifdef SPC_HAS_VULKAN
    std::shared_ptr<spc::gpu::VulkanContext> gpu_ctx;
    std::unique_ptr<spc::gpu::InvertPipeline> gpu_pipeline;
    bool gpu_init_attempted = false;
    bool gpu_available = false;
#endif
};

SPC_PLUGIN_CAST(InvertState)
SPC_PLUGIN_HOST_SERVICES(InvertState, host)

SPC_PLUGIN_DESCRIPTOR(
    spc::DescriptorBuilder("gpu_invert", "GPU Invert", "Examples")
        .author("Speculor").version("1.0.0")
        .description("Inverts image colors — GPU-accelerated with CPU fallback")
        .input("frame", "Frame", SPC_DATA_FRAME, 32, SPC_CONSUME_LATEST)
        .output("frame", "Frame", SPC_DATA_FRAME)
        .gpu_compute()
        .streaming().frame_alloc()
        .build()
)

static SpcPluginInstance* create_instance() {
    return reinterpret_cast<SpcPluginInstance*>(new InvertState{});
}

static void destroy_instance(SpcPluginInstance* inst) {
    auto* s = state(inst);
#ifdef SPC_HAS_VULKAN
    if (s->gpu_pipeline && s->gpu_ctx) {
        s->gpu_pipeline->destroy(*s->gpu_ctx);
    }
#endif
    delete s;
}

#ifdef SPC_HAS_VULKAN
static bool try_init_gpu(InvertState* s) {
    if (s->gpu_init_attempted) return s->gpu_available;
    s->gpu_init_attempted = true;

    s->gpu_ctx = spc::gpu::VulkanContext::get_shared();
    if (!s->gpu_ctx) return false;

    s->gpu_pipeline = std::make_unique<spc::gpu::InvertPipeline>();
    if (!s->gpu_pipeline->init(*s->gpu_ctx)) {
        s->gpu_pipeline.reset();
        s->gpu_ctx.reset();
        return false;
    }
    s->gpu_available = true;
    SPC_LOG_INFO(&s->host.cached_log, "GPU invert pipeline initialized");
    return true;
}
#endif

static int start(SpcPluginInstance*) { return 0; }
static int stop(SpcPluginInstance*) { return 0; }

static int process(SpcPluginInstance* inst, const SpcData* inputs,
                   uint32_t input_count, SpcData* outputs,
                   uint32_t output_count) {
    auto* s = state(inst);
    if (input_count < 1 || output_count < 1) return -1;

    const SpcFrame* in = spc::input_frame(inputs, input_count, 0);
    if (!in || !in->data) return -1;

    uint32_t data_size = in->stride * in->height;

    // try GPU path
#ifdef SPC_HAS_VULKAN
    if (try_init_gpu(s)) {
        SpcFrame* out = s->host.acquire_frame(0, in->width, in->height, in->format);
        if (out) {
            if (s->gpu_pipeline->run(*s->gpu_ctx, in->data, data_size, out->data)) {
                out->frame_number = in->frame_number;
                out->timestamp_us = in->timestamp_us;
                spc::set_frame_output(outputs[0], out);
                return 0;
            }
            // GPU failed — fall through to CPU
            s->host.release_frame(out);
        }
    }
#endif

    // CPU fallback: simple byte inversion
    SpcFrame* out = s->host.acquire_frame(0, in->width, in->height, in->format);
    uint8_t* dst;
    if (out) {
        dst = out->data;
    } else {
        s->cpu_buffer.resize(data_size);
        dst = s->cpu_buffer.data();
    }

    for (uint32_t i = 0; i < data_size; ++i)
        dst[i] = 255 - in->data[i];

    if (out) {
        out->frame_number = in->frame_number;
        out->timestamp_us = in->timestamp_us;
        spc::set_frame_output(outputs[0], out);
    } else {
        std::memset(&s->fallback_frame, 0, sizeof(SpcFrame));
        s->fallback_frame.data = dst;
        s->fallback_frame.width = in->width;
        s->fallback_frame.height = in->height;
        s->fallback_frame.stride = in->stride;
        s->fallback_frame.format = in->format;
        s->fallback_frame.frame_number = in->frame_number;
        s->fallback_frame.timestamp_us = in->timestamp_us;
        spc::set_frame_output(outputs[0], &s->fallback_frame);
    }
    return 0;
}

SPC_DECLARE_PLUGIN_SOURCE(get_descriptor, create_instance, destroy_instance,
                          nullptr, nullptr, process, start, stop)
