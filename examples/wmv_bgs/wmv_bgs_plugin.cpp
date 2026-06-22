#include <speculor/plugin_helpers.h>
#include <cv_helpers.h>
#include <speculor/spclib_log_bridge.h>

#include <bgs/WeightedMovingVariance/WeightedMovingVariance.hpp>
#include <opencv2/core.hpp>

#ifdef SPC_HAS_VULKAN
#include "wmv_gpu_pipeline.h"
#include <gpu/gpu_buffer_registry.h>
#include <gpu/gpu_output_handle.h>
#include <gpu/gpu_failure_tracker.h>
#endif

#include <atomic>
#include <memory>

// GUI-thread-set parameters, snapshotted on the worker (H6). The WMV detector
// is worker-owned: CPU process() applies the snapshot on a dirty flag and
// record_gpu reads the snapshot directly.
struct Params
{
    float threshold = 30.0f;
    int32_t enable_weight = 1;
    int32_t enable_threshold = 1;
    float weight1 = 0.5f;
    float weight2 = 0.3f;
    float weight3 = 0.2f;
};

// internal state
struct WmvBgsState
{
    spc::HostServices host;
    std::unique_ptr<spclib::bgs::WeightedMovingVariance> wmv;
    cv::Mat input_image;
    cv::Mat fg_mask;
    SpcFrame output_frame;

    // cross-thread parameter block (GUI writes, worker snapshots per frame)
    spc::SharedParams<Params> params;
    std::atomic<bool> params_dirty{false};

    // cached detection mask
    cv::Mat cached_mask;
    cv::Mat empty_mask;
    bool has_cached_mask;

    // GPU state
#ifdef SPC_HAS_VULKAN
    std::shared_ptr<spc::gpu::VulkanContext> vk_ctx;
    spc::gpu::WmvGpuPipeline vk_pipeline;
    uint64_t gpu_frame_count;
    spc::gpu::GpuFailureTracker gpu_failure{"WMV"};
#endif
    bool gpu_available;
};

SPC_PLUGIN_CAST(WmvBgsState)
SPC_PLUGIN_HOST_SERVICES(WmvBgsState, host)


SPC_PLUGIN_DESCRIPTOR(
    spc::DescriptorBuilder("wmv_bgs", "WMV BGS", "Analysis/Motion")
        .author("Speculor").version("0.2.0")
        .description("Weighted Moving Variance background subtraction — outputs foreground mask")
        .maturity(SPC_MATURITY_STABLE)
        .tags({"image", "tracking", "surveillance"})
        .input("image_in", "Image", SPC_DATA_FRAME, 32, SPC_CONSUME_FIFO)
        .input("mask_in", "Detect Mask", SPC_DATA_FRAME, 4, SPC_CONSUME_NON_BLOCKING)
        .output("mask_out", "FG Mask", SPC_DATA_FRAME)
        .output("image_out", "Image", SPC_DATA_FRAME)
        .gpu_compute()
        .bool_param("enable_threshold", "Enable Threshold", true, "WMV")
            .param_description("Apply threshold to the weighted difference image")
        .float_param("threshold", "Threshold", 1.0f, 255.0f, 30.0f, 0.5f, "WMV")
            .param_description("Weighted difference threshold to classify a pixel as foreground")
        .bool_param("enable_weight", "Enable Weight", true, "WMV")
            .param_description("Apply weighted averaging across the three background frames")
        .float_param("weight1", "Weight 1", 0.0f, 1.0f, 0.5f, 0.01f, "WMV")
            .param_description("Weight of the most recent background frame")
        .float_param("weight2", "Weight 2", 0.0f, 1.0f, 0.3f, 0.01f, "WMV")
            .param_description("Weight of the second background frame")
        .float_param("weight3", "Weight 3", 0.0f, 1.0f, 0.2f, 0.01f, "WMV")
            .param_description("Weight of the oldest background frame")
        .streaming().frame_alloc()
)

// --- lifecycle ---

