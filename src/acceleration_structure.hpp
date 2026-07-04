#pragma once

#include <vulkan/vulkan.h>

namespace xrphoton
{
// Owns the ray tracing scene: the triangle geometry buffers, the BLAS built over them,
// and the TLAS whose single instance references that BLAS. Built once at startup and
// swapchain-independent, so resize/recreate never touches it. Its VkDevice is non-owning
// (borrowed from VulkanContext); the destructor waits for the device to go idle itself,
// so declaration order relative to other owners does not matter — only that it precedes
// the VulkanContext teardown, which declaring it after ctx in main() guarantees.
struct AccelerationStructure
{
    VkDevice device = VK_NULL_HANDLE;
    // vkDestroyAccelerationStructureKHR is an extension entry point resolved at runtime,
    // so the destructor cannot call it statically. Whoever creates an acceleration
    // structure handle below must set this first (alongside device).
    PFN_vkDestroyAccelerationStructureKHR destroyAccelerationStructure = nullptr;

    // Triangle geometry the BLAS is built from, host-visible by design (see the plan's
    // scope decisions: no staging until real geometry loading lands).
    VkBuffer vertexBuffer = VK_NULL_HANDLE;
    VkDeviceMemory vertexBufferMemory = VK_NULL_HANDLE;
    VkBuffer indexBuffer = VK_NULL_HANDLE;
    VkDeviceMemory indexBufferMemory = VK_NULL_HANDLE;

    // The one VkAccelerationStructureInstanceKHR the TLAS is built from.
    VkBuffer instanceBuffer = VK_NULL_HANDLE;
    VkDeviceMemory instanceBufferMemory = VK_NULL_HANDLE;

    VkAccelerationStructureKHR blas = VK_NULL_HANDLE;
    VkBuffer blasBuffer = VK_NULL_HANDLE;
    VkDeviceMemory blasBufferMemory = VK_NULL_HANDLE;

    // The TLAS handle is what the RT descriptor set will eventually bind
    // (VkWriteDescriptorSetAccelerationStructureKHR takes the handle, not an address).
    VkAccelerationStructureKHR tlas = VK_NULL_HANDLE;
    VkBuffer tlasBuffer = VK_NULL_HANDLE;
    VkDeviceMemory tlasBufferMemory = VK_NULL_HANDLE;

    // Build-time scratch, owned here (not locally in the build path) so a failed build
    // can still bare-return and rely on ~AccelerationStructure for cleanup. The build
    // entry point frees them early once the build has been waited on successfully.
    VkBuffer blasScratchBuffer = VK_NULL_HANDLE;
    VkDeviceMemory blasScratchBufferMemory = VK_NULL_HANDLE;
    VkBuffer tlasScratchBuffer = VK_NULL_HANDLE;
    VkDeviceMemory tlasScratchBufferMemory = VK_NULL_HANDLE;

    AccelerationStructure() = default;
    AccelerationStructure(const AccelerationStructure&) = delete;
    AccelerationStructure& operator=(const AccelerationStructure&) = delete;
    ~AccelerationStructure();
};
}
