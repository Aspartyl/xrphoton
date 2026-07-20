#include "gpu_scene.hpp"

#include "scene.hpp"
#include "vulkan_context.hpp"
#include "vk_mem_alloc.h"

#include <cstring>
#include <iostream>
#include <limits>
#include <new>
#include <stdexcept>
#include <type_traits>
#include <vector>

namespace xrphoton
{
namespace
{
VkFormat toVkFormat(SceneImageFormat format)
{
    switch (format) {
    case SceneImageFormat::Rgba8Srgb: return VK_FORMAT_R8G8B8A8_SRGB;
    case SceneImageFormat::Bc1RgbaSrgb: return VK_FORMAT_BC1_RGBA_SRGB_BLOCK;
    case SceneImageFormat::Bc3Srgb: return VK_FORMAT_BC3_SRGB_BLOCK;
    }
    return VK_FORMAT_UNDEFINED;
}

const char* sceneImageFormatName(SceneImageFormat format)
{
    switch (format) {
    case SceneImageFormat::Rgba8Srgb: return "Rgba8Srgb";
    case SceneImageFormat::Bc1RgbaSrgb: return "Bc1RgbaSrgb";
    case SceneImageFormat::Bc3Srgb: return "Bc3Srgb";
    }
    return "unknown";
}

bool expectedPixelBytes(const SceneImage& image, uint64_t* bytes)
{
    if (image.width == 0 || image.height == 0) {
        return false;
    }

    const uint64_t width = image.width;
    const uint64_t height = image.height;
    switch (image.format) {
    case SceneImageFormat::Rgba8Srgb:
        if (width > std::numeric_limits<uint64_t>::max() / height
            || width * height > std::numeric_limits<uint64_t>::max() / 4) {
            return false;
        }
        *bytes = width * height * 4;
        return true;
    case SceneImageFormat::Bc1RgbaSrgb:
    case SceneImageFormat::Bc3Srgb: {
        const uint64_t blockWidth = (width + 3) / 4;
        const uint64_t blockHeight = (height + 3) / 4;
        const uint64_t blockBytes = image.format == SceneImageFormat::Bc1RgbaSrgb
            ? 8
            : 16;
        if (blockWidth > std::numeric_limits<uint64_t>::max() / blockHeight
            || blockWidth * blockHeight
                > std::numeric_limits<uint64_t>::max() / blockBytes) {
            return false;
        }
        *bytes = blockWidth * blockHeight * blockBytes;
        return true;
    }
    }
    return false;
}

bool formatSupportsTextureProfile(VkPhysicalDevice physicalDevice, VkFormat format)
{
    VkFormatProperties properties{};
    vkGetPhysicalDeviceFormatProperties(physicalDevice, format, &properties);
    constexpr VkFormatFeatureFlags Required = VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT
        | VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT
        | VK_FORMAT_FEATURE_TRANSFER_DST_BIT;
    if ((properties.optimalTilingFeatures & Required) != Required) {
        return false;
    }

    VkImageFormatProperties imageProperties{};
    const VkResult result = vkGetPhysicalDeviceImageFormatProperties(
        physicalDevice,
        format,
        VK_IMAGE_TYPE_2D,
        VK_IMAGE_TILING_OPTIMAL,
        VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT,
        0,
        &imageProperties);
    return result == VK_SUCCESS
        && imageProperties.maxExtent.width >= 1
        && imageProperties.maxExtent.height >= 1
        && imageProperties.maxMipLevels >= 1
        && imageProperties.maxArrayLayers >= 1
        && (imageProperties.sampleCounts & VK_SAMPLE_COUNT_1_BIT) != 0;
}

VkDeviceAddress getBufferAddress(
    VkDevice device,
    const RayTracingFunctions& functions,
    VkBuffer buffer)
{
    VkBufferDeviceAddressInfo addressInfo{};
    addressInfo.sType = VK_STRUCTURE_TYPE_BUFFER_DEVICE_ADDRESS_INFO;
    addressInfo.buffer = buffer;
    return functions.getBufferDeviceAddress(device, &addressInfo);
}

void destroyBuffer(
    VmaAllocator allocator,
    VkBuffer buffer,
    VmaAllocation allocation,
    const char* name)
{
    if (buffer != VK_NULL_HANDLE || allocation != nullptr) {
        vmaDestroyBuffer(allocator, buffer, allocation);
        std::cout << "Destroyed Vulkan " << name << ".\n";
    }
}

struct TextureStagingBuffer
{
    VmaAllocator allocator = nullptr;
    VkBuffer buffer = VK_NULL_HANDLE;
    VmaAllocation allocation = nullptr;

