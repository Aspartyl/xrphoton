#pragma once

#include "frame_lighting_layout.hpp"
#include "vma_fwd.hpp"

#include <cstdint>

#include <vulkan/vulkan.h>

namespace xrphoton
{
struct FrameLighting;
struct SceneLighting;

// GPU publication owner for lighting records. The persistently mapped uniform buffer
// is partitioned by frame slot; callers may rewrite a slot only after its in-flight
// fence has signaled. The three storage buffers are immutable after startup upload.
struct GpuLighting
{
    VkDevice device = VK_NULL_HANDLE;
    VmaAllocator allocator = nullptr;
    VkBuffer frameBuffer = VK_NULL_HANDLE;
    VmaAllocation frameAllocation = nullptr;
    VkBuffer lightBuffer = VK_NULL_HANDLE;
    VmaAllocation lightAllocation = nullptr;
    VkBuffer lightCdfBuffer = VK_NULL_HANDLE;
    VmaAllocation lightCdfAllocation = nullptr;
    VkBuffer emitterLookupBuffer = VK_NULL_HANDLE;
    VmaAllocation emitterLookupAllocation = nullptr;
    void* mappedFrameData = nullptr;
    FrameLightingBufferLayout frameLayout{};

    GpuLighting() = default;
    GpuLighting(const GpuLighting&) = delete;
    GpuLighting& operator=(const GpuLighting&) = delete;
    ~GpuLighting();
};

[[nodiscard]] VkResult createGpuLighting(
    GpuLighting* gpu,
    VkPhysicalDevice physicalDevice,
    VkDevice device,
    VmaAllocator allocator,
    std::uint32_t frameSlotCount,
    const SceneLighting& lighting,
    VkCommandBuffer commandBuffer,
    VkQueue traceQueue,
    VkFence fence);

// Copy a packed record into a retired frame slot and produce the checked 32-bit
// dynamic offset required by vkCmdBindDescriptorSets.
[[nodiscard]] bool writeFrameLightingSlot(
    GpuLighting* gpu,
    std::uint32_t frameSlot,
    const FrameLighting& lighting,
    std::uint32_t* dynamicOffset);
}
