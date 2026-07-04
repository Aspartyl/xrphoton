#pragma once

#include <vulkan/vulkan.h>

namespace xrphoton
{
// Owns the ray tracing pipeline machinery: the descriptor set layout binding the TLAS
// and the storage image, the pipeline layout over it, the descriptor pool and the one
// set allocated from it, the pipeline itself, and the shader binding table buffer.
// Program-lifetime and swapchain-independent except for one obligation: the storage
// image view is recreated with the swapchain, so the descriptor set must be rewritten
// after every recreate (the set handle is held here for exactly that — it is freed
// implicitly with the pool). Its VkDevice is non-owning (borrowed from VulkanContext);
// the destructor waits for the device to go idle itself, so declaration order relative
// to other owners does not matter — only that it precedes the VulkanContext teardown,
// which declaring it after ctx in main() guarantees.
struct RtPipeline
{
    VkDevice device = VK_NULL_HANDLE;

    VkDescriptorSetLayout descriptorSetLayout = VK_NULL_HANDLE;
    VkPipelineLayout pipelineLayout = VK_NULL_HANDLE;
    VkDescriptorPool descriptorPool = VK_NULL_HANDLE;
    VkDescriptorSet descriptorSet = VK_NULL_HANDLE;

    // The one shader module holding all three entry points (Slang compiles them into
    // a single SPIR-V module). Parked here — the scratch-buffer pattern from the AS
    // build, not local RAII — so a failure between module and pipeline creation can
    // bare-return and rely on the destructor; on success it is destroyed immediately
    // after pipeline creation and nulled.
    VkShaderModule shaderModule = VK_NULL_HANDLE;

    VkPipeline pipeline = VK_NULL_HANDLE;

    // The shader binding table, host-visible + coherent by design (see the plan's
    // scope decisions: written once at startup, no staging).
    VkBuffer sbtBuffer = VK_NULL_HANDLE;
    VkDeviceMemory sbtBufferMemory = VK_NULL_HANDLE;

    // The four regions vkCmdTraceRaysKHR consumes, computed once when the SBT is
    // built. The callable region is empty but still points at the SBT base (the
    // strict reading of the VUIDs requires a valid address even for a zero region).
    VkStridedDeviceAddressRegionKHR raygenRegion = {};
    VkStridedDeviceAddressRegionKHR missRegion = {};
    VkStridedDeviceAddressRegionKHR hitRegion = {};
    VkStridedDeviceAddressRegionKHR callableRegion = {};

    RtPipeline() = default;
    RtPipeline(const RtPipeline&) = delete;
    RtPipeline& operator=(const RtPipeline&) = delete;
    ~RtPipeline();
};
}