// apply a parameter snapshot to the worker-owned WMV detector (worker thread)
static void apply_params(WmvBgsState* s, const Params& p)
{
    if (!s->wmv) return;
    auto& wp = s->wmv->get_parameters();
    wp.set_threshold(p.threshold);
    wp.set_enable_weight(p.enable_weight != 0);
    wp.set_enable_threshold(p.enable_threshold != 0);
    wp.set_weights(0, p.weight1);
    wp.set_weights(1, p.weight2);
    wp.set_weights(2, p.weight3);
}

static SpcPluginInstance* create_instance()
{
    auto* s = new WmvBgsState{};
    std::memset(&s->output_frame, 0, sizeof(SpcFrame));
    s->has_cached_mask = false;
    s->gpu_available = false;
#ifdef SPC_HAS_VULKAN
    s->gpu_frame_count = 0;
#endif
    s->wmv = std::make_unique<spclib::bgs::WeightedMovingVariance>(spclib::bgs::WMVParams(), false);
    return reinterpret_cast<SpcPluginInstance*>(s);
}

static void destroy_instance(SpcPluginInstance* inst)
{
    auto* s = state(inst);
#ifdef SPC_HAS_VULKAN
    if (s->vk_ctx)
    {
        s->vk_pipeline.destroy(*s->vk_ctx);
        s->vk_ctx.reset();
    }
#endif
    s->wmv.reset();
    delete s;
}

// --- parameters ---

static int set_parameter(SpcPluginInstance* inst, const char* name, const SpcParameterDesc* value)
{
    auto* s = state(inst);
    bool matched = s->params.update([&](Params& p) {
        return spc::try_set_float(name, value, "threshold", p.threshold)
            || spc::try_set_bool (name, value, "enable_weight", p.enable_weight)
            || spc::try_set_bool (name, value, "enable_threshold", p.enable_threshold)
            || spc::try_set_float(name, value, "weight1", p.weight1)
            || spc::try_set_float(name, value, "weight2", p.weight2)
            || spc::try_set_float(name, value, "weight3", p.weight3);
    });
    if (matched) s->params_dirty.store(true, std::memory_order_release);
    return matched ? 0 : -1;
}

static int get_parameter(SpcPluginInstance* inst, const char* name, SpcParameterDesc* out)
{
    const Params p = state(inst)->params.snapshot();
    if (spc::try_get_float(name, out, "threshold", p.threshold)) return 0;
    if (spc::try_get_bool(name, out, "enable_weight", p.enable_weight)) return 0;
    if (spc::try_get_bool(name, out, "enable_threshold", p.enable_threshold)) return 0;
    if (spc::try_get_float(name, out, "weight1", p.weight1)) {
        if (!p.enable_weight) out->flags |= SPC_PARAM_FLAG_DISABLED;
        return 0;
    }
    if (spc::try_get_float(name, out, "weight2", p.weight2)) {
        if (!p.enable_weight) out->flags |= SPC_PARAM_FLAG_DISABLED;
        return 0;
    }
    if (spc::try_get_float(name, out, "weight3", p.weight3)) {
        if (!p.enable_weight) out->flags |= SPC_PARAM_FLAG_DISABLED;
        return 0;
    }
    return -1;
}

// --- streaming ---

static int start(SpcPluginInstance* inst)
{
    auto* s = state(inst);
    spc::install_spclib_log_bridge(&s->host.cached_log);
    if (s->wmv) {
        s->wmv->restart();
        apply_params(s, s->params.snapshot());  // sync detector to current params
        s->params_dirty.store(false, std::memory_order_release);
    }
    s->cached_mask = cv::Mat();
    s->has_cached_mask = false;

#ifdef SPC_HAS_VULKAN
    s->gpu_available = false;
    s->gpu_frame_count = 0;
    s->vk_ctx = spc::gpu::VulkanContext::get_shared();
    if (s->vk_ctx && s->vk_pipeline.init(*s->vk_ctx))
    {
        s->gpu_available = true;
        SPC_LOG_INFO(&s->host.cached_log, "WMV BGS: GPU available (%s)",
                     s->vk_ctx->device_name().c_str());
    }
    else
    {
        SPC_LOG_INFO(&s->host.cached_log, "WMV BGS: GPU init failed, CPU only");
    }
#endif

    SPC_LOG_INFO(&s->host.cached_log, "WMV BGS started");
    return 0;
}

