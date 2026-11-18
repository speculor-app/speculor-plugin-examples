// Weighted Moving Variance background subtraction: a foreground mask from each
// frame. With a GPU the node is SPC_GPU_RECORDED: the engine calls record_gpu
// instead of process(), and the work is recorded into the engine's command
// buffer, fused with the GPU nodes next to it into one submit. process() is the
// CPU path, with spclib, for a node whose GPU is off or was demoted.

#include <speculor/plugin.h>
#include <speculor/plugin_helpers.h>
#include <speculor/spclib_log_bridge.h>
#include <cv_helpers.h>

#include <bgs/WeightedMovingVariance/WeightedMovingVariance.hpp>
#include <opencv2/core.hpp>

#ifdef SPC_HAS_VULKAN
#include "wmv_gpu_pipeline.h"
#include <gpu/gpu_host.h>
#endif

#include <cstring>
#include <memory>
#include <new>

// The parameters, in declaration order: values[P_...] is that parameter's value.
enum Param : uint32_t {
    P_ENABLE_THRESHOLD,
    P_THRESHOLD,
    P_ENABLE_WEIGHT,
    P_WEIGHT1,
    P_WEIGHT2,
    P_WEIGHT3,
};

// the ports, in declaration order
enum Input : uint32_t { IN_IMAGE, IN_MASK };
enum Output : uint32_t { OUT_MASK, OUT_IMAGE };

SPC_PLUGIN_DESCRIPTOR(
    spc::DescriptorBuilder("wmv_bgs", "WMV BGS", "Analysis/Motion")
        .author("Speculor").version("0.2.0")
        .help("Weighted Moving Variance background subtraction — outputs foreground mask")
        .maturity(SPC_MATURITY_STABLE)
        .tags({"image", "tracking", "surveillance"})
        .input("image_in", "Image", SPC_DATA_FRAME, 32, SPC_CONSUME_FIFO)
        .input("mask_in", "Detect Mask", SPC_DATA_FRAME, 4, SPC_CONSUME_NON_BLOCKING)
        .output("mask_out", "FG Mask", SPC_DATA_FRAME)
        .output("image_out", "Image", SPC_DATA_FRAME)
        .gpu_compute()   // SPC_GPU_RECORDED; the engine adds gpu_enabled and gpu_profile
        .bool_param("enable_threshold", "Enable Threshold", true, "WMV")
            .param_help("Apply threshold to the weighted difference image")
        .float_param("threshold", "Threshold", 1.0f, 255.0f, 30.0f, 0.5f, "WMV")
            .param_help("Weighted difference threshold to classify a pixel as foreground")
        .bool_param("enable_weight", "Enable Weight", true, "WMV")
            .param_help("Apply weighted averaging across the three background frames")
        .float_param("weight1", "Weight 1", 0.0f, 1.0f, 0.5f, 0.01f, "WMV")
            .param_help("Weight of the most recent background frame")
        .float_param("weight2", "Weight 2", 0.0f, 1.0f, 0.3f, 0.01f, "WMV")
            .param_help("Weight of the second background frame")
        .float_param("weight3", "Weight 3", 0.0f, 1.0f, 0.2f, 0.01f, "WMV")
            .param_help("Weight of the oldest background frame")
)

struct Params
{
    int32_t enable_threshold;
    float threshold;
    int32_t enable_weight;
    float weight1;
    float weight2;
    float weight3;
};

static Params read_params(const SpcValue* values)
{
    Params p;
    p.enable_threshold = values[P_ENABLE_THRESHOLD].i;
    p.threshold        = values[P_THRESHOLD].f;
    p.enable_weight    = values[P_ENABLE_WEIGHT].i;
    p.weight1          = values[P_WEIGHT1].f;
    p.weight2          = values[P_WEIGHT2].f;
    p.weight3          = values[P_WEIGHT3].f;
    return p;
}

struct WmvBgsState
{
    SpcHost* host = nullptr;   // valid until stop() returns
    SpcLogContext log{};
    Params params{};
    std::unique_ptr<spclib::bgs::WeightedMovingVariance> wmv;
    cv::Mat input_image;
    // the CPU path's own mask, for when the host lends no frame
    cv::Mat fg_mask;
    SpcFrame output_frame{};

