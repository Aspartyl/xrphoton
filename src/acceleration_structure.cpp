#include "acceleration_structure.hpp"

#include "vulkan_context.hpp"
#include "vk_mem_alloc.h"

#include <cstdint>
#include <cstring>
#include <iomanip>
#include <iostream>
#include <limits>

#include <glm/ext/matrix_transform.hpp>
#include <glm/mat4x4.hpp>
#include <glm/trigonometric.hpp>
#include <vulkan/vulkan.h>

namespace xrphoton
{
namespace
{
// The hardcoded scene: a single triangle in the XY plane, counter-clockwise. Positions
// only — shading attributes come later, alongside real geometry loading.
constexpr float TriangleVertices[] = {
    0.0f, 0.5f, 0.0f,
    -0.5f, -0.5f, 0.0f,
    0.5f, -0.5f, 0.0f,
};
constexpr uint32_t TriangleIndices[] = {0, 1, 2};
constexpr uint32_t TrianglePrimitiveCount = 1;
constexpr uint32_t InstanceCount = 1;

// The vertex format the BLAS build declares. Defined once so the suitability gate
// and the build input can never diverge (the StorageImageFormat pattern from
// swapchain.cpp).
constexpr VkFormat BlasVertexFormat = VK_FORMAT_R32G32B32_SFLOAT;

// Usage shared by every acceleration-structure build input (vertex/index/instance
// buffers): readable by the build, addressable because the build consumes device
// addresses rather than descriptors.
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

// Round a device address up to the next multiple of alignment. Vulkan alignment limits
// are powers of two, so the mask form is exact.
VkDeviceAddress alignUp(VkDeviceAddress address, VkDeviceSize alignment)
{
    return (address + alignment - 1) & ~(static_cast<VkDeviceAddress>(alignment) - 1);
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

// Create the backing buffer and handle for one acceleration structure, plus its build
// scratch buffer. The scratch is allocated with `alignment - 1` bytes of slack and
// *scratchAddress is rounded up, because the spec requires the scratch *device address*
// to be a multiple of minAccelerationStructureScratchOffsetAlignment and a buffer's
// base address carries no such guarantee. The unaligned buffer/memory handles stay in
// the out-parameters for cleanup; only the aligned address is handed to the build.
VkResult createAccelerationStructureWithScratch(
    VmaAllocator allocator,
    VkDevice device,
    const RayTracingFunctions& functions,
    VkAccelerationStructureTypeKHR type,
    const VkAccelerationStructureBuildSizesInfoKHR& sizes,
    VkDeviceSize scratchAlignment,
    VkBufferUsageFlags backingBufferUsage,
    VkBuffer* backingBuffer,
    VmaAllocation* backingBufferAllocation,
    VkAccelerationStructureKHR* accelerationStructure,
    VkBuffer* scratchBuffer,
    VmaAllocation* scratchBufferAllocation,
    VkDeviceAddress* scratchAddress)
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

    result = functions.createAccelerationStructure(
        device,
        &createInfo,
        nullptr,
        accelerationStructure);

    if (result != VK_SUCCESS) {
        return result;
    }

    result = createBuffer(
        allocator,
        sizes.buildScratchSize + scratchAlignment - 1,
        VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT,
        0,
        scratchBuffer,
        scratchBufferAllocation);

    if (result != VK_SUCCESS) {
        return result;
    }

    *scratchAddress = alignUp(
        getBufferAddress(device, functions, *scratchBuffer),
        scratchAlignment);

    return VK_SUCCESS;
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

    // The acceleration structure handles are placed on their backing buffers, so they
    // are destroyed first, then the buffers under them. The destroy entry point is set
    // by whoever created the handles (see the header contract), so it is non-null
    // whenever tlas/blas are.
    if (tlas != VK_NULL_HANDLE && destroyAccelerationStructure != nullptr) {
        destroyAccelerationStructure(device, tlas, nullptr);
        std::cout << "Destroyed Vulkan top-level acceleration structure.\n";
    }

    if (blas != VK_NULL_HANDLE && destroyAccelerationStructure != nullptr) {
        destroyAccelerationStructure(device, blas, nullptr);
        std::cout << "Destroyed Vulkan bottom-level acceleration structure.\n";
    }

    destroyBufferAndAllocation(allocator, tlasScratchBuffer, tlasScratchBufferAllocation, "TLAS scratch buffer");
    destroyBufferAndAllocation(allocator, blasScratchBuffer, blasScratchBufferAllocation, "BLAS scratch buffer");
    destroyBufferAndAllocation(allocator, tlasBuffer, tlasBufferAllocation, "TLAS backing buffer");
    destroyBufferAndAllocation(allocator, blasBuffer, blasBufferAllocation, "BLAS backing buffer");
    destroyBufferAndAllocation(allocator, instanceBuffer, instanceBufferAllocation, "instance buffer");
    destroyBufferAndAllocation(allocator, indexBuffer, indexBufferAllocation, "index buffer");
    destroyBufferAndAllocation(allocator, vertexBuffer, vertexBufferAllocation, "vertex buffer");
}

VkResult buildAccelerationStructures(
    AccelerationStructure* as,
    VkPhysicalDevice physicalDevice,
    VkDevice device,
    VmaAllocator allocator,
    const RayTracingFunctions& functions,
    VkCommandBuffer commandBuffer,
    VkQueue traceQueue,
    VkFence fence)
{
    // Adopt the device and the destroy entry point first, so every resource created
    // below — including a partial set on a failure path — is torn down by the
    // destructor (see the header contract on destroyAccelerationStructure).
    as->device = device;
    as->allocator = allocator;
    as->destroyAccelerationStructure = functions.destroyAccelerationStructure;

    VkPhysicalDeviceAccelerationStructurePropertiesKHR accelerationStructureProperties{};
    accelerationStructureProperties.sType =
        VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ACCELERATION_STRUCTURE_PROPERTIES_KHR;

    VkPhysicalDeviceProperties2 physicalDeviceProperties{};
    physicalDeviceProperties.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2;
    physicalDeviceProperties.pNext = &accelerationStructureProperties;

    vkGetPhysicalDeviceProperties2(physicalDevice, &physicalDeviceProperties);

    const VkDeviceSize scratchAlignment =
        accelerationStructureProperties.minAccelerationStructureScratchOffsetAlignment;

    VkResult result = uploadDeviceLocalBuffer(
        allocator,
        device,
        commandBuffer,
        traceQueue,
        fence,
        TriangleVertices,
        sizeof(TriangleVertices),
        BuildInputBufferUsage | VK_BUFFER_USAGE_TRANSFER_DST_BIT,
        &as->vertexBuffer,
        &as->vertexBufferAllocation);

    if (result != VK_SUCCESS) {
        return result;
    }

    result = uploadDeviceLocalBuffer(
        allocator,
        device,
        commandBuffer,
        traceQueue,
        fence,
        TriangleIndices,
        sizeof(TriangleIndices),
        BuildInputBufferUsage | VK_BUFFER_USAGE_TRANSFER_DST_BIT,
        &as->indexBuffer,
        &as->indexBufferAllocation);

    if (result != VK_SUCCESS) {
        return result;
    }

    const VkDeviceAddress vertexAddress =
        getBufferAddress(device, functions, as->vertexBuffer);
    const VkDeviceAddress indexAddress =
        getBufferAddress(device, functions, as->indexBuffer);

    if (!hasRequiredBuildInputAlignment(
            vertexAddress,
            VertexInputAddressAlignment,
            "vertex")) {
        return VK_ERROR_INITIALIZATION_FAILED;
    }

    if (!hasRequiredBuildInputAlignment(
            indexAddress,
            IndexInputAddressAlignment,
            "index")) {
        return VK_ERROR_INITIALIZATION_FAILED;
    }

    // OPAQUE lets the eventual trace skip any-hit shading for this geometry; nothing in
    // the scene needs transparency.
    VkAccelerationStructureGeometryKHR blasGeometry{};
    blasGeometry.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_KHR;
    blasGeometry.geometryType = VK_GEOMETRY_TYPE_TRIANGLES_KHR;
    blasGeometry.flags = VK_GEOMETRY_OPAQUE_BIT_KHR;
    blasGeometry.geometry.triangles.sType =
        VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_TRIANGLES_DATA_KHR;
    blasGeometry.geometry.triangles.vertexFormat = BlasVertexFormat;
    blasGeometry.geometry.triangles.vertexData.deviceAddress = vertexAddress;
    blasGeometry.geometry.triangles.vertexStride = 3 * sizeof(float);
    blasGeometry.geometry.triangles.maxVertex = 2;
    blasGeometry.geometry.triangles.indexType = VK_INDEX_TYPE_UINT32;
    blasGeometry.geometry.triangles.indexData.deviceAddress = indexAddress;

    VkAccelerationStructureBuildGeometryInfoKHR blasBuildInfo{};
    blasBuildInfo.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_GEOMETRY_INFO_KHR;
    blasBuildInfo.type = VK_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL_KHR;
    blasBuildInfo.flags = VK_BUILD_ACCELERATION_STRUCTURE_PREFER_FAST_TRACE_BIT_KHR;
    blasBuildInfo.mode = VK_BUILD_ACCELERATION_STRUCTURE_MODE_BUILD_KHR;
    blasBuildInfo.geometryCount = 1;
    blasBuildInfo.pGeometries = &blasGeometry;

    VkAccelerationStructureBuildSizesInfoKHR blasSizes{};
    blasSizes.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_SIZES_INFO_KHR;

    functions.getAccelerationStructureBuildSizes(
        device,
        VK_ACCELERATION_STRUCTURE_BUILD_TYPE_DEVICE_KHR,
        &blasBuildInfo,
        &TrianglePrimitiveCount,
        &blasSizes);

    // The BLAS backing buffer needs SHADER_DEVICE_ADDRESS because the TLAS instance
    // below references the BLAS by its acceleration-structure device address, and
    // querying that address requires the flag on the buffer underneath (VUID 09542).
    VkDeviceAddress blasScratchAddress = 0;
    result = createAccelerationStructureWithScratch(
        allocator,
        device,
        functions,
        VK_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL_KHR,
        blasSizes,
        scratchAlignment,
        VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_STORAGE_BIT_KHR
            | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT,
        &as->blasBuffer,
        &as->blasBufferAllocation,
        &as->blas,
        &as->blasScratchBuffer,
        &as->blasScratchBufferAllocation,
        &blasScratchAddress);

    if (result != VK_SUCCESS) {
        return result;
    }

    blasBuildInfo.dstAccelerationStructure = as->blas;
    blasBuildInfo.scratchData.deviceAddress = blasScratchAddress;

    VkAccelerationStructureBuildRangeInfoKHR blasRange{};
    blasRange.primitiveCount = TrianglePrimitiveCount;

    // The acceleration-structure device address is fixed at creation, so the instance
    // can reference the BLAS before the build commands have executed.
    VkAccelerationStructureDeviceAddressInfoKHR blasAddressInfo{};
    blasAddressInfo.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_DEVICE_ADDRESS_INFO_KHR;
    blasAddressInfo.accelerationStructure = as->blas;

    const VkDeviceAddress blasAddress =
        functions.getAccelerationStructureDeviceAddress(device, &blasAddressInfo);

    // Rotate in object space, then translate in world space. The deliberately
    // non-identity transform proves the GLM-to-Vulkan layout conversion before
    // real scene data introduces multiple instances.
    glm::mat4 instanceTransform = glm::translate(
        glm::mat4(1.0f),
        glm::vec3(0.5f, 0.0f, 0.0f));
    instanceTransform = glm::rotate(
        instanceTransform,
        glm::radians(45.0f),
        glm::vec3(0.0f, 0.0f, 1.0f));

    VkAccelerationStructureInstanceKHR instance{};
    instance.transform = toVkTransformMatrix(instanceTransform);
    // Let every ray cull mask used by the renderer hit this instance.
    instance.mask = 0xFF;
    instance.accelerationStructureReference = blasAddress;

    result = createHostVisibleBuffer(
        allocator,
        &instance,
        sizeof(instance),
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
    tlasBuildInfo.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_GEOMETRY_INFO_KHR;
    tlasBuildInfo.type = VK_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL_KHR;
    tlasBuildInfo.flags = VK_BUILD_ACCELERATION_STRUCTURE_PREFER_FAST_TRACE_BIT_KHR;
    tlasBuildInfo.mode = VK_BUILD_ACCELERATION_STRUCTURE_MODE_BUILD_KHR;
    tlasBuildInfo.geometryCount = 1;
    tlasBuildInfo.pGeometries = &tlasGeometry;

    VkAccelerationStructureBuildSizesInfoKHR tlasSizes{};
    tlasSizes.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_SIZES_INFO_KHR;

    functions.getAccelerationStructureBuildSizes(
        device,
        VK_ACCELERATION_STRUCTURE_BUILD_TYPE_DEVICE_KHR,
        &tlasBuildInfo,
        &InstanceCount,
        &tlasSizes);

    // Unlike the BLAS, the TLAS backing buffer needs no device address: the future
    // descriptor set binds the TLAS handle, not an address.
    VkDeviceAddress tlasScratchAddress = 0;
    result = createAccelerationStructureWithScratch(
        allocator,
        device,
        functions,
        VK_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL_KHR,
        tlasSizes,
        scratchAlignment,
        VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_STORAGE_BIT_KHR,
        &as->tlasBuffer,
        &as->tlasBufferAllocation,
        &as->tlas,
        &as->tlasScratchBuffer,
        &as->tlasScratchBufferAllocation,
        &tlasScratchAddress);

    if (result != VK_SUCCESS) {
        return result;
    }

    tlasBuildInfo.dstAccelerationStructure = as->tlas;
    tlasBuildInfo.scratchData.deviceAddress = tlasScratchAddress;

    VkAccelerationStructureBuildRangeInfoKHR tlasRange{};
    tlasRange.primitiveCount = InstanceCount;

    VkCommandBufferBeginInfo beginInfo{};
    beginInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    beginInfo.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;

    // The second staged upload left the borrowed command buffer executable. Reset it
    // before reusing it for the acceleration-structure build submission.
    result = vkResetCommandBuffer(commandBuffer, 0);
    if (result != VK_SUCCESS) {
        return result;
    }

    result = vkBeginCommandBuffer(commandBuffer, &beginInfo);

    if (result != VK_SUCCESS) {
        return result;
    }

    const VkAccelerationStructureBuildRangeInfoKHR* blasRangePtr = &blasRange;
    functions.cmdBuildAccelerationStructures(commandBuffer, 1, &blasBuildInfo, &blasRangePtr);

    // The TLAS build reads the BLAS the previous command wrote, in the same stage.
    VkMemoryBarrier buildBarrier{};
    buildBarrier.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER;
    buildBarrier.srcAccessMask = VK_ACCESS_ACCELERATION_STRUCTURE_WRITE_BIT_KHR;
    buildBarrier.dstAccessMask = VK_ACCESS_ACCELERATION_STRUCTURE_READ_BIT_KHR;

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

    const VkAccelerationStructureBuildRangeInfoKHR* tlasRangePtr = &tlasRange;
    functions.cmdBuildAccelerationStructures(commandBuffer, 1, &tlasBuildInfo, &tlasRangePtr);

    // Make the TLAS contents visible to future traversal. The fence below only gives
    // the *host* visibility of the build, and submission order alone carries no memory
    // dependency — but a pipeline barrier's second scope covers all commands later in
    // submission order on this queue, so this one barrier covers every subsequent
    // vkCmdTraceRaysKHR without the frame path needing its own.
    VkMemoryBarrier traversalBarrier{};
    traversalBarrier.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER;
    traversalBarrier.srcAccessMask = VK_ACCESS_ACCELERATION_STRUCTURE_WRITE_BIT_KHR;
    traversalBarrier.dstAccessMask = VK_ACCESS_ACCELERATION_STRUCTURE_READ_BIT_KHR;

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

    // Reset -> submit -> wait leaves the fence signaled, so a caller passing the
    // pre-loop in-flight fence (created signaled) gets it back in the state the first
    // drawFrame's wait depends on. Nothing has been submitted against the fence before
    // this, so no wait is needed ahead of the reset.
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

    // The build is complete, so the scratch memory is dead weight; release it now
    // instead of holding it for the program's lifetime. The destructor's null guards
    // make the early release safe.
    destroyBufferAndAllocation(
        allocator,
        as->blasScratchBuffer,
        as->blasScratchBufferAllocation,
        "BLAS scratch buffer (post-build)");
    as->blasScratchBuffer = VK_NULL_HANDLE;
    as->blasScratchBufferAllocation = nullptr;

    destroyBufferAndAllocation(
        allocator,
        as->tlasScratchBuffer,
        as->tlasScratchBufferAllocation,
        "TLAS scratch buffer (post-build)");
    as->tlasScratchBuffer = VK_NULL_HANDLE;
    as->tlasScratchBufferAllocation = nullptr;

    return VK_SUCCESS;
}
}
