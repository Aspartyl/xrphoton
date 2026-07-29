#pragma once

#include "frame_lighting_layout.hpp"
#include "vma_fwd.hpp"

#include <cstdint>

#include <vulkan/vulkan.h>

namespace xrphoton
{
struct FrameLighting;

// GPU publication owner for lighting records. The one persistently mapped uniform
// buffer is partitioned by frame slot; callers may rewrite a slot only after its
// in-flight fence has signaled.
struct GpuLighting
{
    VkDevice device = VK_NULL_HANDLE;
    VmaAllocator allocator = nullptr;
    VkBuffer frameBuffer = VK_NULL_HANDLE;
    VmaAllocation frameAllocation = nullptr;
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
    std::uint32_t frameSlotCount);

// Copy a packed record into a retired frame slot and produce the checked 32-bit
// dynamic offset required by vkCmdBindDescriptorSets.
[[nodiscard]] bool writeFrameLightingSlot(
    GpuLighting* gpu,
    std::uint32_t frameSlot,
    const FrameLighting& lighting,
    std::uint32_t* dynamicOffset);
}
