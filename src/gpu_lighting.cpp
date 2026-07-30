#include "gpu_lighting.hpp"

#include "scene_lighting.hpp"
#include "vulkan_context.hpp"
#include "vk_mem_alloc.h"

#include <algorithm>
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
    if (lightBuffer != VK_NULL_HANDLE || lightAllocation != nullptr) {
        vmaDestroyBuffer(allocator, lightBuffer, lightAllocation);
    }
    if (lightCdfBuffer != VK_NULL_HANDLE || lightCdfAllocation != nullptr) {
        vmaDestroyBuffer(allocator, lightCdfBuffer, lightCdfAllocation);
    }
    if (emitterLookupBuffer != VK_NULL_HANDLE
        || emitterLookupAllocation != nullptr) {
        vmaDestroyBuffer(
            allocator,
            emitterLookupBuffer,
            emitterLookupAllocation);
    }
    if (lightBuffer != VK_NULL_HANDLE || lightCdfBuffer != VK_NULL_HANDLE
        || emitterLookupBuffer != VK_NULL_HANDLE) {
        std::cout << "Destroyed Vulkan static-light buffers.\n";
    }
}

VkResult createGpuLighting(
    GpuLighting* gpu,
    VkPhysicalDevice physicalDevice,
    VkDevice device,
    VmaAllocator allocator,
    std::uint32_t frameSlotCount,
    const SceneLighting& lighting,
    VkCommandBuffer commandBuffer,
    VkQueue traceQueue,
    VkFence fence)
{
    if (gpu == nullptr || device == VK_NULL_HANDLE || allocator == nullptr
        || commandBuffer == VK_NULL_HANDLE || traceQueue == VK_NULL_HANDLE
        || fence == VK_NULL_HANDLE) {
        return VK_ERROR_INITIALIZATION_FAILED;
    }
    gpu->device = device;
    gpu->allocator = allocator;

    FrameLighting checkedLighting;
    if (!makeFrameLighting(lighting, lighting.instanceCount, &checkedLighting)) {
        std::cerr << "Static lighting tables are invalid.\n";
        return VK_ERROR_INITIALIZATION_FAILED;
    }

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

    const LightRecord lightSentinel{};
    const float cdfSentinel = 0.0f;
    const EmitterLookupRecord lookupSentinel{};
    const void* lightData = lighting.lights.empty()
        ? static_cast<const void*>(&lightSentinel)
        : static_cast<const void*>(lighting.lights.data());
    const void* cdfData = lighting.lightCdf.empty()
        ? static_cast<const void*>(&cdfSentinel)
        : static_cast<const void*>(lighting.lightCdf.data());
    const void* lookupData = lighting.emitterLookup.empty()
        ? static_cast<const void*>(&lookupSentinel)
        : static_cast<const void*>(lighting.emitterLookup.data());
    const std::size_t lightCount = std::max<std::size_t>(lighting.lights.size(), 1);
    const std::size_t cdfCount = std::max<std::size_t>(lighting.lightCdf.size(), 1);
    const std::size_t lookupCount =
        std::max<std::size_t>(lighting.emitterLookup.size(), 1);
    if (lightCount > std::numeric_limits<VkDeviceSize>::max() / sizeof(LightRecord)
        || cdfCount > std::numeric_limits<VkDeviceSize>::max() / sizeof(float)
        || lookupCount > std::numeric_limits<VkDeviceSize>::max()
            / sizeof(EmitterLookupRecord)) {
        return VK_ERROR_INITIALIZATION_FAILED;
    }
    const VkDeviceSize lightBytes = lightCount * sizeof(LightRecord);
    const VkDeviceSize cdfBytes = cdfCount * sizeof(float);
    const VkDeviceSize lookupBytes = lookupCount * sizeof(EmitterLookupRecord);
    if (lightBytes > properties.limits.maxStorageBufferRange
        || cdfBytes > properties.limits.maxStorageBufferRange
        || lookupBytes > properties.limits.maxStorageBufferRange) {
        std::cerr << "Static lighting exceeds maxStorageBufferRange.\n";
        return VK_ERROR_FEATURE_NOT_PRESENT;
    }

    constexpr VkBufferUsageFlags StaticLightUsage =
        VK_BUFFER_USAGE_TRANSFER_DST_BIT | VK_BUFFER_USAGE_STORAGE_BUFFER_BIT;
    VkResult uploadResult = uploadDeviceLocalBuffer(
        allocator,
        device,
        commandBuffer,
        traceQueue,
        fence,
        lightData,
        lightBytes,
        StaticLightUsage,
        &gpu->lightBuffer,
        &gpu->lightAllocation);
    if (uploadResult != VK_SUCCESS) {
        return uploadResult;
    }
    uploadResult = uploadDeviceLocalBuffer(
        allocator,
        device,
        commandBuffer,
        traceQueue,
        fence,
        cdfData,
        cdfBytes,
        StaticLightUsage,
        &gpu->lightCdfBuffer,
        &gpu->lightCdfAllocation);
    if (uploadResult != VK_SUCCESS) {
        return uploadResult;
    }
    return uploadDeviceLocalBuffer(
        allocator,
        device,
        commandBuffer,
        traceQueue,
        fence,
        lookupData,
        lookupBytes,
        StaticLightUsage,
        &gpu->emitterLookupBuffer,
        &gpu->emitterLookupAllocation);
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