static int stop(SpcPluginInstance* inst)
{
    auto* s = state(inst);
#ifdef SPC_HAS_VULKAN
    if (s->vk_ctx)
    {
        s->vk_pipeline.destroy(*s->vk_ctx);
        s->vk_ctx.reset();
    }
    s->gpu_available = false;
    s->gpu_frame_count = 0;
#endif
    SPC_LOG_INFO(&s->host.cached_log, "WMV BGS stopped");
    return 0;
}

// --- process ---

static int process(SpcPluginInstance* inst, const SpcData* inputs, uint32_t input_count,
                   SpcData* outputs, uint32_t output_count)
{
    auto* s = state(inst);

    thread_local bool bridge_installed = false;
    if (!bridge_installed) {
        spc::install_spclib_log_bridge(&s->host.cached_log);
        bridge_installed = true;
    }

    if (input_count < 1 || output_count < 2) return -1;
    if (inputs[0].type != SPC_DATA_FRAME || !inputs[0].frame) return -1;

    // apply any GUI-thread parameter change to the worker-owned detector
    if (s->params_dirty.exchange(false, std::memory_order_acquire))
        apply_params(s, s->params.snapshot());

    const SpcFrame* in_frame = inputs[0].frame;
    int cv_type = spc::cv_type_for_format(in_frame->format);
    if (cv_type < 0) return -1;

    auto w = static_cast<uint32_t>(in_frame->width);
    auto h = static_cast<uint32_t>(in_frame->height);

    if (input_count > 1 && inputs[1].type == SPC_DATA_FRAME && inputs[1].frame)
    {
        const SpcFrame* mask_frame = inputs[1].frame;
        cv::Mat temp_mask(static_cast<int>(mask_frame->height),
                          static_cast<int>(mask_frame->width),
                          CV_8UC1, mask_frame->data,
                          static_cast<size_t>(mask_frame->stride));
        temp_mask.copyTo(s->cached_mask);
        s->has_cached_mask = true;
    }

    // GPU path lives entirely in record_gpu (Phase 8). process() is the
    // CPU fallback — entered when the engine demoted GPU for this node
    // or built the node without a subgraph executor.

    // CPU path
    s->input_image = cv::Mat(static_cast<int>(in_frame->height),
                             static_cast<int>(in_frame->width),
                             cv_type, in_frame->data,
                             static_cast<size_t>(in_frame->stride));

    SpcFrame* out = s->host.acquire_frame(0, w, h, SPC_PIXEL_FORMAT_GRAY8);
    if (out)
    {
        s->fg_mask = cv::Mat(static_cast<int>(h), static_cast<int>(w), CV_8UC1, out->data);
        s->wmv->apply(s->input_image, s->fg_mask,
                      s->has_cached_mask ? s->cached_mask : s->empty_mask);
        out->frame_number = in_frame->frame_number;
        out->timestamp_ns = in_frame->timestamp_ns;
        outputs[0].type = SPC_DATA_FRAME;
        outputs[0].frame = out;
    }
    else
    {
        s->wmv->apply(s->input_image, s->fg_mask,
                      s->has_cached_mask ? s->cached_mask : s->empty_mask);
        const cv::Mat& fg_mat = s->fg_mask;
        spc::mat_to_frame(fg_mat, &s->output_frame, SPC_PIXEL_FORMAT_GRAY8,
                         in_frame->frame_number, in_frame->timestamp_ns);
        outputs[0].type = SPC_DATA_FRAME;
        outputs[0].frame = &s->output_frame;
    }

    outputs[1].type = SPC_DATA_FRAME;
    outputs[1].frame = const_cast<SpcFrame*>(in_frame);
    return 0;
}

