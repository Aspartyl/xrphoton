#include "gpu_lighting.hpp"

#include "scene_lighting.hpp"
#include "vulkan_context.hpp"
#include "vk_mem_alloc.h"

#include <cstring>
#include <iostream>
#include <limits>

namespace xrphoton
{
GpuLighting::~GpuLighting()
{
    if (device == VK_NULL_HANDLE) {
        return;
    }

    (void)vkDeviceWaitIdle(device);
    if (frameBuffer != VK_NULL_HANDLE || frameAllocation != nullptr) {
        vmaDestroyBuffer(allocator, frameBuffer, frameAllocation);
        std::cout << "Destroyed Vulkan frame-lighting buffer.\n";
    }
}

VkResult createGpuLighting(
    GpuLighting* gpu,
    VkPhysicalDevice physicalDevice,
    VkDevice device,
    VmaAllocator allocator,
    std::uint32_t frameSlotCount)
{
    if (gpu == nullptr || device == VK_NULL_HANDLE || allocator == nullptr) {
        return VK_ERROR_INITIALIZATION_FAILED;
    }
    gpu->device = device;
    gpu->allocator = allocator;

    VkPhysicalDeviceProperties properties{};
    vkGetPhysicalDeviceProperties(physicalDevice, &properties);
    if (sizeof(FrameLighting) > properties.limits.maxUniformBufferRange) {
        std::cerr << "FrameLighting exceeds maxUniformBufferRange.\n";
        return VK_ERROR_FEATURE_NOT_PRESENT;
    }
    if (!makeFrameLightingBufferLayout(
            sizeof(FrameLighting),
            properties.limits.minUniformBufferOffsetAlignment,
            frameSlotCount,
            &gpu->frameLayout)
        || gpu->frameLayout.byteSize > std::numeric_limits<VkDeviceSize>::max()) {
        std::cerr << "Frame-lighting dynamic-buffer layout overflows VkDeviceSize.\n";
        return VK_ERROR_INITIALIZATION_FAILED;
    }
    std::uint32_t finalDynamicOffset = 0;
    if (!makeFrameLightingDynamicOffset(
            gpu->frameLayout,
            frameSlotCount - 1,
            &finalDynamicOffset)) {
        std::cerr << "Frame-lighting dynamic offset exceeds uint32_t.\n";
        return VK_ERROR_INITIALIZATION_FAILED;
    }

    const VkResult result = createBuffer(
        allocator,
        static_cast<VkDeviceSize>(gpu->frameLayout.byteSize),
        VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT,
        VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT
            | VMA_ALLOCATION_CREATE_MAPPED_BIT,
        &gpu->frameBuffer,
        &gpu->frameAllocation);
    if (result != VK_SUCCESS) {
        return result;
    }

    VmaAllocationInfo allocationInfo{};
    vmaGetAllocationInfo(allocator, gpu->frameAllocation, &allocationInfo);
    gpu->mappedFrameData = allocationInfo.pMappedData;
    if (gpu->mappedFrameData == nullptr) {
        return VK_ERROR_MEMORY_MAP_FAILED;
    }
    return VK_SUCCESS;
}

bool writeFrameLightingSlot(
    GpuLighting* gpu,
    std::uint32_t frameSlot,
    const FrameLighting& lighting,
    std::uint32_t* dynamicOffset)
{
    if (gpu == nullptr || gpu->mappedFrameData == nullptr
        || !hasValidFrameLightingFlags(lighting.flags)) {
        return false;
    }

    std::uint32_t checkedOffset = 0;
    if (!makeFrameLightingDynamicOffset(
            gpu->frameLayout,
            frameSlot,
            &checkedOffset)) {
        return false;
    }
    const std::uint64_t end =
        static_cast<std::uint64_t>(checkedOffset) + sizeof(FrameLighting);
    if (end > gpu->frameLayout.byteSize) {
        return false;
    }

    auto* destination = static_cast<unsigned char*>(gpu->mappedFrameData)
        + checkedOffset;
    std::memcpy(destination, &lighting, sizeof(lighting));
    if (dynamicOffset != nullptr) {
        *dynamicOffset = checkedOffset;
    }
    return true;
}
}