    // cached detection mask
    cv::Mat cached_mask;
    cv::Mat empty_mask;
    bool has_cached_mask = false;

#ifdef SPC_HAS_VULKAN
    std::shared_ptr<spc::gpu::VulkanContext> vk_ctx;   // the lent device, held until stop()
    spc::gpu::WmvGpuPipeline vk_pipeline;
    bool gpu_available = false;
#endif
};

static WmvBgsState* state(SpcPluginInstance* instance)
{
    return reinterpret_cast<WmvBgsState*>(instance);
}

// the CPU detector takes the parameters; record_gpu reads them from the state
static void apply_params(WmvBgsState* s)
{
    const Params& p = s->params;
    auto& wp = s->wmv->get_parameters();
    wp.set_threshold(p.threshold);
    wp.set_enable_weight(p.enable_weight != 0);
    wp.set_enable_threshold(p.enable_threshold != 0);
    wp.set_weights(0, p.weight1);
    wp.set_weights(1, p.weight2);
    wp.set_weights(2, p.weight3);
}

static SpcPluginInstance* start(SpcHost* host, const SpcValue* values)
{
    auto* s = new (std::nothrow) WmvBgsState{};
    if (!s) return nullptr;
    s->host = host;
    s->log = spc::log_context(host);   // a convenience over host->log, for the SPC_LOG_* macros
    s->params = read_params(values);
    s->wmv = std::make_unique<spclib::bgs::WeightedMovingVariance>(spclib::bgs::WMVParams(), false);
    apply_params(s);

#ifdef SPC_HAS_VULKAN
    // record_gpu replaces process(), so the GPU side is built here, before the
    // first step. spc::gpu::context_from_host is a convenience over host->gpu(host):
    // it wraps the device the host lends, retained until the context is let go,
    // and is null when the node's GPU is off or the host has no device.
    s->vk_ctx = spc::gpu::context_from_host(host);
    if (s->vk_ctx && s->vk_pipeline.init(*s->vk_ctx))
    {
        s->gpu_available = true;
        SPC_LOG_INFO(&s->log, "WMV BGS: GPU available (%s)",
                     s->vk_ctx->device_name().c_str());
    }
    else
    {
        SPC_LOG_INFO(&s->log, "WMV BGS: GPU init failed, CPU only");
    }
#endif

    SPC_LOG_INFO(&s->log, "WMV BGS started");
    return reinterpret_cast<SpcPluginInstance*>(s);
}

static void stop(SpcPluginInstance* instance)
{
    auto* s = state(instance);
#ifdef SPC_HAS_VULKAN
    // what the plugin built on the lent device goes before the device is let go
    if (s->vk_ctx)
    {
        s->vk_pipeline.destroy(*s->vk_ctx);
        s->vk_ctx.reset();
    }
#endif
    SPC_LOG_INFO(&s->log, "WMV BGS stopped");
    delete s;
}

// On the node's thread, between steps: the detector can be changed in place.
static void params_changed(SpcPluginInstance* instance, const SpcValue* values,
                           uint64_t /*changed*/)
{
    auto* s = state(instance);
    s->params = read_params(values);
    apply_params(s);
}

// Which parameters the values leave disabled: a function of the values alone,
// called with no instance, on any thread. `states` has one entry per parameter.
static void param_rules(const SpcValue* values, uint32_t* states)
{
    if (values[P_ENABLE_WEIGHT].i == 0) {
        states[P_WEIGHT1] |= SPC_PARAM_STATE_DISABLED;
        states[P_WEIGHT2] |= SPC_PARAM_STATE_DISABLED;
        states[P_WEIGHT3] |= SPC_PARAM_STATE_DISABLED;
    }
}

