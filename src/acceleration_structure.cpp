#include "acceleration_structure.hpp"

#include "vulkan_context.hpp"

#include <cstdint>
#include <cstring>
#include <iostream>
#include <limits>

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

// Usage shared by every acceleration-structure build input (vertex/index/instance
// buffers): readable by the build, addressable because the build consumes device
// addresses rather than descriptors.
constexpr VkBufferUsageFlags BuildInputBufferUsage =
    VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_BUILD_INPUT_READ_ONLY_BIT_KHR
    | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT;

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

// Create a host-visible buffer and copy `size` bytes of `data` into it. Coherent memory
// so the writes need no explicit flush: the queue submission that consumes the buffer
// makes them visible to the device.
VkResult createHostVisibleBuffer(
    VkPhysicalDevice physicalDevice,
    VkDevice device,
    const void* data,
    VkDeviceSize size,
    VkBufferUsageFlags usage,
    VkBuffer* buffer,
    VkDeviceMemory* memory)
{
    VkResult result = createBuffer(
        physicalDevice,
        device,
        size,
        usage,
        VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
        buffer,
        memory);

    if (result != VK_SUCCESS) {
        return result;
    }

    void* mapped = nullptr;
    result = vkMapMemory(device, *memory, 0, size, 0, &mapped);

    if (result != VK_SUCCESS) {
        return result;
    }

    std::memcpy(mapped, data, size);
    vkUnmapMemory(device, *memory);

    return VK_SUCCESS;
}

// Create the backing buffer and handle for one acceleration structure, plus its build
// scratch buffer. The scratch is allocated with `alignment - 1` bytes of slack and
// *scratchAddress is rounded up, because the spec requires the scratch *device address*
// to be a multiple of minAccelerationStructureScratchOffsetAlignment and a buffer's
// base address carries no such guarantee. The unaligned buffer/memory handles stay in
// the out-parameters for cleanup; only the aligned address is handed to the build.
VkResult createAccelerationStructureWithScratch(
    VkPhysicalDevice physicalDevice,
    VkDevice device,
    const RayTracingFunctions& functions,
    VkAccelerationStructureTypeKHR type,
    const VkAccelerationStructureBuildSizesInfoKHR& sizes,
    VkDeviceSize scratchAlignment,
    VkBufferUsageFlags backingBufferUsage,
    VkBuffer* backingBuffer,
    VkDeviceMemory* backingBufferMemory,
    VkAccelerationStructureKHR* accelerationStructure,
    VkBuffer* scratchBuffer,
    VkDeviceMemory* scratchBufferMemory,
    VkDeviceAddress* scratchAddress)
{
    VkResult result = createBuffer(
        physicalDevice,
        device,
        sizes.accelerationStructureSize,
        backingBufferUsage,
        VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT,
        backingBuffer,
        backingBufferMemory);

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
        physicalDevice,
        device,
        sizes.buildScratchSize + scratchAlignment - 1,
        VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT,
        VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT,
        scratchBuffer,
        scratchBufferMemory);

    if (result != VK_SUCCESS) {
        return result;
    }

    *scratchAddress = alignUp(
        getBufferAddress(device, functions, *scratchBuffer),
        scratchAlignment);

    return VK_SUCCESS;
}
// Destroy a buffer and free its backing memory, logging once if either existed. Handles
// the partially created case (buffer without memory, or vice versa) that the bare-return
// failure paths can leave behind.
void destroyBufferAndMemory(
    VkDevice device,
    VkBuffer buffer,
    VkDeviceMemory memory,
    const char* name)
{
    if (buffer != VK_NULL_HANDLE) {
        vkDestroyBuffer(device, buffer, nullptr);
    }

    if (memory != VK_NULL_HANDLE) {
        vkFreeMemory(device, memory, nullptr);
    }

    if (buffer != VK_NULL_HANDLE || memory != VK_NULL_HANDLE) {
        std::cout << "Destroyed Vulkan " << name << ".\n";
    }
}

} // namespace

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

    destroyBufferAndMemory(device, tlasScratchBuffer, tlasScratchBufferMemory, "TLAS scratch buffer");
    destroyBufferAndMemory(device, blasScratchBuffer, blasScratchBufferMemory, "BLAS scratch buffer");
    destroyBufferAndMemory(device, tlasBuffer, tlasBufferMemory, "TLAS backing buffer");
    destroyBufferAndMemory(device, blasBuffer, blasBufferMemory, "BLAS backing buffer");
    destroyBufferAndMemory(device, instanceBuffer, instanceBufferMemory, "instance buffer");
    destroyBufferAndMemory(device, indexBuffer, indexBufferMemory, "index buffer");
    destroyBufferAndMemory(device, vertexBuffer, vertexBufferMemory, "vertex buffer");
}

