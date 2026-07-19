#pragma once

#include <vulkan/vulkan.h>

#include "ray_types.hpp"
#include "vma_fwd.hpp"

namespace xrphoton
{
struct RayTracingFunctions;
struct GpuScene;
struct SceneData;

// Owns the ray tracing pipeline machinery: the descriptor set layout binding the TLAS,
// storage image, and scene records; the pipeline layout plus camera push constants;
// the descriptor pool and the one set allocated from it, the pipeline itself, and the
// shader binding table buffer.
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
    VmaAllocator allocator = nullptr;

    VkDescriptorSetLayout descriptorSetLayout = VK_NULL_HANDLE;
    VkPipelineLayout pipelineLayout = VK_NULL_HANDLE;
    VkDescriptorPool descriptorPool = VK_NULL_HANDLE;
    VkDescriptorSet descriptorSet = VK_NULL_HANDLE;

    // The one shader module holding all four entry points (Slang compiles them into
    // a single SPIR-V module). Parked here — the scratch-buffer pattern from the AS
    // build, not local RAII — so a failure between module and pipeline creation can
    // bare-return and rely on the destructor; on success it is destroyed immediately
    // after pipeline creation and nulled.
    VkShaderModule shaderModule = VK_NULL_HANDLE;

    VkPipeline pipeline = VK_NULL_HANDLE;

    // The shader binding table, host-visible + coherent by design (see the plan's
    // scope decisions: written once at startup, no staging).
    VkBuffer sbtBuffer = VK_NULL_HANDLE;
    VmaAllocation sbtBufferAllocation = nullptr;

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
// bindings 2–3 the geometry/material records, and binding 4 the fixed scene-texture
// array), a pool sized for exactly one set,
// and allocate that set.
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

// Write the program-lifetime scene buffers and every slot of the fixed texture array
// once at startup. Resize only rewrites bindings 0–1 through writeRtDescriptorSet.
void writeSceneDescriptorSet(
    VkDevice device,
    VkDescriptorSet descriptorSet,
    const GpuScene& gpuScene);

// Create the pipeline layout (the one descriptor set plus raygen camera push
// constants) and the ray tracing pipeline: four stages sharing the single embedded
// shader module, four groups in the SBT-contract order — 0 raygen, 1 miss, 2 opaque
// triangles-hit (closest hit), 3 alpha-tested triangles-hit (closest + any hit).
// Primary rays only, so maxPipelineRayRecursionDepth is 1, which the spec guarantees
// supported. Requires createRtDescriptorSet to have succeeded (uses the set layout
// and the adopted device); on failure *rt again holds whatever was created and the
// caller can bare-return.
VkResult createRtPipeline(
    RtPipeline* rt,
    VkDevice device,
    const RayTracingFunctions& functions);

// Build the shader binding table: fetch the four group handles from the pipeline,
// lay out raygen, RayTypeCount miss records, and one class-selected hit record per
// scene geometry per ray type (each region starts baseAlignment-aligned), and store the four
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
    VmaAllocator allocator,
    const RayTracingFunctions& functions,
    const SceneData& scene);
}
