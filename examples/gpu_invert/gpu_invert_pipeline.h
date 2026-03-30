#pragma once
#include <gpu/gpu_pipeline_base.h>
#include <gpu/vulkan_context.h>

namespace spc::gpu {

// simple GPU pipeline that inverts image pixel values
class InvertPipeline : public GpuPipelineBase {
public:
    bool init(VulkanContext& ctx);
    bool run(VulkanContext& ctx,
             const uint8_t* input, uint32_t size,
             uint8_t* output);
    void destroy(VulkanContext& ctx);

private:
    VkPipeline pipeline_ = VK_NULL_HANDLE;
    VkPipelineLayout layout_ = VK_NULL_HANDLE;
    VkDescriptorSetLayout desc_layout_ = VK_NULL_HANDLE;
    VkDescriptorPool desc_pool_ = VK_NULL_HANDLE;
    VkDescriptorSet desc_set_ = VK_NULL_HANDLE;
    VkBuffer input_buf_ = VK_NULL_HANDLE;
    VkBuffer output_buf_ = VK_NULL_HANDLE;
    VkDeviceMemory input_mem_ = VK_NULL_HANDLE;
    VkDeviceMemory output_mem_ = VK_NULL_HANDLE;
    uint32_t buf_size_ = 0;

    struct PushConstants {
        uint32_t total_bytes;
    };
};

} // namespace spc::gpu
