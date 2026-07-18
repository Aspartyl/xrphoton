#include "acceleration_structure.hpp"

#include "gpu_scene.hpp"
#include "scene.hpp"
#include "vulkan_context.hpp"
#include "vk_mem_alloc.h"

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <iomanip>
#include <iostream>
#include <limits>
#include <new>
#include <stdexcept>
#include <type_traits>
#include <vector>

#include <glm/mat4x4.hpp>
#include <vulkan/vulkan.h>

namespace xrphoton
{
namespace
{
// The vertex format the BLAS build declares. Defined once so the suitability gate
// and the build input can never diverge (the StorageImageFormat pattern from
// swapchain.cpp).
constexpr VkFormat BlasVertexFormat = VK_FORMAT_R32G32B32_SFLOAT;

// The TLAS instance buffer is readable by the build and addressable because the build
// consumes a device address rather than a descriptor.
constexpr VkBufferUsageFlags BuildInputBufferUsage =
    VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_BUILD_INPUT_READ_ONLY_BIT_KHR
    | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT;

// Command-specific address alignments for the current build inputs: a component of
// R32G32B32_SFLOAT, a UINT32 index, and a non-pointer instance array respectively.
// Correct buffer usage does not itself guarantee these alignments for the base BDA.
constexpr VkDeviceSize VertexInputAddressAlignment = 4;
constexpr VkDeviceSize IndexInputAddressAlignment = 4;
constexpr VkDeviceSize InstanceInputAddressAlignment = 16;

// VkTransformMatrixKHR stores the top three rows of a row-major 4x4. GLM
// indexes its column-major matrices as [column][row], so transpose while
// copying at the one boundary where scene transforms enter Vulkan.
VkTransformMatrixKHR toVkTransformMatrix(const glm::mat4& matrix)
{
    VkTransformMatrixKHR transform{};

    for (uint32_t row = 0; row < 3; ++row) {
        for (uint32_t column = 0; column < 4; ++column) {
            transform.matrix[row][column] = matrix[column][row];
        }
    }

    return transform;
}

bool checkedAdd(VkDeviceSize left, VkDeviceSize right, VkDeviceSize* sum)
{
    if (right > std::numeric_limits<VkDeviceSize>::max() - left) {
        return false;
    }
    *sum = left + right;
    return true;
}

// Alignment applies to the device address handed to Vulkan, not merely to the
// allocation offset. The division form handles any positive alignment and lets the
// addition fail explicitly instead of wrapping at the address-space boundary.
bool checkedAlignUp(
    VkDeviceSize value,
    VkDeviceSize alignment,
    VkDeviceSize* alignedValue)
{
    if (alignment == 0) {
        return false;
    }
    const VkDeviceSize remainder = value % alignment;
    if (remainder == 0) {
        *alignedValue = value;
        return true;
    }
    return checkedAdd(value, alignment - remainder, alignedValue);
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

// Fail loudly instead of recording a build with undefined behavior. Real geometry
// suballocation will align the offsets themselves; dedicated startup buffers only need
// this guard against an unexpectedly under-aligned base address.
bool hasRequiredBuildInputAlignment(
    VkDeviceAddress address,
    VkDeviceSize requiredAlignment,
    const char* inputName)
{
    if ((address % requiredAlignment) == 0) {
        return true;
    }

    std::cerr << "Vulkan " << inputName << " build-input device address 0x"
              << std::hex << address << std::dec
              << " is not aligned to " << requiredAlignment << " bytes.\n";
    return false;
}

// Create a host-visible buffer and copy `size` bytes of `data` into it. Coherent memory
// so the writes need no explicit flush: the queue submission that consumes the buffer
// makes them visible to the device.
VkResult createHostVisibleBuffer(
    VmaAllocator allocator,
    const void* data,
    VkDeviceSize size,
    VkBufferUsageFlags usage,
    VkBuffer* buffer,
    VmaAllocation* allocation)
{
    VkResult result = createBuffer(
        allocator,
        size,
        usage,
        VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT
            | VMA_ALLOCATION_CREATE_MAPPED_BIT,
        buffer,
        allocation);

    if (result != VK_SUCCESS) {
        return result;
    }

    VmaAllocationInfo allocationInfo{};
    vmaGetAllocationInfo(allocator, *allocation, &allocationInfo);
    if (allocationInfo.pMappedData == nullptr) {
        return VK_ERROR_MEMORY_MAP_FAILED;
    }

    std::memcpy(allocationInfo.pMappedData, data, size);

    return VK_SUCCESS;
}

// Create one acceleration-structure handle directly on its owner-held backing buffer.
// Scratch is deliberately separate: every BLAS is built concurrently from one arena.
VkResult createAccelerationStructureBacking(
    VmaAllocator allocator,
    VkDevice device,
    const RayTracingFunctions& functions,
    VkAccelerationStructureTypeKHR type,
    const VkAccelerationStructureBuildSizesInfoKHR& sizes,
    VkBufferUsageFlags backingBufferUsage,
    VkBuffer* backingBuffer,
    VmaAllocation* backingBufferAllocation,
    VkAccelerationStructureKHR* accelerationStructure)
{
    VkResult result = createBuffer(
        allocator,
        sizes.accelerationStructureSize,
        backingBufferUsage,
        0,
        backingBuffer,
        backingBufferAllocation);

    if (result != VK_SUCCESS) {
        return result;
    }

    VkAccelerationStructureCreateInfoKHR createInfo{};
    createInfo.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_CREATE_INFO_KHR;
    createInfo.buffer = *backingBuffer;
    createInfo.offset = 0;
    createInfo.size = sizes.accelerationStructureSize;
    createInfo.type = type;

    return functions.createAccelerationStructure(
        device,
        &createInfo,
        nullptr,
        accelerationStructure);
}
// Destroy a buffer and its VMA allocation, logging once if either existed.
void destroyBufferAndAllocation(
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

} // namespace

bool hasRequiredAccelerationStructureFormatSupport(VkPhysicalDevice physicalDevice)
{
    VkFormatProperties formatProperties{};
    vkGetPhysicalDeviceFormatProperties(
        physicalDevice,
        BlasVertexFormat,
        &formatProperties);

    return (formatProperties.bufferFeatures
        & VK_FORMAT_FEATURE_ACCELERATION_STRUCTURE_VERTEX_BUFFER_BIT_KHR) != 0;
}

AccelerationStructure::~AccelerationStructure()
{
    // A default-constructed owner never received a device and owns nothing.
    if (device == VK_NULL_HANDLE) {
        return;
    }

    // Self-contained idle wait: nothing here may still be referenced by in-flight work,
    // and this owner must not depend on another owner's destructor having waited first.
    (void)vkDeviceWaitIdle(device);

    // Every acceleration-structure handle is placed on its backing buffer, so destroy
    // all handles before releasing any of the storage under them. TLAS first also
    // removes the world-level references to the BLAS addresses before those BLASes go.
    if (tlas != VK_NULL_HANDLE && destroyAccelerationStructure != nullptr) {
        destroyAccelerationStructure(device, tlas, nullptr);
        std::cout << "Destroyed Vulkan top-level acceleration structure.\n";
    }

    for (BlasEntry& entry : blases) {
        if (entry.blas != VK_NULL_HANDLE && destroyAccelerationStructure != nullptr) {
            destroyAccelerationStructure(device, entry.blas, nullptr);
            std::cout << "Destroyed Vulkan bottom-level acceleration structure.\n";
        }
    }

    destroyBufferAndAllocation(allocator, tlasBuffer, tlasBufferAllocation, "TLAS backing buffer");
    for (BlasEntry& entry : blases) {
        destroyBufferAndAllocation(
            allocator,
            entry.buffer,
            entry.allocation,
            "BLAS backing buffer");
    }
    destroyBufferAndAllocation(
        allocator,
        scratchBuffer,
        scratchBufferAllocation,
        "acceleration-structure scratch arena");
    destroyBufferAndAllocation(allocator, instanceBuffer, instanceBufferAllocation, "instance buffer");
}

VkResult buildAccelerationStructures(
    AccelerationStructure* as,
    VkPhysicalDevice physicalDevice,
    VkDevice device,
    VmaAllocator allocator,
    const RayTracingFunctions& functions,
    const SceneData& scene,
    const GpuScene& gpuScene,
    VkCommandBuffer commandBuffer,
    VkQueue traceQueue,
    VkFence fence)
{
    if (as == nullptr) {
        std::cerr << "Cannot build acceleration structures into a null owner.\n";
        return VK_ERROR_INITIALIZATION_FAILED;
    }

    // Adopt the device and the destroy entry point first, so every resource created
    // below — including a partial set on a failure path — is torn down by the
    // destructor (see the header contract on destroyAccelerationStructure).
    as->device = device;
    as->allocator = allocator;
    as->destroyAccelerationStructure = functions.destroyAccelerationStructure;

    try {
        if (scene.meshes.empty() || scene.instances.empty()) {
            std::cerr << "Cannot build acceleration structures for an empty mesh or instance set.\n";
            return VK_ERROR_INITIALIZATION_FAILED;
        }
        if (scene.meshes.size() > std::numeric_limits<uint32_t>::max()) {
            std::cerr << "Scene mesh count exceeds the Vulkan batched-build count field.\n";
            return VK_ERROR_INITIALIZATION_FAILED;
        }
        if (scene.instances.size() > std::numeric_limits<uint32_t>::max()) {
            std::cerr << "Scene instance count exceeds the Vulkan TLAS primitive-count field.\n";
            return VK_ERROR_INITIALIZATION_FAILED;
        }

        VkPhysicalDeviceAccelerationStructurePropertiesKHR accelerationStructureProperties{};
        accelerationStructureProperties.sType =
            VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ACCELERATION_STRUCTURE_PROPERTIES_KHR;

        VkPhysicalDeviceProperties2 physicalDeviceProperties{};
        physicalDeviceProperties.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2;
        physicalDeviceProperties.pNext = &accelerationStructureProperties;
        vkGetPhysicalDeviceProperties2(physicalDevice, &physicalDeviceProperties);

        if (static_cast<uint64_t>(scene.instances.size())
            > accelerationStructureProperties.maxInstanceCount) {
            std::cerr << "Scene instance count " << scene.instances.size()
                      << " exceeds maxInstanceCount "
                      << accelerationStructureProperties.maxInstanceCount << ".\n";
            return VK_ERROR_INITIALIZATION_FAILED;
        }

        const VkDeviceSize scratchAlignment =
            accelerationStructureProperties.minAccelerationStructureScratchOffsetAlignment;
        if (scratchAlignment == 0) {
            std::cerr << "Vulkan reported a zero acceleration-structure scratch alignment.\n";
            return VK_ERROR_INITIALIZATION_FAILED;
        }

        const std::size_t meshCount = scene.meshes.size();
        const std::size_t instanceCountSize = scene.instances.size();
        if (instanceCountSize
            > std::numeric_limits<VkDeviceSize>::max()
                / sizeof(VkAccelerationStructureInstanceKHR)) {
            std::cerr << "Scene instance-buffer size overflows VkDeviceSize.\n";
            return VK_ERROR_INITIALIZATION_FAILED;
        }
        const VkDeviceSize instanceBufferSize =
            static_cast<VkDeviceSize>(instanceCountSize)
            * sizeof(VkAccelerationStructureInstanceKHR);

        // Allocate every CPU container before the first Vulkan resource is created.
        // After this preparation, reserved BLAS adoption and all metadata writes are
        // non-allocating, so an exception cannot strand a fresh handle outside `as`.
        as->blases.reserve(meshCount);
        std::vector<std::vector<VkAccelerationStructureGeometryKHR>> blasGeometries(
            meshCount);
        std::vector<std::vector<uint32_t>> blasPrimitiveCounts(meshCount);
        std::vector<std::vector<VkAccelerationStructureBuildRangeInfoKHR>> blasRanges(
            meshCount);
        std::vector<VkAccelerationStructureBuildGeometryInfoKHR> blasBuildInfos(
            meshCount);
        std::vector<VkAccelerationStructureBuildSizesInfoKHR> blasSizes(meshCount);
        std::vector<const VkAccelerationStructureBuildRangeInfoKHR*> blasRangePointers(
            meshCount);
        std::vector<VkDeviceSize> blasScratchOffsets(meshCount);
        std::vector<VkAccelerationStructureInstanceKHR> vkInstances(instanceCountSize);

        VkDeviceSize alignedBlasScratchTotal = 0;
        for (std::size_t meshIndex = 0; meshIndex < meshCount; ++meshIndex) {
            const SceneMesh& mesh = scene.meshes[meshIndex];
            const uint64_t geometryEnd =
                static_cast<uint64_t>(mesh.firstGeometry) + mesh.geometryCount;
            if (mesh.geometryCount == 0 || geometryEnd > scene.geometries.size()) {
                std::cerr << "Scene mesh[" << meshIndex
                          << "] has an empty or out-of-range geometry range.\n";
                return VK_ERROR_INITIALIZATION_FAILED;
            }
            if (static_cast<uint64_t>(mesh.geometryCount)
                > accelerationStructureProperties.maxGeometryCount) {
                std::cerr << "Scene mesh[" << meshIndex << "] geometry count "
                          << mesh.geometryCount << " exceeds maxGeometryCount "
                          << accelerationStructureProperties.maxGeometryCount << ".\n";
                return VK_ERROR_INITIALIZATION_FAILED;
            }

            std::vector<VkAccelerationStructureGeometryKHR>& geometries =
                blasGeometries[meshIndex];
            std::vector<uint32_t>& primitiveCounts =
                blasPrimitiveCounts[meshIndex];
            std::vector<VkAccelerationStructureBuildRangeInfoKHR>& ranges =
                blasRanges[meshIndex];
            geometries.resize(mesh.geometryCount);
            primitiveCounts.resize(mesh.geometryCount);
            ranges.resize(mesh.geometryCount);

            uint64_t meshPrimitiveCount = 0;
            for (uint32_t localGeometry = 0;
                 localGeometry < mesh.geometryCount;
                 ++localGeometry) {
                const SceneGeometry& geometry =
                    scene.geometries[mesh.firstGeometry + localGeometry];
                if (geometry.vertexCount == 0
                    || geometry.indexCount == 0
                    || (geometry.indexCount % 3) != 0) {
                    std::cerr << "Scene geometry["
                              << (mesh.firstGeometry + localGeometry)
                              << "] must contain nonempty indexed triangles.\n";
                    return VK_ERROR_INITIALIZATION_FAILED;
                }

                const uint32_t primitiveCount = geometry.indexCount / 3;
                if (static_cast<uint64_t>(primitiveCount)
                    > std::numeric_limits<uint64_t>::max() - meshPrimitiveCount) {
                    std::cerr << "Scene mesh[" << meshIndex
                              << "] primitive count overflows uint64.\n";
                    return VK_ERROR_INITIALIZATION_FAILED;
                }
                meshPrimitiveCount += primitiveCount;
                if (meshPrimitiveCount
                    > accelerationStructureProperties.maxPrimitiveCount) {
                    std::cerr << "Scene mesh[" << meshIndex
                              << "] primitive count " << meshPrimitiveCount
                              << " exceeds maxPrimitiveCount "
                              << accelerationStructureProperties.maxPrimitiveCount
                              << ".\n";
                    return VK_ERROR_INITIALIZATION_FAILED;
                }

                const VkDeviceSize vertexOffset =
                    static_cast<VkDeviceSize>(geometry.firstVertex)
                    * 3 * sizeof(float);
                const VkDeviceSize indexOffset =
                    static_cast<VkDeviceSize>(geometry.firstIndex)
                    * sizeof(uint32_t);
                VkDeviceSize vertexAddress = 0;
                VkDeviceSize indexAddress = 0;
                if (!checkedAdd(
                        gpuScene.positionBufferAddress,
                        vertexOffset,
                        &vertexAddress)
                    || !checkedAdd(
                        gpuScene.indexBufferAddress,
                        indexOffset,
                        &indexAddress)) {
                    std::cerr << "Scene geometry["
                              << (mesh.firstGeometry + localGeometry)
                              << "] build-input device address overflows.\n";
                    return VK_ERROR_INITIALIZATION_FAILED;
                }
                if (!hasRequiredBuildInputAlignment(
                        vertexAddress,
                        VertexInputAddressAlignment,
                        "vertex")
                    || !hasRequiredBuildInputAlignment(
                        indexAddress,
                        IndexInputAddressAlignment,
                        "index")) {
                    return VK_ERROR_INITIALIZATION_FAILED;
                }

                VkAccelerationStructureGeometryKHR& blasGeometry =
                    geometries[localGeometry];
                blasGeometry.sType =
                    VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_KHR;
                blasGeometry.geometryType = VK_GEOMETRY_TYPE_TRIANGLES_KHR;
                // The runtime decoder still rejects alpha-tested geometry until the
                // any-hit/SBT-class consumer lands, so every accepted range is opaque.
                blasGeometry.flags = VK_GEOMETRY_OPAQUE_BIT_KHR;
                blasGeometry.geometry.triangles.sType =
                    VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_TRIANGLES_DATA_KHR;
                blasGeometry.geometry.triangles.vertexFormat = BlasVertexFormat;
                blasGeometry.geometry.triangles.vertexData.deviceAddress = vertexAddress;
                blasGeometry.geometry.triangles.vertexStride = 3 * sizeof(float);
                blasGeometry.geometry.triangles.maxVertex = geometry.vertexCount - 1;
                blasGeometry.geometry.triangles.indexType = VK_INDEX_TYPE_UINT32;
                blasGeometry.geometry.triangles.indexData.deviceAddress = indexAddress;

                primitiveCounts[localGeometry] = primitiveCount;
                ranges[localGeometry].primitiveCount = primitiveCount;
                ranges[localGeometry].primitiveOffset = 0;
                ranges[localGeometry].firstVertex = 0;
                ranges[localGeometry].transformOffset = 0;
            }

            VkAccelerationStructureBuildGeometryInfoKHR& buildInfo =
                blasBuildInfos[meshIndex];
            buildInfo.sType =
                VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_GEOMETRY_INFO_KHR;
            buildInfo.type = VK_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL_KHR;
            buildInfo.flags =
                VK_BUILD_ACCELERATION_STRUCTURE_PREFER_FAST_TRACE_BIT_KHR;
            buildInfo.mode = VK_BUILD_ACCELERATION_STRUCTURE_MODE_BUILD_KHR;
            buildInfo.geometryCount = mesh.geometryCount;
            buildInfo.pGeometries = geometries.data();

            VkAccelerationStructureBuildSizesInfoKHR& sizes = blasSizes[meshIndex];
            sizes.sType =
                VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_SIZES_INFO_KHR;
            functions.getAccelerationStructureBuildSizes(
                device,
                VK_ACCELERATION_STRUCTURE_BUILD_TYPE_DEVICE_KHR,
                &buildInfo,
                primitiveCounts.data(),
                &sizes);

            blasRangePointers[meshIndex] = ranges.data();
            blasScratchOffsets[meshIndex] = alignedBlasScratchTotal;
            VkDeviceSize alignedScratchSize = 0;
            if (!checkedAlignUp(
                    sizes.buildScratchSize,
                    scratchAlignment,
                    &alignedScratchSize)
                || !checkedAdd(
                    alignedBlasScratchTotal,
                    alignedScratchSize,
                    &alignedBlasScratchTotal)) {
                std::cerr << "Combined BLAS scratch-arena size overflows VkDeviceSize.\n";
                return VK_ERROR_INITIALIZATION_FAILED;
            }
        }

        for (std::size_t instanceIndex = 0;
             instanceIndex < scene.instances.size();
             ++instanceIndex) {
            if (scene.instances[instanceIndex].meshIndex >= scene.meshes.size()) {
                std::cerr << "Scene instance[" << instanceIndex
                          << "] references an out-of-range mesh.\n";
                return VK_ERROR_INITIALIZATION_FAILED;
            }
        }

        static_assert(std::is_nothrow_default_constructible_v<BlasEntry>);
        static_assert(std::is_nothrow_move_constructible_v<BlasEntry>);
        VkResult result = VK_SUCCESS;
        for (std::size_t meshIndex = 0; meshIndex < meshCount; ++meshIndex) {
            // Capacity was reserved before resource creation, so adopting this blank
            // entry cannot allocate. Every subsequent partial resource is immediately
            // visible to the owner destructor.
            as->blases.emplace_back();
            BlasEntry& entry = as->blases.back();
            result = createAccelerationStructureBacking(
                allocator,
                device,
                functions,
                VK_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL_KHR,
                blasSizes[meshIndex],
                VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_STORAGE_BIT_KHR
                    | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT,
                &entry.buffer,
                &entry.allocation,
                &entry.blas);
            if (result != VK_SUCCESS) {
                return result;
            }

            blasBuildInfos[meshIndex].dstAccelerationStructure = entry.blas;

            // The address is fixed when the handle is created, so instances can point
            // at it before the batched build commands execute.
            VkAccelerationStructureDeviceAddressInfoKHR addressInfo{};
            addressInfo.sType =
                VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_DEVICE_ADDRESS_INFO_KHR;
            addressInfo.accelerationStructure = entry.blas;
            entry.address = functions.getAccelerationStructureDeviceAddress(
                device,
                &addressInfo);
            if (entry.address == 0) {
                std::cerr << "Vulkan returned a null BLAS device address for mesh["
                          << meshIndex << "].\n";
                return VK_ERROR_INITIALIZATION_FAILED;
            }
        }

        for (std::size_t instanceIndex = 0;
             instanceIndex < scene.instances.size();
             ++instanceIndex) {
            const SceneInstance& source = scene.instances[instanceIndex];
            const SceneMesh& mesh = scene.meshes[source.meshIndex];
            VkAccelerationStructureInstanceKHR& destination =
                vkInstances[instanceIndex];
            destination.transform = toVkTransformMatrix(source.transform);
            // The BLAS geometry list follows the mesh's contiguous flat range, so
            // InstanceID() + GeometryIndex() reconstructs the global geometry record.
            destination.instanceCustomIndex = mesh.firstGeometry;
            destination.mask = 0xFF;
            // Load-bearing interim SBT contract: every geometry routes to the one hit
            // record until the opaque/alpha split lands atomically with SBT routing.
            destination.instanceShaderBindingTableRecordOffset = 0;
            destination.flags = 0;
            destination.accelerationStructureReference =
                as->blases[source.meshIndex].address;
        }

        result = createHostVisibleBuffer(
            allocator,
            vkInstances.data(),
            instanceBufferSize,
            BuildInputBufferUsage,
            &as->instanceBuffer,
            &as->instanceBufferAllocation);
        if (result != VK_SUCCESS) {
            return result;
        }

        const VkDeviceAddress instanceAddress =
            getBufferAddress(device, functions, as->instanceBuffer);
        if (!hasRequiredBuildInputAlignment(
                instanceAddress,
                InstanceInputAddressAlignment,
                "instance")) {
            return VK_ERROR_INITIALIZATION_FAILED;
        }

        VkAccelerationStructureGeometryKHR tlasGeometry{};
        tlasGeometry.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_KHR;
        tlasGeometry.geometryType = VK_GEOMETRY_TYPE_INSTANCES_KHR;
        tlasGeometry.geometry.instances.sType =
            VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_INSTANCES_DATA_KHR;
        tlasGeometry.geometry.instances.arrayOfPointers = VK_FALSE;
        tlasGeometry.geometry.instances.data.deviceAddress = instanceAddress;

        VkAccelerationStructureBuildGeometryInfoKHR tlasBuildInfo{};
        tlasBuildInfo.sType =
            VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_GEOMETRY_INFO_KHR;
        tlasBuildInfo.type = VK_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL_KHR;
        tlasBuildInfo.flags =
            VK_BUILD_ACCELERATION_STRUCTURE_PREFER_FAST_TRACE_BIT_KHR;
        tlasBuildInfo.mode = VK_BUILD_ACCELERATION_STRUCTURE_MODE_BUILD_KHR;
        tlasBuildInfo.geometryCount = 1;
        tlasBuildInfo.pGeometries = &tlasGeometry;

        const uint32_t instanceCount = static_cast<uint32_t>(scene.instances.size());
        VkAccelerationStructureBuildSizesInfoKHR tlasSizes{};
        tlasSizes.sType =
            VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_SIZES_INFO_KHR;
        functions.getAccelerationStructureBuildSizes(
            device,
            VK_ACCELERATION_STRUCTURE_BUILD_TYPE_DEVICE_KHR,
            &tlasBuildInfo,
            &instanceCount,
            &tlasSizes);

        // The TLAS is descriptor-bound and its own address is never queried, so its
        // backing buffer deliberately needs no shader-device-address usage.
        result = createAccelerationStructureBacking(
            allocator,
            device,
            functions,
            VK_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL_KHR,
            tlasSizes,
            VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_STORAGE_BIT_KHR,
            &as->tlasBuffer,
            &as->tlasBufferAllocation,
            &as->tlas);
        if (result != VK_SUCCESS) {
            return result;
        }
        tlasBuildInfo.dstAccelerationStructure = as->tlas;

        VkDeviceSize alignedTlasScratchSize = 0;
        if (!checkedAlignUp(
                tlasSizes.buildScratchSize,
                scratchAlignment,
                &alignedTlasScratchSize)) {
            std::cerr << "TLAS scratch size alignment overflows VkDeviceSize.\n";
            return VK_ERROR_INITIALIZATION_FAILED;
        }
        const VkDeviceSize scratchDataSize =
            std::max(alignedBlasScratchTotal, alignedTlasScratchSize);
        VkDeviceSize scratchAllocationSize = 0;
        if (!checkedAdd(
                scratchDataSize,
                scratchAlignment - 1,
                &scratchAllocationSize)
            || scratchAllocationSize == 0) {
            std::cerr << "Acceleration-structure scratch-arena allocation size overflows.\n";
            return VK_ERROR_INITIALIZATION_FAILED;
        }

        result = createBuffer(
            allocator,
            scratchAllocationSize,
            VK_BUFFER_USAGE_STORAGE_BUFFER_BIT
                | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT,
            0,
            &as->scratchBuffer,
            &as->scratchBufferAllocation);
        if (result != VK_SUCCESS) {
            return result;
        }

        const VkDeviceAddress scratchBaseAddress =
            getBufferAddress(device, functions, as->scratchBuffer);
        VkDeviceSize alignedScratchBase = 0;
        if (!checkedAlignUp(
                scratchBaseAddress,
                scratchAlignment,
                &alignedScratchBase)) {
            std::cerr << "Acceleration-structure scratch device address overflows while aligning.\n";
            return VK_ERROR_INITIALIZATION_FAILED;
        }
        for (std::size_t meshIndex = 0; meshIndex < meshCount; ++meshIndex) {
            VkDeviceSize scratchAddress = 0;
            if (!checkedAdd(
                    alignedScratchBase,
                    blasScratchOffsets[meshIndex],
                    &scratchAddress)) {
                std::cerr << "BLAS scratch device address overflows for mesh["
                          << meshIndex << "].\n";
                return VK_ERROR_INITIALIZATION_FAILED;
            }
            blasBuildInfos[meshIndex].scratchData.deviceAddress = scratchAddress;
        }
        tlasBuildInfo.scratchData.deviceAddress = alignedScratchBase;

        VkAccelerationStructureBuildRangeInfoKHR tlasRange{};
        tlasRange.primitiveCount = instanceCount;

        VkCommandBufferBeginInfo beginInfo{};
        beginInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
        beginInfo.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;

        // The final GpuScene upload left the borrowed command buffer executable.
        result = vkResetCommandBuffer(commandBuffer, 0);
        if (result != VK_SUCCESS) {
            return result;
        }
        result = vkBeginCommandBuffer(commandBuffer, &beginInfo);
        if (result != VK_SUCCESS) {
            return result;
        }

        functions.cmdBuildAccelerationStructures(
            commandBuffer,
            static_cast<uint32_t>(meshCount),
            blasBuildInfos.data(),
            blasRangePointers.data());

        // The read dependency exposes BLAS contents to the TLAS. The write dependency
        // additionally orders TLAS scratch writes after every batched BLAS scratch write
        // before the TLAS reuses the same arena base.
        VkMemoryBarrier buildBarrier{};
        buildBarrier.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER;
        buildBarrier.srcAccessMask =
            VK_ACCESS_ACCELERATION_STRUCTURE_WRITE_BIT_KHR;
        buildBarrier.dstAccessMask =
            VK_ACCESS_ACCELERATION_STRUCTURE_READ_BIT_KHR
            | VK_ACCESS_ACCELERATION_STRUCTURE_WRITE_BIT_KHR;
        vkCmdPipelineBarrier(
            commandBuffer,
            VK_PIPELINE_STAGE_ACCELERATION_STRUCTURE_BUILD_BIT_KHR,
            VK_PIPELINE_STAGE_ACCELERATION_STRUCTURE_BUILD_BIT_KHR,
            0,
            1,
            &buildBarrier,
            0,
            nullptr,
            0,
            nullptr);

        const VkAccelerationStructureBuildRangeInfoKHR* tlasRangePointer =
            &tlasRange;
        functions.cmdBuildAccelerationStructures(
            commandBuffer,
            1,
            &tlasBuildInfo,
            &tlasRangePointer);

        // Submission order alone carries no memory dependency. This trailing scope
        // makes the TLAS visible to every later trace submission on the same queue.
        VkMemoryBarrier traversalBarrier{};
        traversalBarrier.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER;
        traversalBarrier.srcAccessMask =
            VK_ACCESS_ACCELERATION_STRUCTURE_WRITE_BIT_KHR;
        traversalBarrier.dstAccessMask =
            VK_ACCESS_ACCELERATION_STRUCTURE_READ_BIT_KHR;
        vkCmdPipelineBarrier(
            commandBuffer,
            VK_PIPELINE_STAGE_ACCELERATION_STRUCTURE_BUILD_BIT_KHR,
            VK_PIPELINE_STAGE_RAY_TRACING_SHADER_BIT_KHR,
            0,
            1,
            &traversalBarrier,
            0,
            nullptr,
            0,
            nullptr);

        result = vkEndCommandBuffer(commandBuffer);
        if (result != VK_SUCCESS) {
            return result;
        }

        // Reset -> submit -> wait returns the borrowed frame-0 fence signaled for the
        // first drawFrame wait, matching every preceding startup upload.
        result = vkResetFences(device, 1, &fence);
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
            device,
            1,
            &fence,
            VK_TRUE,
            std::numeric_limits<uint64_t>::max());
        if (result != VK_SUCCESS) {
            return result;
        }

        // Build completion ends the arena's lifetime; null-resetting preserves the
        // same partial-failure teardown contract in the owner destructor.
        destroyBufferAndAllocation(
            allocator,
            as->scratchBuffer,
            as->scratchBufferAllocation,
            "acceleration-structure scratch arena (post-build)");
        as->scratchBuffer = VK_NULL_HANDLE;
        as->scratchBufferAllocation = nullptr;
        return VK_SUCCESS;
    } catch (const std::bad_alloc&) {
        std::cerr << "Acceleration-structure CPU metadata allocation failed.\n";
        return VK_ERROR_OUT_OF_HOST_MEMORY;
    } catch (const std::length_error&) {
        std::cerr << "Acceleration-structure CPU metadata size exceeds its container limit.\n";
        return VK_ERROR_OUT_OF_HOST_MEMORY;
    }
}
}
