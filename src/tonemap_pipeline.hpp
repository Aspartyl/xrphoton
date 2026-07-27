#pragma once

#include <vulkan/vulkan.h>

#include "tonemap.hpp"

namespace xrphoton
{
// Program-lifetime compute pipeline that reads swapchain-sized HDR radiance and writes
// the existing 8-bit linear output. The descriptor set is rewritten after resize.
struct TonemapPipeline
{
    VkDevice device = VK_NULL_HANDLE;
    VkDescriptorSetLayout descriptorSetLayout = VK_NULL_HANDLE;
    VkPipelineLayout pipelineLayout = VK_NULL_HANDLE;
    VkDescriptorPool descriptorPool = VK_NULL_HANDLE;
    VkDescriptorSet descriptorSet = VK_NULL_HANDLE;
    VkShaderModule shaderModule = VK_NULL_HANDLE;
    VkPipeline pipeline = VK_NULL_HANDLE;
    TonemapState state = DefaultTonemapState;

    TonemapPipeline() = default;
    TonemapPipeline(const TonemapPipeline&) = delete;
    TonemapPipeline& operator=(const TonemapPipeline&) = delete;
    ~TonemapPipeline();
};

VkResult createTonemapPipeline(TonemapPipeline* tonemap, VkDevice device);

void writeTonemapDescriptorSet(
    VkDevice device,
    VkDescriptorSet descriptorSet,
    VkImageView hdrRadianceImageView,
    VkImageView ldrOutputImageView);
}