// The CPU path.
static int process(SpcPluginInstance* instance, SpcStep* step)
{
    auto* s = state(instance);
    SpcHost* host = s->host;
    const SpcData* inputs = step->inputs;
    SpcData* outputs = step->outputs;

    // spclib logs through a sink kept per thread: route this thread's to the
    // node's log before calling into it
    spc::install_spclib_log_bridge(&s->log);

    if (inputs[IN_IMAGE].type != SPC_DATA_FRAME || !inputs[IN_IMAGE].frame) return SPC_SKIP;

    SpcFrame* in_frame = inputs[IN_IMAGE].frame;
    int cv_type = spc::cv_type_for_format(in_frame->format);
    if (cv_type < 0) return SPC_FAILED;

    auto w = static_cast<uint32_t>(in_frame->width);
    auto h = static_cast<uint32_t>(in_frame->height);

    if (inputs[IN_MASK].type == SPC_DATA_FRAME && inputs[IN_MASK].frame)
    {
        const SpcFrame* mask_frame = inputs[IN_MASK].frame;
        cv::Mat temp_mask(static_cast<int>(mask_frame->height),
                          static_cast<int>(mask_frame->width),
                          CV_8UC1, mask_frame->data,
                          static_cast<size_t>(mask_frame->stride));
        temp_mask.copyTo(s->cached_mask);
        s->has_cached_mask = true;
    }

    s->input_image = cv::Mat(static_cast<int>(in_frame->height),
                             static_cast<int>(in_frame->width),
                             cv_type, in_frame->data,
                             static_cast<size_t>(in_frame->stride));
    const cv::Mat& detect_mask = s->has_cached_mask ? s->cached_mask : s->empty_mask;

    SpcFrame* out = host->frame ? host->frame(host, OUT_MASK, w, h, SPC_PIXEL_FORMAT_GRAY8)
                                : nullptr;
    if (out)
    {
        // the mask is written straight into the pool frame, which is the
        // engine's once emitted: nothing here keeps pointing at it
        cv::Mat fg(static_cast<int>(h), static_cast<int>(w), CV_8UC1, out->data,
                   static_cast<size_t>(out->stride));
        s->wmv->apply(s->input_image, fg, detect_mask);
        out->sequence = in_frame->sequence;
        out->timestamp_ns = in_frame->timestamp_ns;
        outputs[OUT_MASK].type = SPC_DATA_FRAME;
        outputs[OUT_MASK].frame = out;
    }
    else
    {
        s->wmv->apply(s->input_image, s->fg_mask, detect_mask);
        spc::mat_to_frame(s->fg_mask, &s->output_frame, SPC_PIXEL_FORMAT_GRAY8,
                          in_frame->sequence, in_frame->timestamp_ns);
        outputs[OUT_MASK].type = SPC_DATA_FRAME;
        outputs[OUT_MASK].frame = &s->output_frame;
    }

    // an input frame passed on unchanged: the engine shares it, no copy
    outputs[OUT_IMAGE].type = SPC_DATA_FRAME;
    outputs[OUT_IMAGE].frame = in_frame;
    return SPC_OK;
}

#ifdef SPC_HAS_VULKAN
// a frame from host->frame() that is not emitted goes back to the host
static int release_unused(SpcHost* host, SpcFrame* frame)
{
    if (host->release_frame) host->release_frame(host, frame);
    return SPC_FAILED;
}

// Fill this frame's slot of the engine's upload ring for the image input with
// the CPU frame. Used only when the input is not already GPU-resident. The
// pipeline copies the slot's staging into one of its rolling STATE buffers;
// the slot's device buffer is unused (WMV reads its 3 rolling buffers
// directly). WMV here is 8-bit only (1 byte/channel). False when the ring call
// fails (the caller gives up the frame, and the engine counts the failure).
static bool acquire_input_upload(WmvBgsState* s, SpcGpuPass* pass,
                                 const SpcFrame* in_frame, int num_channels,
                                 VkBuffer& in_staging)
{
    in_staging = VK_NULL_HANDLE;
    const uint32_t w = static_cast<uint32_t>(in_frame->width);
    const uint32_t h = static_cast<uint32_t>(in_frame->height);
    const int row_bytes = static_cast<int>(w) * num_channels;
    // device + staging are sized input_device_size() (the padded frame_bytes_
    // the pipeline's cmd_upload_input copies into the rolling buffer); only the
    // real row_bytes*h bytes are memcpy'd in. Sizing staging at the unpadded
    // frame size would overrun on the padded device copy.
    const uint64_t ring_bytes = static_cast<uint64_t>(s->vk_pipeline.input_device_size());
    // spc::acquire_ringed_upload is a convenience over pass->acquire_ringed_upload(
    // pass, port, device_size, staging_size, &buffer): it returns the buffer,
    // zeroed when the call fails
    SpcGpuEdgeBuffer up = spc::acquire_ringed_upload(pass, IN_IMAGE, ring_bytes, ring_bytes);
    if (!up.staging_buffer || !up.staging_mapped) return false;
    auto* dst = static_cast<uint8_t*>(up.staging_mapped);
    const auto* src = in_frame->data;
    const int stride = static_cast<int>(in_frame->stride);
    if (stride == row_bytes)
        std::memcpy(dst, src, static_cast<size_t>(row_bytes) * h);
    else
        for (uint32_t y = 0; y < h; ++y)
            std::memcpy(dst + y * row_bytes, src + y * stride, row_bytes);
    in_staging = static_cast<VkBuffer>(up.staging_buffer);
    return true;
}

