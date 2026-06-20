#ifdef SPC_HAS_VULKAN

#include "wmv_gpu_pipeline.h"

#include "wmv_compute_comp.spv.h"
#include "wmv_pack_mask_comp.spv.h"

#include <algorithm>
#include <cstring>

namespace spc::gpu {

WmvGpuPipeline::WmvGpuPipeline() = default;
WmvGpuPipeline::~WmvGpuPipeline() = default;

bool WmvGpuPipeline::init(VulkanContext &ctx)
{
    if (initialized_) return true;
    if (!ctx.valid || !ctx.device) return false;
    if (!init_base(ctx)) return false;

    constexpr int TOTAL_BINDINGS = NUM_BUFFERS + 1;  // +1 for packed_mask
    desc_layout_ = ctx.has_push_descriptors
        ? spc::gpu::create_push_descriptor_layout(ctx, TOTAL_BINDINGS)
        : spc::gpu::create_storage_buffer_layout(ctx, TOTAL_BINDINGS);
    pipeline_layout_ = spc::gpu::create_pipeline_layout(ctx, desc_layout_, sizeof(WmvPushConstants));
    if (!desc_layout_ || !pipeline_layout_) { destroy(ctx); return false; }

    if (!ctx.has_push_descriptors)
    {
        desc_pool_ = spc::gpu::create_descriptor_pool(ctx, TOTAL_BINDINGS);
        desc_set_ = spc::gpu::allocate_descriptor_set(ctx, desc_pool_, desc_layout_);
        if (!desc_pool_ || !desc_set_) { destroy(ctx); return false; }
    }

    if (!spc::gpu::create_compute_pipeline(ctx, wmv_compute_comp_spv, sizeof(wmv_compute_comp_spv),
                                           pipeline_layout_, compute_pipeline_)) { destroy(ctx); return false; }
    if (!spc::gpu::create_compute_pipeline(ctx, wmv_pack_mask_comp_spv, sizeof(wmv_pack_mask_comp_spv),
                                           pipeline_layout_, pack_mask_pipeline_)) { destroy(ctx); return false; }

    if (ctx.has_shader_objects) {
        VkPushConstantRange push_range{};
        push_range.stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
        push_range.size = sizeof(WmvPushConstants);
        compute_shader_ = spc::gpu::create_shader_object(ctx, wmv_compute_comp_spv,
            sizeof(wmv_compute_comp_spv), &desc_layout_, 1, &push_range, 1);
        pack_mask_shader_ = spc::gpu::create_shader_object(ctx, wmv_pack_mask_comp_spv,
            sizeof(wmv_pack_mask_comp_spv), &desc_layout_, 1, &push_range, 1);
    }

    initialized_ = true;
    return true;
}

bool WmvGpuPipeline::prepare(VulkanContext &ctx, uint32_t width, uint32_t height, int num_channels)
{
    if (!ctx.valid || width == 0 || height == 0) return false;
    if (width == width_ && height == height_ && num_channels == num_channels_) return true;

    wait_timeline_idle(ctx);

    // output buffer (bufs_[4]) + its staging are engine-owned ring slots now.
    staging_mask_.destroy(ctx);
    for (int i = 0; i < NUM_BUFFERS; ++i) spc::gpu::destroy_buffer(ctx, bufs_[i], mems_[i]);
    spc::gpu::destroy_buffer(ctx, packed_mask_buf_, packed_mask_mem_);

    width_ = width;
    height_ = height;
    num_channels_ = num_channels;
    pixel_count_ = width * height;
    use_wide_layout_ = false;
    const VkDeviceSize pixels = static_cast<VkDeviceSize>(width) * height;
    mask_byte_size_ = std::max(static_cast<VkDeviceSize>(pixels), VkDeviceSize(4));
    frame_bytes_ = ((pixels * num_channels + 3) / 4) * 4;
    const VkDeviceSize width4 = (static_cast<VkDeviceSize>(width) + 3) / 4;
    mask_bytes_ = width4 * height * sizeof(uint32_t);

    frame_count_ = 0;
    rolling_idx_ = 0;

    // 3 frame buffers (rolling)
    for (int i = 0; i < 3; ++i)
    {
        if (!spc::gpu::create_buffer(ctx, frame_bytes_,
                           VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                           VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, bufs_[i], mems_[i]))
        { width_ = 0; return false; }
    }
    // mask input buffer
    VkDeviceSize mask_in_bytes = ((pixels + 3) / 4) * 4;
    if (!spc::gpu::create_buffer(ctx, mask_in_bytes,
                       VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                       VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, bufs_[3], mems_[3]))
    { width_ = 0; return false; }
    // output mask buffer (bufs_[4]) is an engine output ring slot — not here.
    // packed GRAY8 output buffer (binding 5) — bound for descriptor validity.
    if (!spc::gpu::create_buffer(ctx, mask_byte_size_,
                       VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                       VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, packed_mask_buf_, packed_mask_mem_))
    { width_ = 0; return false; }

    // staging for the optional detect mask (binding 3 upload source). The input
    // frame upload staging + output download staging are engine-owned ring
    // slots resolved per frame.
    if (!staging_mask_.allocate(ctx, mask_in_bytes, VK_BUFFER_USAGE_TRANSFER_SRC_BIT))
    { width_ = 0; return false; }

    return true;
}

bool WmvGpuPipeline::record(VulkanContext &ctx, VkCommandBuffer cmd,
                             VkBuffer in_staging,
                             VkBuffer out_device, VkBuffer out_staging,
                             const uint8_t *mask_in, int mask_stride,
                             const WmvPushConstants &params,
                             VkBuffer gpu_input)
{
    if (!ctx.valid || !ctx.device) return false;
    // CPU-fed input needs a valid upload ring staging; GPU-resident input is
    // copied device-to-device from gpu_input instead.
    if (gpu_input == VK_NULL_HANDLE && in_staging == VK_NULL_HANDLE) return false;

    // advance rolling buffer (the 3 rolling frame buffers are cross-frame STATE
    // and stay plugin-owned; only the upload staging + output are ring slots).
    int target = rolling_idx_ % 3;
    rolling_idx_ = (rolling_idx_ + 1) % 3;
    ++frame_count_;

    // Frame upload source: the engine upload ring slot's staging (the plugin
    // already memcpy'd the CPU frame in). The copy lands in the rolling STATE
    // buffer bufs_[target]. cmd_upload_input(gpu_input) does device-to-device
    // when gpu_input != NULL (staging ignored).
    StagingBuffer in_stg{}; in_stg.buffer = in_staging;

    // Warmup (frame_count_ < 3): record the upload-only branch into the engine
    // secondary so the read of gpu_input is properly ordered after upstream
    // writes via the engine's inter-member barrier. Skip the compute dispatch —
    // the rolling buffer doesn't yet have 2 prior frames to read from.
    if (frame_count_ < 3)
    {
        ScopedExternalRecording scope(*this, cmd);
        cmd_upload_input(in_stg, bufs_[target], frame_bytes_, gpu_input);
        return true;
    }

    if (out_device == VK_NULL_HANDLE || out_staging == VK_NULL_HANDLE) return false;

    // copy mask to the plugin's own staging if present
    if (mask_in)
    {
        auto *mask_staging = static_cast<uint8_t *>(staging_mask_.mapped);
        if (mask_stride == static_cast<int>(width_))
            std::memcpy(mask_staging, mask_in, static_cast<size_t>(width_) * height_);
        else
            for (uint32_t y = 0; y < height_; ++y)
                std::memcpy(mask_staging + y * width_, mask_in + y * mask_stride, width_);
    }

    // rolling order: newest=target, middle=(target+2)%3, oldest=(target+1)%3.
    // Binding 4 (output) is the engine output ring slot; binding 5 (packed) is
    // the stable plugin buffer (descriptor validity only). The cache is rebuilt
    // every frame because bindings 0..2 rotate among the rolling buffers.
    int f0 = target;
    int f1 = (target + 2) % 3;
    int f2 = (target + 1) % 3;
    constexpr int TOTAL_BINDINGS = NUM_BUFFERS + 1;
    VkBuffer bufs6[] = { bufs_[f0], bufs_[f1], bufs_[f2], bufs_[3], out_device, packed_mask_buf_ };
    VkDeviceSize sizes[] = { frame_bytes_, frame_bytes_, frame_bytes_,
                             ((static_cast<VkDeviceSize>(width_) * height_ + 3) / 4) * 4,
                             mask_bytes_, mask_byte_size_ };
    for (int i = 0; i < TOTAL_BINDINGS; ++i)
    {
        push_bufs_[i]  = bufs6[i];
        push_sizes_[i] = sizes[i];
    }
    if (!ctx.has_push_descriptors)
    {
        VkDescriptorBufferInfo buf_infos[TOTAL_BINDINGS]{};
        VkWriteDescriptorSet  desc_writes[TOTAL_BINDINGS]{};
        for (int i = 0; i < TOTAL_BINDINGS; ++i)
        {
            buf_infos[i].buffer = bufs6[i];
            buf_infos[i].range  = sizes[i];
            desc_writes[i].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
            desc_writes[i].dstSet = desc_set_;
            desc_writes[i].dstBinding = static_cast<uint32_t>(i);
            desc_writes[i].descriptorCount = 1;
            desc_writes[i].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
            desc_writes[i].pBufferInfo = &buf_infos[i];
        }
        vkUpdateDescriptorSets(ctx.device, TOTAL_BINDINGS, desc_writes, 0, nullptr);
    }

    uint32_t width4   = (width_ + 3) / 4;
    uint32_t groups_x = (width4 + 15) / 16;
    uint32_t groups_y = (height_ + 15) / 16;
    WmvPushConstants pc = params;
    pc.width4 = static_cast<int32_t>(width4);

    ScopedExternalRecording scope(*this, cmd);

    cmd_upload_input(in_stg, bufs_[target], frame_bytes_, gpu_input);
    if (mask_in)
        cmd_upload_input(staging_mask_, bufs_[3], ((static_cast<VkDeviceSize>(width_) * height_ + 3) / 4) * 4);
    barrier_transfer_to_compute();
    cmd_dispatch_compute(ctx, compute_pipeline_, compute_shader_,
                         pipeline_layout_, desc_set_,
                         push_bufs_, push_sizes_, NUM_BUFFERS + 1,
                         pc, groups_x, groups_y);
    if (use_wide_layout_)
    {
        barrier_compute_to_compute();
        uint32_t pack_words  = (pixel_count_ + 3) / 4;
        uint32_t pack_groups = (pack_words + 255) / 256;
        cmd_dispatch_compute(ctx, pack_mask_pipeline_, pack_mask_shader_,
                             pipeline_layout_, desc_set_,
                             push_bufs_, push_sizes_, NUM_BUFFERS + 1,
                             pc, pack_groups, 1);
    }
    // Download to the OUTPUT ring slot's staging — engine handles the wait +
    // invalidate via the registry. Non-wide (active) path downloads the compute
    // shader's direct output (out_device).
    barrier_compute_to_transfer();
    StagingBuffer out_stg{}; out_stg.buffer = out_staging;
    cmd_download_to_staging(use_wide_layout_ ? packed_mask_buf_ : out_device,
                            out_stg,
                            use_wide_layout_ ? mask_byte_size_ : mask_bytes_);
    return true;
}

void WmvGpuPipeline::destroy(VulkanContext &ctx)
{
    if (!ctx.device) return;

    // output buffer (bufs_[4]) + its staging are engine-owned ring slots now.
    staging_mask_.destroy(ctx);
    for (int i = 0; i < NUM_BUFFERS; ++i) spc::gpu::destroy_buffer(ctx, bufs_[i], mems_[i]);
    spc::gpu::destroy_buffer(ctx, packed_mask_buf_, packed_mask_mem_);

    spc::gpu::destroy_shader_object(ctx, compute_shader_); compute_shader_ = VK_NULL_HANDLE;
    spc::gpu::destroy_shader_object(ctx, pack_mask_shader_); pack_mask_shader_ = VK_NULL_HANDLE;
    if (pack_mask_pipeline_ != VK_NULL_HANDLE) { vkDestroyPipeline(ctx.device, pack_mask_pipeline_, nullptr); pack_mask_pipeline_ = VK_NULL_HANDLE; }
    if (compute_pipeline_ != VK_NULL_HANDLE) { vkDestroyPipeline(ctx.device, compute_pipeline_, nullptr); compute_pipeline_ = VK_NULL_HANDLE; }
    if (pipeline_layout_ != VK_NULL_HANDLE) { vkDestroyPipelineLayout(ctx.device, pipeline_layout_, nullptr); pipeline_layout_ = VK_NULL_HANDLE; }
    if (desc_pool_ != VK_NULL_HANDLE) { vkDestroyDescriptorPool(ctx.device, desc_pool_, nullptr); desc_pool_ = VK_NULL_HANDLE; }
    if (desc_layout_ != VK_NULL_HANDLE) { vkDestroyDescriptorSetLayout(ctx.device, desc_layout_, nullptr); desc_layout_ = VK_NULL_HANDLE; }

    destroy_base(ctx);
    desc_set_ = VK_NULL_HANDLE;
    initialized_ = false;
    width_ = 0;
    height_ = 0;
    frame_count_ = 0;
}

} // namespace spc::gpu

#endif // SPC_HAS_VULKAN
