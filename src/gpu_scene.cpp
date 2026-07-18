#include "gpu_scene.hpp"

#include "scene.hpp"
#include "vulkan_context.hpp"
#include "vk_mem_alloc.h"

#include <cstring>
#include <iostream>
#include <limits>
#include <new>
#include <stdexcept>
#include <vector>

namespace xrphoton
{
namespace
{
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

GpuScene::~GpuScene()
{
    if (device == VK_NULL_HANDLE) {
        return;
    }

    (void)vkDeviceWaitIdle(device);
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

    VkPhysicalDeviceProperties physicalDeviceProperties{};
    vkGetPhysicalDeviceProperties(physicalDevice, &physicalDeviceProperties);

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

    VkResult result = uploadDeviceLocalBuffer(
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