    ~TextureStagingBuffer()
    {
        if (buffer != VK_NULL_HANDLE || allocation != nullptr) {
            vmaDestroyBuffer(allocator, buffer, allocation);
        }
    }
};

VkResult uploadSceneTexture(
    GpuScene* gpu,
    const SceneImage& source,
    VkPhysicalDevice physicalDevice,
    VkCommandBuffer commandBuffer,
    VkQueue traceQueue,
    VkFence fence)
{
    const VkFormat format = toVkFormat(source.format);
    if (format == VK_FORMAT_UNDEFINED) {
        std::cerr << "Scene image has an unknown format.\n";
        return VK_ERROR_FORMAT_NOT_SUPPORTED;
    }

    VkImageFormatProperties imageProperties{};
    const VkResult formatResult = vkGetPhysicalDeviceImageFormatProperties(
        physicalDevice,
        format,
        VK_IMAGE_TYPE_2D,
        VK_IMAGE_TILING_OPTIMAL,
        VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT,
        0,
        &imageProperties);
    if (formatResult != VK_SUCCESS) {
        std::cerr << "Scene image format " << sceneImageFormatName(source.format)
                  << " does not support the required 2D sampled-transfer tuple.\n";
        return formatResult;
    }
    if (source.width > imageProperties.maxExtent.width
        || source.height > imageProperties.maxExtent.height
        || imageProperties.maxMipLevels < 1
        || imageProperties.maxArrayLayers < 1
        || (imageProperties.sampleCounts & VK_SAMPLE_COUNT_1_BIT) == 0) {
        std::cerr << "Scene image " << source.width << 'x' << source.height
                  << " exceeds the format tuple's maxExtent "
                  << imageProperties.maxExtent.width << 'x'
                  << imageProperties.maxExtent.height << ".\n";
        return VK_ERROR_FORMAT_NOT_SUPPORTED;
    }

    VkImageCreateInfo imageCreateInfo{};
    imageCreateInfo.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    imageCreateInfo.imageType = VK_IMAGE_TYPE_2D;
    imageCreateInfo.format = format;
    imageCreateInfo.extent = {source.width, source.height, 1};
    imageCreateInfo.mipLevels = 1;
    imageCreateInfo.arrayLayers = 1;
    imageCreateInfo.samples = VK_SAMPLE_COUNT_1_BIT;
    imageCreateInfo.tiling = VK_IMAGE_TILING_OPTIMAL;
    imageCreateInfo.usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    imageCreateInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    imageCreateInfo.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;

    VmaAllocationCreateInfo allocationCreateInfo{};
    allocationCreateInfo.usage = VMA_MEMORY_USAGE_AUTO;
    allocationCreateInfo.requiredFlags = VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT;

    SceneTexture texture{};
    VkResult result = vmaCreateImage(
        gpu->allocator,
        &imageCreateInfo,
        &allocationCreateInfo,
        &texture.image,
        &texture.allocation,
        nullptr);
    if (result != VK_SUCCESS) {
        return result;
    }

    // The vector was reserved before any Vulkan creation, so adoption cannot throw.
    gpu->textures.push_back(texture);
    SceneTexture& ownedTexture = gpu->textures.back();

    TextureStagingBuffer staging{};
    staging.allocator = gpu->allocator;
    result = createBuffer(
        gpu->allocator,
        static_cast<VkDeviceSize>(source.pixels.size()),
        VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
        VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT
            | VMA_ALLOCATION_CREATE_MAPPED_BIT,
        &staging.buffer,
        &staging.allocation);
    if (result != VK_SUCCESS) {
        return result;
    }

    VmaAllocationInfo stagingInfo{};
    vmaGetAllocationInfo(gpu->allocator, staging.allocation, &stagingInfo);
    if (stagingInfo.pMappedData == nullptr) {
        return VK_ERROR_MEMORY_MAP_FAILED;
    }
    std::memcpy(stagingInfo.pMappedData, source.pixels.data(), source.pixels.size());

    result = vkResetCommandBuffer(commandBuffer, 0);
    if (result != VK_SUCCESS) {
        return result;
    }
    VkCommandBufferBeginInfo beginInfo{};
    beginInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    beginInfo.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    result = vkBeginCommandBuffer(commandBuffer, &beginInfo);
    if (result != VK_SUCCESS) {
        return result;
    }

    VkImageMemoryBarrier toTransfer{};
    toTransfer.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    toTransfer.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    toTransfer.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    toTransfer.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    toTransfer.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    toTransfer.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    toTransfer.image = ownedTexture.image;
    toTransfer.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    toTransfer.subresourceRange.levelCount = 1;
    toTransfer.subresourceRange.layerCount = 1;
    vkCmdPipelineBarrier(
        commandBuffer,
        VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
        VK_PIPELINE_STAGE_TRANSFER_BIT,
        0,
        0,
        nullptr,
        0,
        nullptr,
        1,
        &toTransfer);

    VkBufferImageCopy copyRegion{};
    copyRegion.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    copyRegion.imageSubresource.layerCount = 1;
    copyRegion.imageExtent = {source.width, source.height, 1};
    vkCmdCopyBufferToImage(
        commandBuffer,
        staging.buffer,
        ownedTexture.image,
        VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
        1,
        &copyRegion);

    VkImageMemoryBarrier toShaderRead = toTransfer;
    toShaderRead.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    toShaderRead.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
    toShaderRead.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    toShaderRead.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    vkCmdPipelineBarrier(
        commandBuffer,
        VK_PIPELINE_STAGE_TRANSFER_BIT,
        VK_PIPELINE_STAGE_RAY_TRACING_SHADER_BIT_KHR,
        0,
        0,
        nullptr,
        0,
        nullptr,
        1,
        &toShaderRead);

    result = vkEndCommandBuffer(commandBuffer);
    if (result != VK_SUCCESS) {
        return result;
    }
    result = vkResetFences(gpu->device, 1, &fence);
    if (result != VK_SUCCESS) {
        return result;
    }
    VkSubmitInfo submitInfo{};
    submitInfo.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    submitInfo.commandBufferCount = 1;
    submitInfo.pCommandBuffers = &commandBuffer;
    result = vkQueueSubmit(traceQueue, 1, &submitInfo, fence);
    if (result != VK_SUCCESS) {
        return result;
    }
    result = vkWaitForFences(
        gpu->device,
        1,
        &fence,
        VK_TRUE,
        std::numeric_limits<uint64_t>::max());
    if (result != VK_SUCCESS) {
        // The local staging owner cannot outlive this call. Retire potentially pending
        // work before it releases memory the queue may still reference.
        (void)vkDeviceWaitIdle(gpu->device);
        return result;
    }
    return VK_SUCCESS;
}

bool hasScalarLoadAlignment(VkDeviceAddress address, const char* name)
{
    constexpr VkDeviceSize ScalarAlignment = 4;
    if ((address % ScalarAlignment) == 0) {
        return true;
    }

    std::cerr << "Vulkan " << name << " device address is not 4-byte aligned.\n";
    return false;
}

bool checkedRecordBufferSize(
    std::size_t recordCount,
    VkDeviceSize recordSize,
    const char* name,
    VkDeviceSize maximumRange,
    VkDeviceSize* byteSize)
{
    if (recordCount > std::numeric_limits<VkDeviceSize>::max() / recordSize) {
        std::cerr << "Scene " << name << " byte size overflows VkDeviceSize.\n";
        return false;
    }

    *byteSize = static_cast<VkDeviceSize>(recordCount) * recordSize;
    if (*byteSize > maximumRange) {
        std::cerr << "Scene " << name << " requires " << *byteSize
                  << " bytes, exceeding maxStorageBufferRange "
                  << maximumRange << ".\n";
        return false;
    }
    return true;
}
}

bool hasRequiredSceneTextureDescriptorLimits(const VkPhysicalDeviceLimits& limits)
{
    return limits.maxPerStageDescriptorSampledImages >= MaxSceneTextures
        && limits.maxPerStageDescriptorSamplers >= MaxSceneTextures
        && limits.maxDescriptorSetSampledImages >= MaxSceneTextures
        && limits.maxDescriptorSetSamplers >= MaxSceneTextures
        && limits.maxPerStageResources >= MaxSceneTextures + 2;
}

SceneTextureFormatSupport queryRequiredSceneTextureFormatSupport(
    VkPhysicalDevice physicalDevice)
{
    return {
        .rgba8Srgb =
            formatSupportsTextureProfile(physicalDevice, VK_FORMAT_R8G8B8A8_SRGB),
        .bc1RgbaSrgb =
            formatSupportsTextureProfile(physicalDevice, VK_FORMAT_BC1_RGBA_SRGB_BLOCK),
        .bc3Srgb =
            formatSupportsTextureProfile(physicalDevice, VK_FORMAT_BC3_SRGB_BLOCK),
    };
}

GpuScene::~GpuScene()
{
    if (device == VK_NULL_HANDLE) {
        return;
    }

    (void)vkDeviceWaitIdle(device);
    if (textureSampler != VK_NULL_HANDLE) {
        vkDestroySampler(device, textureSampler, nullptr);
        std::cout << "Destroyed Vulkan scene texture sampler.\n";
    }
    for (auto texture = textures.rbegin(); texture != textures.rend(); ++texture) {
        if (texture->view != VK_NULL_HANDLE) {
            vkDestroyImageView(device, texture->view, nullptr);
        }
    }
    for (auto texture = textures.rbegin(); texture != textures.rend(); ++texture) {
        if (texture->image != VK_NULL_HANDLE || texture->allocation != nullptr) {
            vmaDestroyImage(allocator, texture->image, texture->allocation);
        }
    }
    destroyBuffer(allocator, materialBuffer, materialAllocation, "material buffer");
    destroyBuffer(
        allocator,
        geometryRecordBuffer,
        geometryRecordAllocation,
        "geometry-record buffer");
    destroyBuffer(allocator, indexBuffer, indexAllocation, "scene index buffer");
    destroyBuffer(allocator, attributeBuffer, attributeAllocation, "scene attribute buffer");
    destroyBuffer(allocator, positionBuffer, positionAllocation, "scene position buffer");
}

VkResult createGpuScene(
    GpuScene* gpu,
    const SceneData& scene,
    VkPhysicalDevice physicalDevice,
    VkDevice device,
    VmaAllocator allocator,
    const RayTracingFunctions& functions,
    VkCommandBuffer commandBuffer,
    VkQueue traceQueue,
    VkFence fence)
{
    gpu->device = device;
    gpu->allocator = allocator;

    if (scene.positions.empty()
        || scene.attributes.empty()
        || scene.indices.empty()
        || scene.geometries.empty()
        || scene.materials.empty()) {
        std::cerr << "Cannot create a GPU scene from empty geometry or material data.\n";
        return VK_ERROR_INITIALIZATION_FAILED;
    }

    if (scene.images.empty()) {
        std::cerr << "Cannot create a GPU scene without the fallback scene image.\n";
        return VK_ERROR_INITIALIZATION_FAILED;
    }
    if (scene.images.size() > MaxSceneTextures) {
        std::cerr << "Scene image count " << scene.images.size()
                  << " exceeds MaxSceneTextures " << MaxSceneTextures << ".\n";
        return VK_ERROR_INITIALIZATION_FAILED;
    }
    for (std::size_t index = 0; index < scene.materials.size(); ++index) {
        if (scene.materials[index].baseColorImage >= scene.images.size()) {
            std::cerr << "Scene material[" << index << "].baseColorImage "
                      << scene.materials[index].baseColorImage << " is outside "
                      << scene.images.size() << " scene images.\n";
            return VK_ERROR_INITIALIZATION_FAILED;
        }
    }
    for (std::size_t index = 0; index < scene.images.size(); ++index) {
        uint64_t expectedBytes = 0;
        const SceneImage& image = scene.images[index];
        if (!expectedPixelBytes(image, &expectedBytes)
            || expectedBytes != image.pixels.size()) {
            std::cerr << "Scene image[" << index << "] "
                      << sceneImageFormatName(image.format)
                      << " pixel size does not match its dimensions.\n";
            return VK_ERROR_INITIALIZATION_FAILED;
        }
    }

    VkPhysicalDeviceProperties physicalDeviceProperties{};
    vkGetPhysicalDeviceProperties(physicalDevice, &physicalDeviceProperties);
    if (!hasRequiredSceneTextureDescriptorLimits(physicalDeviceProperties.limits)) {
        std::cerr << "Physical device no longer satisfies the fixed scene texture "
                     "descriptor limits checked during device selection.\n";
        return VK_ERROR_INITIALIZATION_FAILED;
    }
    if (physicalDeviceProperties.limits.maxSamplerAnisotropy
        < SceneTextureAnisotropy) {
        std::cerr << "Physical device maxSamplerAnisotropy "
                  << physicalDeviceProperties.limits.maxSamplerAnisotropy
                  << " is below the required " << SceneTextureAnisotropy << "x.\n";
        return VK_ERROR_FEATURE_NOT_PRESENT;
    }
    if (!queryRequiredSceneTextureFormatSupport(physicalDevice).isComplete()) {
        std::cerr << "Physical device no longer satisfies the scene texture format "
                     "profile checked during device selection.\n";
        return VK_ERROR_FORMAT_NOT_SUPPORTED;
    }

    try {
        gpu->textures.reserve(scene.images.size());
    } catch (const std::bad_alloc&) {
        return VK_ERROR_OUT_OF_HOST_MEMORY;
    } catch (const std::length_error&) {
        return VK_ERROR_OUT_OF_HOST_MEMORY;
    }
    static_assert(std::is_nothrow_move_constructible_v<SceneTexture>);

    for (const SceneImage& image : scene.images) {
        const VkResult textureResult = uploadSceneTexture(
            gpu,
            image,
            physicalDevice,
            commandBuffer,
            traceQueue,
            fence);
        if (textureResult != VK_SUCCESS) {
            return textureResult;
        }
    }

    for (std::size_t index = 0; index < scene.images.size(); ++index) {
        VkImageViewCreateInfo viewCreateInfo{};
        viewCreateInfo.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
        viewCreateInfo.image = gpu->textures[index].image;
        viewCreateInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
        viewCreateInfo.format = toVkFormat(scene.images[index].format);
        viewCreateInfo.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        viewCreateInfo.subresourceRange.levelCount = 1;
        viewCreateInfo.subresourceRange.layerCount = 1;
        const VkResult viewResult = vkCreateImageView(
            device,
            &viewCreateInfo,
            nullptr,
            &gpu->textures[index].view);
        if (viewResult != VK_SUCCESS) {
            return viewResult;
        }
    }

    VkSamplerCreateInfo samplerCreateInfo{};
    samplerCreateInfo.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
    samplerCreateInfo.magFilter = VK_FILTER_LINEAR;
    samplerCreateInfo.minFilter = VK_FILTER_LINEAR;
    samplerCreateInfo.mipmapMode = VK_SAMPLER_MIPMAP_MODE_LINEAR;
    samplerCreateInfo.addressModeU = VK_SAMPLER_ADDRESS_MODE_REPEAT;
    samplerCreateInfo.addressModeV = VK_SAMPLER_ADDRESS_MODE_REPEAT;
    samplerCreateInfo.addressModeW = VK_SAMPLER_ADDRESS_MODE_REPEAT;
    samplerCreateInfo.anisotropyEnable = VK_TRUE;
    samplerCreateInfo.maxAnisotropy = SceneTextureAnisotropy;
    // The current profile deliberately retains and uploads only mip 0. Explicit
    // ray gradients still let anisotropy filter that level along an oblique
    // footprint, while this clamp prevents access to nonexistent levels.
    samplerCreateInfo.maxLod = 0.0f;
    VkResult result = vkCreateSampler(
        device,
        &samplerCreateInfo,
        nullptr,
        &gpu->textureSampler);
    if (result != VK_SUCCESS) {
        return result;
    }

    if ((scene.positions.size() % 3) != 0
        || scene.attributes.size() != scene.positions.size() / 3) {
        std::cerr << "Scene position and attribute counts do not describe the same vertices.\n";
        return VK_ERROR_INITIALIZATION_FAILED;
    }

    for (const SceneGeometry& geometry : scene.geometries) {
        const uint64_t vertexEnd = static_cast<uint64_t>(geometry.firstVertex)
            + geometry.vertexCount;
        const uint64_t indexEnd = static_cast<uint64_t>(geometry.firstIndex)
            + geometry.indexCount;
        if (vertexEnd > scene.attributes.size()
            || indexEnd > scene.indices.size()
            || geometry.materialIndex >= scene.materials.size()) {
            std::cerr << "Scene geometry range exceeds its vertex, index, or material data.\n";
            return VK_ERROR_INITIALIZATION_FAILED;
        }

        for (uint64_t index = geometry.firstIndex; index < indexEnd; ++index) {
            if (scene.indices[index] >= geometry.vertexCount) {
                std::cerr << "Scene geometry contains an out-of-range local index.\n";
                return VK_ERROR_INITIALIZATION_FAILED;
            }
        }
    }

    VkDeviceSize geometryRecordBytes = 0;
    VkDeviceSize materialRecordBytes = 0;
    if (!checkedRecordBufferSize(
            scene.geometries.size(),
            sizeof(GeometryRecord),
            "geometry-record buffer",
            physicalDeviceProperties.limits.maxStorageBufferRange,
            &geometryRecordBytes)
        || !checkedRecordBufferSize(
            scene.materials.size(),
            sizeof(MaterialRecord),
            "material buffer",
            physicalDeviceProperties.limits.maxStorageBufferRange,
            &materialRecordBytes)) {
        return VK_ERROR_INITIALIZATION_FAILED;
    }

    result = uploadDeviceLocalBuffer(
        allocator,
        device,
        commandBuffer,
        traceQueue,
        fence,
        scene.positions.data(),
        scene.positions.size() * sizeof(float),
        VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_BUILD_INPUT_READ_ONLY_BIT_KHR
            | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT
            | VK_BUFFER_USAGE_TRANSFER_DST_BIT,
        &gpu->positionBuffer,
        &gpu->positionAllocation);
    if (result != VK_SUCCESS) {
        return result;
    }

    result = uploadDeviceLocalBuffer(
        allocator,
        device,
        commandBuffer,
        traceQueue,
        fence,
        scene.attributes.data(),
        scene.attributes.size() * sizeof(VertexAttributes),
        VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT,
        &gpu->attributeBuffer,
        &gpu->attributeAllocation);
    if (result != VK_SUCCESS) {
        return result;
    }

    result = uploadDeviceLocalBuffer(
        allocator,
        device,
        commandBuffer,
        traceQueue,
        fence,
        scene.indices.data(),
        scene.indices.size() * sizeof(uint32_t),
        VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_BUILD_INPUT_READ_ONLY_BIT_KHR
            | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT
            | VK_BUFFER_USAGE_TRANSFER_DST_BIT,
        &gpu->indexBuffer,
        &gpu->indexAllocation);
    if (result != VK_SUCCESS) {
        return result;
    }

    gpu->positionBufferAddress = getBufferAddress(device, functions, gpu->positionBuffer);
    gpu->attributeBufferAddress = getBufferAddress(device, functions, gpu->attributeBuffer);
    gpu->indexBufferAddress = getBufferAddress(device, functions, gpu->indexBuffer);

    if (!hasScalarLoadAlignment(gpu->positionBufferAddress, "position buffer")
        || !hasScalarLoadAlignment(gpu->attributeBufferAddress, "attribute buffer")
        || !hasScalarLoadAlignment(gpu->indexBufferAddress, "index buffer")) {
        return VK_ERROR_INITIALIZATION_FAILED;
    }

    std::vector<GeometryRecord> geometryRecords;
    std::vector<MaterialRecord> materialRecords;
    try {
        geometryRecords.reserve(scene.geometries.size());
        for (const SceneGeometry& geometry : scene.geometries) {
            geometryRecords.push_back({
                .indexAddress = gpu->indexBufferAddress
                    + static_cast<VkDeviceSize>(geometry.firstIndex) * sizeof(uint32_t),
                .positionAddress = gpu->positionBufferAddress
                    + static_cast<VkDeviceSize>(geometry.firstVertex) * 3 * sizeof(float),
                .attributeAddress = gpu->attributeBufferAddress
                    + static_cast<VkDeviceSize>(geometry.firstVertex) * sizeof(VertexAttributes),
                .materialIndex = geometry.materialIndex,
            });
        }

        materialRecords.resize(scene.materials.size());
    } catch (const std::bad_alloc&) {
        return VK_ERROR_OUT_OF_HOST_MEMORY;
    } catch (const std::length_error&) {
        return VK_ERROR_OUT_OF_HOST_MEMORY;
    }

    result = uploadDeviceLocalBuffer(
        allocator,
        device,
        commandBuffer,
        traceQueue,
        fence,
        geometryRecords.data(),
        geometryRecordBytes,
        VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT,
        &gpu->geometryRecordBuffer,
        &gpu->geometryRecordAllocation);
    if (result != VK_SUCCESS) {
        return result;
    }

    for (std::size_t index = 0; index < scene.materials.size(); ++index) {
        const SceneMaterial& source = scene.materials[index];
        MaterialRecord& destination = materialRecords[index];
        std::memcpy(
            destination.baseColorFactor,
            source.baseColorFactor,
            sizeof(source.baseColorFactor));
        destination.baseColorTexture = source.baseColorImage;
        destination.alphaCutoff = source.alphaCutoff;
    }

    return uploadDeviceLocalBuffer(
        allocator,
        device,
        commandBuffer,
        traceQueue,
        fence,
        materialRecords.data(),
        materialRecordBytes,
        VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT,
        &gpu->materialBuffer,
        &gpu->materialAllocation);
}
}
