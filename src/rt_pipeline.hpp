#pragma once

#include <vulkan/vulkan.h>

namespace xrphoton
{
struct RayTracingFunctions;

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

// Create the descriptor set layout (binding 0 the TLAS, binding 1 the storage image,
// both raygen-only), a pool sized for exactly the one set, and allocate that set.
// Adopts device into *rt first, so on failure *rt holds whatever was created so far
// and ~RtPipeline cleans it up; the caller can bare-return.
VkResult createRtDescriptorSet(RtPipeline* rt, VkDevice device);

// Point the set's bindings at the TLAS (binding 0) and the storage image view in
// GENERAL layout (binding 1). Called once at startup and again after every successful
// swapchain recreate: the storage image view is recreated with the swapchain, and
// recreateSwapchain's device-idle guarantees the set is not referenced by pending
// work, which vkUpdateDescriptorSets requires.
void writeRtDescriptorSet(
    VkDevice device,
    VkDescriptorSet descriptorSet,
    VkAccelerationStructureKHR tlas,
    VkImageView storageImageView);

// Create the pipeline layout (the one descriptor set, no push constants yet) and the
// ray tracing pipeline: three stages sharing the single embedded shader module, three
// groups in the order the SBT build relies on — 0 raygen, 1 miss, 2 triangles-hit
// (closest hit only; the geometry is OPAQUE, so no any-hit). Primary rays only, so
// maxPipelineRayRecursionDepth is 1, which the spec guarantees supported. Requires
// createRtDescriptorSet to have succeeded (uses the set layout and the adopted
// device); on failure *rt again holds whatever was created and the caller can
// bare-return.
VkResult createRtPipeline(
    RtPipeline* rt,
    VkDevice device,
    const RayTracingFunctions& functions);

// Build the shader binding table: fetch the three group handles from the pipeline,
// lay them out one record per region (raygen, miss, hit — each region starting
// baseAlignment-aligned from the table's aligned base), and store the four
// VkStridedDeviceAddressRegionKHRs the trace consumes. The buffer is host-visible +
// coherent and written once here (see the plan's scope decisions: no staging). The
// callable region is empty but points at the table base — the VUIDs require a valid
// SBT-buffer address even for a zero region under a strict reading. Requires
// createRtPipeline to have succeeded; on failure *rt holds whatever was created and
// the caller can bare-return.
VkResult buildShaderBindingTable(
    RtPipeline* rt,
    VkPhysicalDevice physicalDevice,
    VkDevice device,
    const RayTracingFunctions& functions);
}
