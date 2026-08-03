#pragma once

#include <vulkan/vulkan.h>

namespace xrphoton
{
// Program-lifetime compute pipelines for the spatial denoiser: robust preparation,
// variance estimation, and the shared a-trous iteration, all over swapchain-sized
// storage images. Two descriptor sets express the working-image ping-pong (set 0
// reads A and writes B, set 1 the reverse). Preparation writes B, variance reads B
// and writes A, then a-trous starts from A. All image bindings are resize-bound.
struct DenoisePipeline
{
    VkDevice device = VK_NULL_HANDLE;
    VkDescriptorSetLayout descriptorSetLayout = VK_NULL_HANDLE;
    VkPipelineLayout pipelineLayout = VK_NULL_HANDLE;
    VkDescriptorPool descriptorPool = VK_NULL_HANDLE;
    VkDescriptorSet descriptorSets[2] = {VK_NULL_HANDLE, VK_NULL_HANDLE};
    VkShaderModule shaderModule = VK_NULL_HANDLE;
    VkPipeline clampPipeline = VK_NULL_HANDLE;
    VkPipeline variancePipeline = VK_NULL_HANDLE;
    VkPipeline atrousPipeline = VK_NULL_HANDLE;

    DenoisePipeline() = default;
    DenoisePipeline(const DenoisePipeline&) = delete;
    DenoisePipeline& operator=(const DenoisePipeline&) = delete;
    ~DenoisePipeline();
};

VkResult createDenoisePipeline(DenoisePipeline* denoise, VkDevice device);

// Point both parity sets at the current resize-bound views. Bindings 0-2 (HDR,
// normal + depth, albedo) are identical across the pair; bindings 3-4 swap the
// working images.
void writeDenoiseDescriptorSets(
    VkDevice device,
    const VkDescriptorSet descriptorSets[2],
    VkImageView hdrRadianceImageView,
    VkImageView gbufferNormalDepthImageView,
    VkImageView gbufferAlbedoImageView,
    VkImageView workingImageViewA,
    VkImageView workingImageViewB);
}
