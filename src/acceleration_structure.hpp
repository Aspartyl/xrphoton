#pragma once

#include <vector>

#include <vulkan/vulkan.h>

#include "vma_fwd.hpp"

namespace xrphoton
{
struct RayTracingFunctions;
struct SceneData;
struct GpuScene;

struct BlasEntry
{
    VkAccelerationStructureKHR blas = VK_NULL_HANDLE;
    VkBuffer buffer = VK_NULL_HANDLE;
    VmaAllocation allocation = nullptr;
    VkDeviceAddress address = 0;
};

struct TlasInstanceSlot
{
    VkBuffer buffer = VK_NULL_HANDLE;
    VmaAllocation allocation = nullptr;
    void* mapped = nullptr;
    VkDeviceAddress address = 0;
};

// Owns the acceleration structures built over GpuScene geometry, the per-frame
// host-visible TLAS instance inputs, and the persistent TLAS rebuild scratch. BLASes
// are built once at startup; the TLAS is rebuilt in place every frame. Everything is
// swapchain-independent, so resize/recreate never replaces these resources. Its
// VkDevice is non-owning (borrowed from VulkanContext); the destructor waits for the
// device to go idle itself, so declaration order relative to other owners does not
// matter — only that it precedes the VulkanContext teardown, which declaring it after
// ctx in main() guarantees.
struct AccelerationStructure
{
    VkDevice device = VK_NULL_HANDLE;
    VmaAllocator allocator = nullptr;
    // vkDestroyAccelerationStructureKHR is an extension entry point resolved at runtime,
    // so the destructor cannot call it statically. Whoever creates an acceleration
    // structure handle below must set this first (alongside device).
    PFN_vkDestroyAccelerationStructureKHR destroyAccelerationStructure = nullptr;

    // One host-visible input buffer per frame-in-flight slot. The CPU rewrites only a
    // slot whose fence has completed, while the static Vulkan fields live in the
    // template and only transforms are refreshed from SceneData each frame.
    std::vector<TlasInstanceSlot> instanceSlots;
    std::vector<VkAccelerationStructureInstanceKHR> instanceTemplate;

    // One BLAS per SceneMesh. Entries retain the address referenced by every TLAS
    // instance so no temporary address side table can diverge from ownership.
    std::vector<BlasEntry> blases;

    // The TLAS handle is what the RT descriptor set will eventually bind
    // (VkWriteDescriptorSetAccelerationStructureKHR takes the handle, not an address).
    VkAccelerationStructureKHR tlas = VK_NULL_HANDLE;
    VkBuffer tlasBuffer = VK_NULL_HANDLE;
    VmaAllocation tlasBufferAllocation = nullptr;

    // One transient arena gives every concurrently batched BLAS a disjoint aligned
    // region. Ownership lives here so a failed startup build can still bare-return;
    // success releases it after the fence wait.
    VkBuffer scratchBuffer = VK_NULL_HANDLE;
    VmaAllocation scratchBufferAllocation = nullptr;

    // The shared TLAS is rebuilt serially, so one persistent aligned scratch region is
    // sufficient for every frame slot. The cached address is the aligned build address,
    // not necessarily the raw buffer base.
    VkBuffer tlasScratchBuffer = VK_NULL_HANDLE;
    VmaAllocation tlasScratchAllocation = nullptr;
    VkDeviceAddress tlasScratchAddress = 0;

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

// Populate *as: batch one BLAS build per assembled mesh over the borrowed GpuScene
// buffers, then build a TLAS over every assembled world instance. frameSlotCount must
// be nonzero; one persistently mapped instance input is created for each slot.
// The submission borrows commandBuffer/traceQueue/fence and blocks until the GPU
// finishes. The three build-input device addresses are checked against their
// required 4/4/16-byte alignments; an under-aligned base address fails with
// VK_ERROR_INITIALIZATION_FAILED. The BLAS scratch arena is released before returning;
// TLAS scratch remains program-lifetime for per-frame rebuilds. On success the fence is
// signaled for the first drawFrame wait. On failure the fence may be left unsignaled and
// *as holds whatever was created so far; ~AccelerationStructure cleans it up, so the
// caller can bare-return.
VkResult buildAccelerationStructures(
    AccelerationStructure* as,
    VkPhysicalDevice physicalDevice,
    VkDevice device,
    VmaAllocator allocator,
    const RayTracingFunctions& functions,
    const SceneData& scene,
    const GpuScene& gpuScene,
    uint32_t frameSlotCount,
    VkCommandBuffer commandBuffer,
    VkQueue traceQueue,
    VkFence fence);

// Refresh the transform field in the startup instance template and copy the complete
// records into frameSlot's mapped input. The caller must wait that slot's in-flight
// fence first. Runtime instance-count drift, an invalid slot, missing mapped storage,
// and a non-finite or singular transform fail without writing the mapped buffer.
bool writeTlasInstances(
    AccelerationStructure* as,
    const SceneData& scene,
    uint32_t frameSlot);

// Record the cross-frame pre-build dependency, one in-place TLAS BUILD using
// frameSlot's instance input and the persistent scratch, and the build-to-traversal
// dependency. Precondition: writeTlasInstances succeeded for this slot in this frame.
void recordTlasRebuild(
    VkCommandBuffer commandBuffer,
    const RayTracingFunctions& functions,
    const AccelerationStructure& as,
    uint32_t frameSlot);
}
