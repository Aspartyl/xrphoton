#pragma once

#include <vulkan/vulkan.h>

#include "vma_fwd.hpp"

namespace xrphoton
{
struct RayTracingFunctions;

// Owns the ray tracing scene: the triangle geometry buffers, the BLAS built over them,
// and the TLAS whose single instance references that BLAS. Built once at startup and
// swapchain-independent, so resize/recreate never touches it. Its VkDevice is non-owning
// (borrowed from VulkanContext); the destructor waits for the device to go idle itself,
// so declaration order relative to other owners does not matter — only that it precedes
// the VulkanContext teardown, which declaring it after ctx in main() guarantees.
struct AccelerationStructure
{
    VkDevice device = VK_NULL_HANDLE;
    VmaAllocator allocator = nullptr;
    // vkDestroyAccelerationStructureKHR is an extension entry point resolved at runtime,
    // so the destructor cannot call it statically. Whoever creates an acceleration
    // structure handle below must set this first (alongside device).
    PFN_vkDestroyAccelerationStructureKHR destroyAccelerationStructure = nullptr;

    // Device-local triangle geometry uploaded through transient staging buffers.
    VkBuffer vertexBuffer = VK_NULL_HANDLE;
    VmaAllocation vertexBufferAllocation = nullptr;
    VkBuffer indexBuffer = VK_NULL_HANDLE;
    VmaAllocation indexBufferAllocation = nullptr;

    // The one VkAccelerationStructureInstanceKHR the TLAS is built from.
    VkBuffer instanceBuffer = VK_NULL_HANDLE;
    VmaAllocation instanceBufferAllocation = nullptr;

    VkAccelerationStructureKHR blas = VK_NULL_HANDLE;
    VkBuffer blasBuffer = VK_NULL_HANDLE;
    VmaAllocation blasBufferAllocation = nullptr;

    // The TLAS handle is what the RT descriptor set will eventually bind
    // (VkWriteDescriptorSetAccelerationStructureKHR takes the handle, not an address).
    VkAccelerationStructureKHR tlas = VK_NULL_HANDLE;
    VkBuffer tlasBuffer = VK_NULL_HANDLE;
    VmaAllocation tlasBufferAllocation = nullptr;

    // Build-time scratch, owned here (not locally in the build path) so a failed build
    // can still bare-return and rely on ~AccelerationStructure for cleanup. The build
    // entry point frees them early once the build has been waited on successfully.
    VkBuffer blasScratchBuffer = VK_NULL_HANDLE;
    VmaAllocation blasScratchBufferAllocation = nullptr;
    VkBuffer tlasScratchBuffer = VK_NULL_HANDLE;
    VmaAllocation tlasScratchBufferAllocation = nullptr;

    AccelerationStructure() = default;
    AccelerationStructure(const AccelerationStructure&) = delete;
    AccelerationStructure& operator=(const AccelerationStructure&) = delete;
    ~AccelerationStructure();
};

// Part of device suitability: true if the BLAS vertex format supports acceleration
// structure builds on this device. The spec mandates this support for the format in
// use wherever the accelerationStructure feature exists, so this is a conformance
// backstop that fails loudly at selection rather than a real capability query — the
// same philosophy as the trace dispatch-limit gate. Kept here (rather than in
// vulkan_context) so selection and the BLAS build share one format definition.
bool hasRequiredAccelerationStructureFormatSupport(VkPhysicalDevice physicalDevice);

// Populate *as: stage the triangle into device-local buffers, then build the BLAS over
// it and the TLAS over its single instance (recorded back-to-back with a build-to-build
// barrier). Each submission borrows commandBuffer/traceQueue/fence and blocks until the
// GPU finishes. The three build-input device addresses are checked against their
// required 4/4/16-byte alignments; an under-aligned base address fails with
// VK_ERROR_INITIALIZATION_FAILED. The scratch buffers are released before returning,
// so on success *as holds only program-lifetime resources and the fence is signaled for
// the first drawFrame wait. On failure the fence may be left unsignaled and *as holds
// whatever was created so far; ~AccelerationStructure cleans it up, so the caller can
// bare-return.
VkResult buildAccelerationStructures(
    AccelerationStructure* as,
    VkPhysicalDevice physicalDevice,
    VkDevice device,
    VmaAllocator allocator,
    const RayTracingFunctions& functions,
    VkCommandBuffer commandBuffer,
    VkQueue traceQueue,
    VkFence fence);
}