VkResult buildAccelerationStructures(
    AccelerationStructure* as,
    VkPhysicalDevice physicalDevice,
    VkDevice device,
    const RayTracingFunctions& functions,
    VkCommandBuffer commandBuffer,
    VkQueue traceQueue,
    VkFence fence)
{
    // Adopt the device and the destroy entry point first, so every resource created
    // below — including a partial set on a failure path — is torn down by the
    // destructor (see the header contract on destroyAccelerationStructure).
    as->device = device;
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

    VkResult result = createHostVisibleBuffer(
        physicalDevice,
        device,
        TriangleVertices,
        sizeof(TriangleVertices),
        BuildInputBufferUsage,
        &as->vertexBuffer,
        &as->vertexBufferMemory);

    if (result != VK_SUCCESS) {
        return result;
    }

    result = createHostVisibleBuffer(
        physicalDevice,
        device,
        TriangleIndices,
        sizeof(TriangleIndices),
        BuildInputBufferUsage,
        &as->indexBuffer,
        &as->indexBufferMemory);

    if (result != VK_SUCCESS) {
        return result;
    }

    // OPAQUE lets the eventual trace skip any-hit shading for this geometry; nothing in
    // the scene needs transparency.
    VkAccelerationStructureGeometryKHR blasGeometry{};
    blasGeometry.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_KHR;
    blasGeometry.geometryType = VK_GEOMETRY_TYPE_TRIANGLES_KHR;
    blasGeometry.flags = VK_GEOMETRY_OPAQUE_BIT_KHR;
    blasGeometry.geometry.triangles.sType =
        VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_TRIANGLES_DATA_KHR;
    blasGeometry.geometry.triangles.vertexFormat = VK_FORMAT_R32G32B32_SFLOAT;
    blasGeometry.geometry.triangles.vertexData.deviceAddress =
        getBufferAddress(device, functions, as->vertexBuffer);
    blasGeometry.geometry.triangles.vertexStride = 3 * sizeof(float);
    blasGeometry.geometry.triangles.maxVertex = 2;
    blasGeometry.geometry.triangles.indexType = VK_INDEX_TYPE_UINT32;
    blasGeometry.geometry.triangles.indexData.deviceAddress =
        getBufferAddress(device, functions, as->indexBuffer);

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
        physicalDevice,
        device,
        functions,
        VK_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL_KHR,
        blasSizes,
        scratchAlignment,
        VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_STORAGE_BIT_KHR
            | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT,
        &as->blasBuffer,
        &as->blasBufferMemory,
        &as->blas,
        &as->blasScratchBuffer,
        &as->blasScratchBufferMemory,
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

    // Identity transform (VkTransformMatrixKHR is row-major, three rows of a 3x4
    // matrix); mask 0xFF so every ray's cull mask hits the instance.
    VkAccelerationStructureInstanceKHR instance{};
    instance.transform.matrix[0][0] = 1.0f;
    instance.transform.matrix[1][1] = 1.0f;
    instance.transform.matrix[2][2] = 1.0f;
    instance.mask = 0xFF;
    instance.accelerationStructureReference = blasAddress;

    result = createHostVisibleBuffer(
        physicalDevice,
        device,
        &instance,
        sizeof(instance),
        BuildInputBufferUsage,
        &as->instanceBuffer,
        &as->instanceBufferMemory);

    if (result != VK_SUCCESS) {
        return result;
    }

    VkAccelerationStructureGeometryKHR tlasGeometry{};
    tlasGeometry.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_KHR;
    tlasGeometry.geometryType = VK_GEOMETRY_TYPE_INSTANCES_KHR;
    tlasGeometry.geometry.instances.sType =
        VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_INSTANCES_DATA_KHR;
    tlasGeometry.geometry.instances.arrayOfPointers = VK_FALSE;
    tlasGeometry.geometry.instances.data.deviceAddress =
        getBufferAddress(device, functions, as->instanceBuffer);

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
        physicalDevice,
        device,
        functions,
        VK_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL_KHR,
        tlasSizes,
        scratchAlignment,
        VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_STORAGE_BIT_KHR,
        &as->tlasBuffer,
        &as->tlasBufferMemory,
        &as->tlas,
        &as->tlasScratchBuffer,
        &as->tlasScratchBufferMemory,
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
    destroyBufferAndMemory(
        device,
        as->blasScratchBuffer,
        as->blasScratchBufferMemory,
        "BLAS scratch buffer (post-build)");
    as->blasScratchBuffer = VK_NULL_HANDLE;
    as->blasScratchBufferMemory = VK_NULL_HANDLE;

    destroyBufferAndMemory(
        device,
        as->tlasScratchBuffer,
        as->tlasScratchBufferMemory,
        "TLAS scratch buffer (post-build)");
    as->tlasScratchBuffer = VK_NULL_HANDLE;
    as->tlasScratchBufferMemory = VK_NULL_HANDLE;

    return VK_SUCCESS;
}
}