// --- record_gpu (engine-driven coalesced submit, Phase 7) ---
//
// Mirrors the GPU happy path of process() for the per-frame compute case.
// The pipeline's record() handles the 2-frame warmup internally by
// recording an upload-only branch into the engine secondary; we use
// ready() afterwards to decide whether to emit a real GPU mask or a
// zero mask. A plugin-private submit_and_wait for warmup would race the
// engine's not-yet-submitted coalesced secondary and read uninitialized
// memory from upstream's output buffer.

#ifdef SPC_HAS_VULKAN
// Acquire the engine-owned K-deep INPUT upload ring slot for this frame and
// memcpy the CPU frame into its staging. Only used when the upstream input is
// NOT already GPU-resident (gpu_input == NULL). The plugin copies the slot's
// staging into one of its rolling STATE buffers; the slot's device buffer is
// unused (WMV reads its 3 rolling buffers directly, not a separate input
// binding). WMV here is 8-bit only (1 byte/channel). Returns false if the host
// has no edge-ring service or the allocation failed (caller demotes to CPU).
static bool acquire_input_upload(WmvBgsState* s, SpcGpuRecordCtx* rctx,
                                 const SpcFrame* in_frame, int num_channels,
                                 VkBuffer& in_staging)
{
    in_staging = VK_NULL_HANDLE;
    if (!rctx->edge_ring_ctx) return false;
    const uint32_t w = static_cast<uint32_t>(in_frame->width);
    const uint32_t h = static_cast<uint32_t>(in_frame->height);
    const int row_bytes = static_cast<int>(w) * num_channels;
    // device + staging are sized input_device_size() (the padded frame_bytes_
    // the pipeline's cmd_upload_input copies into the rolling buffer); only the
    // real row_bytes*h bytes are memcpy'd in. Sizing staging at the unpadded
    // frame size would overrun on the padded device copy.
    const uint64_t ring_bytes = static_cast<uint64_t>(s->vk_pipeline.input_device_size());
    SpcGpuEdgeBuffer up = s->host.acquire_ringed_upload(
        rctx->edge_ring_ctx, 0, ring_bytes, ring_bytes);
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

static int record_gpu(SpcPluginInstance* inst, SpcGpuRecordCtx* rctx)
{
    auto* s = state(inst);
    if (!rctx || rctx->struct_size < sizeof(SpcGpuRecordCtx)) return -1;
    if (rctx->input_count < 1 || rctx->output_count < 2) return -1;
    if (rctx->inputs[0].type != SPC_DATA_FRAME || !rctx->inputs[0].frame) return -1;
    if (!s->gpu_available || !s->vk_ctx) return -1;

    const Params p = s->params.snapshot();  // one consistent view per frame

    const SpcFrame* in_frame = rctx->inputs[0].frame;
    int cv_type = spc::cv_type_for_format(in_frame->format);
    if (cv_type < 0) return -1;

    auto w = static_cast<uint32_t>(in_frame->width);
    auto h = static_cast<uint32_t>(in_frame->height);

    if (rctx->input_count > 1 && rctx->inputs[1].type == SPC_DATA_FRAME && rctx->inputs[1].frame) {
        const SpcFrame* mask_frame = rctx->inputs[1].frame;
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
    if (!is_8bit || num_channels > 3) return -1;

    if (!s->vk_pipeline.prepare(*s->vk_ctx, w, h, num_channels)) return -1;

    bool input_on_gpu = in_frame->gpu_handle != 0 &&
                        (in_frame->gpu_flags & SPC_GPU_FLAG_RESIDENT);
    VkBuffer gpu_input_buf = VK_NULL_HANDLE;
    if (input_on_gpu) {
        auto entry = spc::gpu::GpuBufferRegistry::instance().lookup(in_frame->gpu_handle);
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

    auto cmd = static_cast<VkCommandBuffer>(rctx->cmd_buffer_handle);
    if (!cmd) return -1;

    SpcFrame* out = s->host.acquire_frame(0, w, h, SPC_PIXEL_FORMAT_GRAY8);
    if (!out) return -1;

    // Acquire the engine-owned OUTPUT ring slot for this frame (binding 4 +
    // download dst). The engine registers the slot's buffer + staging against
    // the returned gpu_handle, so the post-submit invalidate covers it and
    // downstream consumers resolve it by handle. Acquired on warmup frames too
    // (record() simply doesn't write it then).
    if (!rctx->edge_ring_ctx) { s->host.release_frame(out); return -1; }
    SpcGpuEdgeBuffer outbuf = s->host.acquire_ringed_output(
        rctx->edge_ring_ctx, 0, w, h, out->stride, SPC_PIXEL_FORMAT_GRAY8,
        static_cast<uint64_t>(s->vk_pipeline.output_device_size()),
        static_cast<uint64_t>(s->vk_pipeline.output_staging_size()));
    if (!outbuf.device_buffer || !outbuf.staging_buffer || outbuf.gpu_handle == 0) {
        s->host.release_frame(out);
        return -1;
    }

    // CPU-fed input → acquire the upload ring slot + memcpy the frame into its
    // staging (the rolling STATE buffers consume it). GPU-resident input →
    // device-to-device, no upload ring.
    VkBuffer in_staging = VK_NULL_HANDLE;
    if (gpu_input_buf == VK_NULL_HANDLE &&
        !acquire_input_upload(s, rctx, in_frame, num_channels, in_staging)) {
        s->host.release_frame(out);
        return -1;
    }

    // record() handles warmup (upload-only) and per-frame compute uniformly.
    if (!s->vk_pipeline.record(*s->vk_ctx, cmd,
                                in_staging,
                                static_cast<VkBuffer>(outbuf.device_buffer),
                                static_cast<VkBuffer>(outbuf.staging_buffer),
                                mask_in_ptr, det_mask_stride,
                                pc, gpu_input_buf)) {
        s->host.release_frame(out);
        return -1;
    }

    if (!s->vk_pipeline.ready()) {
        // Warmup frame — no compute output. Emit zero mask (NOT GPU-resident)
        // + passthrough; the output ring slot stays unused this frame.
        std::memset(out->data, 0, static_cast<size_t>(out->stride) * h);
        out->frame_number = in_frame->frame_number;
        out->timestamp_ns = in_frame->timestamp_ns;
        rctx->outputs[0].type = SPC_DATA_FRAME;
        rctx->outputs[0].frame = out;
        rctx->outputs[1].type = SPC_DATA_FRAME;
        rctx->outputs[1].frame = const_cast<SpcFrame*>(in_frame);
        return 0;
    }

    ++s->gpu_frame_count;
    // Stamp the output frame as GPU-resident from the engine ring slot's handle
    // (same fields GpuOutputHandle::bind_to_frame set on the prior path).
    out->gpu_handle   = outbuf.gpu_handle;
    out->gpu_flags   |= SPC_GPU_FLAG_RESIDENT;
    out->frame_number = in_frame->frame_number;
    out->timestamp_ns = in_frame->timestamp_ns;
    rctx->outputs[0].type = SPC_DATA_FRAME;
    rctx->outputs[0].frame = out;
    rctx->outputs[1].type = SPC_DATA_FRAME;
    rctx->outputs[1].frame = const_cast<SpcFrame*>(in_frame);
    return 0;
}
#endif

#ifdef SPC_HAS_VULKAN
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
    .record_gpu        = record_gpu
)
#else
SPC_PLUGIN_VTABLE(
    .get_descriptor    = get_descriptor,
    .create_instance   = create_instance,
    .destroy_instance  = destroy_instance,
    .set_parameter     = set_parameter,
    .get_parameter     = get_parameter,
    .process           = process,
    .start             = start,
    .stop              = stop,
    .set_host_services = set_host_services
)
#endif
