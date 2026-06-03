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

    // push frame + run in single GPU submission
    // returns false on first 2 frames (need 3 to compute)
    // when ready, computes WMV and writes out_mask
    // gpu_input: if non-null, device-to-device copy replaces CPU staging upload
    // skip_download: if true, skip CPU memcpy from staging (staging still populated for lazy readback)
    bool push_and_run(VulkanContext &ctx,
                      const uint8_t *frame, int frame_stride,
                      const uint8_t *mask_in, int mask_stride,
                      uint8_t *out_mask,
                      const WmvPushConstants &params,
                      VkBuffer gpu_input = VK_NULL_HANDLE,
                      bool skip_download = false);

    // Engine-driven coalesced submit (Phase 7). Records the per-frame
    // upload + (when ready) compute + download into the supplied secondary
    // cmd buffer and returns without submitting. Handles the 2-frame
    // warmup internally: while frame_count_ < 3, only the rolling-buffer
    // upload is recorded and the dispatch is skipped. Plugin should call
    // unconditionally and use ready() afterwards to decide whether to
    // emit a real GPU output or a zero mask.
    bool record(VulkanContext &ctx, VkCommandBuffer cmd,
                const uint8_t *frame, int frame_stride,
                const uint8_t *mask_in, int mask_stride,
                const WmvPushConstants &params,
                VkBuffer gpu_input = VK_NULL_HANDLE);

    VkBuffer input_buffer(int slot) const { return bufs_[slot % 3]; }
    VkDeviceSize frame_byte_size() const { return frame_bytes_; }

    // GPU-resident packed GRAY8 output. Returns whichever buffer the compute
    // just wrote into: in wide layout the pack_mask shader produces
    // packed_mask_buf_; in non-wide layout (today's only active path) the
    // compute shader writes packed GRAY8 directly into bufs_[4] (output),
    // and packed_mask_buf_ stays empty — returning it would register a zero
    // buffer for GPU-resident downstream consumers.
    VkBuffer packed_mask_buffer() const {
        return use_wide_layout_ ? packed_mask_buf_ : bufs_[4];
    }
    VkDeviceMemory packed_mask_memory() const {
        return use_wide_layout_ ? packed_mask_mem_ : mems_[4];
    }
    VkDeviceSize packed_mask_bytes() const { return mask_byte_size_; }
    const void* staging_output_mapped() const { return staging_out_.mapped; }
    const StagingBuffer& output_staging() const { return staging_out_; }
    void invalidate_staging_output(VulkanContext& ctx) { staging_out_.invalidate(ctx); }

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

    // 3 frame buffers (rolling) + 1 mask + 1 output
    static constexpr int NUM_BUFFERS = 5;
    VkBuffer bufs_[NUM_BUFFERS]{};
    VkDeviceMemory mems_[NUM_BUFFERS]{};

    // packed GRAY8 output buffer (binding 5)
    VkBuffer packed_mask_buf_ = VK_NULL_HANDLE;
    VkDeviceMemory packed_mask_mem_ = VK_NULL_HANDLE;

    // push descriptor buffer cache
    VkBuffer push_bufs_[NUM_BUFFERS + 1] = {};
    VkDeviceSize push_sizes_[NUM_BUFFERS + 1] = {};

    // staging
    StagingBuffer staging_in_;
    StagingBuffer staging_out_;

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
