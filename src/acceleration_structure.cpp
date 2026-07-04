#include "acceleration_structure.hpp"

#include <iostream>

#include <vulkan/vulkan.h>

namespace xrphoton
{
namespace
{
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
}