// The GPU path, on the node's thread, with the same step process() gets. The
// pass is the engine's, valid until stop(): its command buffer is recorded
// into here and never begun, ended or submitted, which the engine does once
// for the whole fused chain. Three failures in a row put the node on its CPU
// path for the rest of the run.
//
// The pipeline's record() handles the 2-frame warmup itself, by recording an
// upload-only branch into the engine's command buffer; ready() then says
// whether this frame has a real GPU mask or a zero one. A submit of the
// plugin's own for the warmup would race the engine's not-yet-submitted
// command buffer and read upstream's output buffer before it is written.
static int record_gpu(SpcPluginInstance* instance, SpcStep* step, SpcGpuPass* pass)
{
    auto* s = state(instance);
    SpcHost* host = s->host;
    const SpcData* inputs = step->inputs;
    SpcData* outputs = step->outputs;
    if (inputs[IN_IMAGE].type != SPC_DATA_FRAME || !inputs[IN_IMAGE].frame) return SPC_FAILED;
    if (!s->gpu_available) return SPC_FAILED;

    const Params& p = s->params;

    SpcFrame* in_frame = inputs[IN_IMAGE].frame;
    int cv_type = spc::cv_type_for_format(in_frame->format);
    if (cv_type < 0) return SPC_FAILED;

    auto w = static_cast<uint32_t>(in_frame->width);
    auto h = static_cast<uint32_t>(in_frame->height);

    if (inputs[IN_MASK].type == SPC_DATA_FRAME && inputs[IN_MASK].frame) {
        const SpcFrame* mask_frame = inputs[IN_MASK].frame;
        if (mask_frame->format == SPC_PIXEL_FORMAT_GRAY8) {
            cv::Mat temp_mask(static_cast<int>(mask_frame->height),
                              static_cast<int>(mask_frame->width),
                              CV_8UC1, mask_frame->data,
                              static_cast<size_t>(mask_frame->stride));
            temp_mask.copyTo(s->cached_mask);
            s->has_cached_mask = true;
        }
    }

    int num_channels = (in_frame->format == SPC_PIXEL_FORMAT_GRAY8 ||
                        in_frame->format == SPC_PIXEL_FORMAT_GRAY16) ? 1 : 3;
    bool is_8bit = (in_frame->format == SPC_PIXEL_FORMAT_GRAY8 ||
                    in_frame->format == SPC_PIXEL_FORMAT_BGR24 ||
                    in_frame->format == SPC_PIXEL_FORMAT_RGB24);
    if (!is_8bit || num_channels > 3) return SPC_FAILED;

    if (!s->vk_pipeline.prepare(*s->vk_ctx, w, h, num_channels)) return SPC_FAILED;

    // A GPU-resident input is read where it is. spc::gpu::buffer_from_host is
    // a convenience over host->gpu_buffer(host, gpu_handle, &buffer): the
    // VkBuffer behind the frame's handle.
    bool input_on_gpu = in_frame->gpu_handle != 0 &&
                        (in_frame->gpu_flags & SPC_GPU_FLAG_RESIDENT);
    VkBuffer gpu_input_buf = VK_NULL_HANDLE;
    if (input_on_gpu) {
        auto entry = spc::gpu::buffer_from_host(host, in_frame->gpu_handle);
        if (entry.buffer != VK_NULL_HANDLE) gpu_input_buf = entry.buffer;
    }

    spc::gpu::WmvPushConstants pc{};
    pc.width = static_cast<int32_t>(w);
    pc.height = static_cast<int32_t>(h);
    pc.num_channels = num_channels;
    pc.weight1 = p.enable_weight ? p.weight1 : (1.0f / 3.0f);
    pc.weight2 = p.enable_weight ? p.weight2 : (1.0f / 3.0f);
    pc.weight3 = p.enable_weight ? p.weight3 : (1.0f / 3.0f);
    pc.threshold_sq = p.enable_threshold ? (p.threshold * p.threshold) : 0.0f;
    pc.has_mask = s->has_cached_mask ? 1 : 0;

    const uint8_t* mask_in_ptr = s->has_cached_mask ? s->cached_mask.data : nullptr;
    int det_mask_stride = s->has_cached_mask ? static_cast<int>(s->cached_mask.step[0]) : 0;

    auto cmd = static_cast<VkCommandBuffer>(pass->cmd_buffer_handle);
    if (!cmd) return SPC_FAILED;

    SpcFrame* out = host->frame ? host->frame(host, OUT_MASK, w, h, SPC_PIXEL_FORMAT_GRAY8)
                                : nullptr;
    if (!out) return SPC_FAILED;

    // This frame's slot of the engine's output ring: the compute writes its
    // device buffer, a download copies it into the staging, and the returned
    // gpu_handle names it downstream. Taken on warmup frames too, though
    // record() leaves it unwritten then.
    SpcGpuEdgeBuffer outbuf = spc::acquire_ringed_output(
        pass, OUT_MASK, w, h, out->stride, SPC_PIXEL_FORMAT_GRAY8,
        static_cast<uint64_t>(s->vk_pipeline.output_device_size()),
        static_cast<uint64_t>(s->vk_pipeline.output_staging_size()));
    if (!outbuf.device_buffer || !outbuf.staging_buffer || outbuf.gpu_handle == 0)
        return release_unused(host, out);

    // CPU-fed input: copy the frame into this frame's upload slot (the rolling
    // STATE buffers consume it). GPU-resident input: device to device, no
    // upload.
    VkBuffer in_staging = VK_NULL_HANDLE;
    if (gpu_input_buf == VK_NULL_HANDLE &&
        !acquire_input_upload(s, pass, in_frame, num_channels, in_staging))
        return release_unused(host, out);

    // record() handles warmup (upload-only) and per-frame compute uniformly.
    if (!s->vk_pipeline.record(*s->vk_ctx, cmd,
                                in_staging,
                                static_cast<VkBuffer>(outbuf.device_buffer),
                                static_cast<VkBuffer>(outbuf.staging_buffer),
                                mask_in_ptr, det_mask_stride,
                                pc, gpu_input_buf))
        return release_unused(host, out);

    out->sequence = in_frame->sequence;
    out->timestamp_ns = in_frame->timestamp_ns;
    if (!s->vk_pipeline.ready()) {
        // Warmup frame, no compute output: a zero mask in CPU memory, not
        // GPU-resident; the output ring slot stays unused this frame.
        std::memset(out->data, 0, static_cast<size_t>(out->stride) * h);
    } else {
        // the mask is GPU-resident, in the output ring slot
        out->gpu_handle = outbuf.gpu_handle;
        out->gpu_flags |= SPC_GPU_FLAG_RESIDENT;
    }
    outputs[OUT_MASK].type = SPC_DATA_FRAME;
    outputs[OUT_MASK].frame = out;
    outputs[OUT_IMAGE].type = SPC_DATA_FRAME;
    outputs[OUT_IMAGE].frame = in_frame;
    return SPC_OK;
}
#endif

extern "C" {
SPC_API SpcPlugin spc_plugin = {
    .abi            = SPC_ABI,
    .info           = get_descriptor,
    .start          = start,
    .stop           = stop,
    .process        = process,
    .params_changed = params_changed,
#ifdef SPC_HAS_VULKAN
    .record_gpu     = record_gpu,
#endif
    .param_rules    = param_rules,
};
}
