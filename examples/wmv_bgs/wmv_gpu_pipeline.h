#pragma once

#ifdef SPC_HAS_VULKAN

#include <gpu/vulkan_context.h>
#include <gpu/gpu_utils.h>
#include <gpu/gpu_pipeline_base.h>

#include <cstdint>

namespace spc::gpu {

struct WmvPushConstants
{
    int32_t width;
    int32_t height;
    int32_t num_channels;
    float weight1;
    float weight2;
    float weight3;
    float threshold_sq;
    int32_t width4;
    int32_t has_mask;
    int32_t pad0;
};

using WmvGpuTiming = GpuTiming;

class WmvGpuPipeline : public GpuPipelineBase
{
public:
    WmvGpuPipeline();
    ~WmvGpuPipeline();

    WmvGpuPipeline(const WmvGpuPipeline &) = delete;
    WmvGpuPipeline &operator=(const WmvGpuPipeline &) = delete;

    bool init(VulkanContext &ctx);
    bool prepare(VulkanContext &ctx, uint32_t width, uint32_t height,
                 int num_channels);

    // Engine-driven coalesced submit (Phase 7). Records the per-frame
    // upload + (when ready) compute + download into the supplied secondary
    // cmd buffer and returns without submitting. Handles the 2-frame
    // warmup internally: while frame_count_ < 3, only the rolling-buffer
    // upload is recorded and the dispatch is skipped. Plugin should call
    // unconditionally and use ready() afterwards to decide whether to
    // emit a real GPU output or a zero mask.
    //
    // The 3 rolling frame buffers (bufs_[0..2]) are genuine cross-frame STATE
    // and stay plugin-owned (single). What rotates per frame is the OUTPUT
    // and the transient UPLOAD STAGING, both engine-owned K-deep ring slots:
    //   in_staging — upload ring slot's host-visible staging; the plugin
    //                already memcpy'd the CPU frame in. Copied into the rolling
    //                buffer bufs_[target]. Ignored when gpu_input != NULL.
    //   out_device / out_staging — output ring slot (binding 4 + download dst).
    //                Only used once ready(); on warmup frames pass NULL.
    bool record(VulkanContext &ctx, VkCommandBuffer cmd,
                VkBuffer in_staging,
                VkBuffer out_device, VkBuffer out_staging,
                const uint8_t *mask_in, int mask_stride,
                const WmvPushConstants &params,
                VkBuffer gpu_input = VK_NULL_HANDLE);

    VkBuffer input_buffer(int slot) const { return bufs_[slot % 3]; }

    // Byte sizes the plugin passes to the host's edge-ring acquire calls. The
    // output device buffer + staging are mask_bytes_ (width4*4*height, the
    // padded packed size the compute writes + the download copies) — same
    // allocation as the prior plugin-owned bufs_[4] / staging_out_.
    VkDeviceSize input_device_size() const { return frame_bytes_; }
    VkDeviceSize output_device_size() const { return mask_bytes_; }
    VkDeviceSize output_staging_size() const { return mask_bytes_; }
    VkDeviceSize frame_byte_size() const { return frame_bytes_; }
    VkDeviceSize packed_mask_bytes() const { return mask_byte_size_; }

    void destroy(VulkanContext &ctx);

    bool initialized() const { return initialized_; }
    bool ready() const { return frame_count_ >= 3; }
    const WmvGpuTiming &last_timing() const { return timing_; }

private:
    VkPipelineLayout pipeline_layout_ = VK_NULL_HANDLE;
    VkDescriptorSetLayout desc_layout_ = VK_NULL_HANDLE;
    VkDescriptorPool desc_pool_ = VK_NULL_HANDLE;
    VkDescriptorSet desc_set_ = VK_NULL_HANDLE;
    VkPipeline compute_pipeline_ = VK_NULL_HANDLE;
    VkPipeline pack_mask_pipeline_ = VK_NULL_HANDLE;  // wide uint32 -> packed GRAY8
    VkShaderEXT compute_shader_ = VK_NULL_HANDLE;
    VkShaderEXT pack_mask_shader_ = VK_NULL_HANDLE;

    // 3 frame buffers (rolling, binding 0..2) + 1 mask (binding 3) + 1 output
    // (binding 4). The rolling frame buffers are genuine cross-frame STATE and
    // the mask is a within-frame side input — all stay plugin-owned (single).
    // bufs_[4] (output) is NO LONGER owned here — it is an engine-owned K-deep
    // OUTPUT ring slot resolved per-frame from the host. At K=1 it is a single
    // slot, byte-identical to the prior plugin-owned single output buffer.
    static constexpr int NUM_BUFFERS = 5;
    VkBuffer bufs_[NUM_BUFFERS]{};
    VkDeviceMemory mems_[NUM_BUFFERS]{};

    // packed GRAY8 output buffer (binding 5) — bound for descriptor validity
    // (only written by the disabled wide-layout pack_mask shader).
    VkBuffer packed_mask_buf_ = VK_NULL_HANDLE;
    VkDeviceMemory packed_mask_mem_ = VK_NULL_HANDLE;

    // push descriptor buffer cache. Bindings 0..2 rotate among the rolling
    // buffers each frame; binding 4 (output) is the engine ring slot.
    VkBuffer push_bufs_[NUM_BUFFERS + 1] = {};
    VkDeviceSize push_sizes_[NUM_BUFFERS + 1] = {};

    // staging for the optional detect mask (host-visible, mapped). The input
    // frame upload staging + output download staging are engine-owned ring
    // slots (acquire_ringed_upload / acquire_ringed_output).
    StagingBuffer staging_mask_;

    uint32_t width_ = 0;
    uint32_t height_ = 0;
    int num_channels_ = 0;
    VkDeviceSize frame_bytes_ = 0;   // aligned frame size in bytes
    VkDeviceSize mask_bytes_ = 0;    // packed output mask size
    VkDeviceSize mask_byte_size_ = 0; // straight GRAY8 (width * height)
    uint32_t pixel_count_ = 0;
    bool use_wide_layout_ = false;
    int frame_count_ = 0;            // frames pushed (need >= 3)
    int rolling_idx_ = 0;            // 0,1,2 cycling

    bool initialized_ = false;
    WmvGpuTiming timing_{};
};

} // namespace spc::gpu

#endif // SPC_HAS_VULKAN
