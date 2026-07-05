#pragma once

#include <cstdint>

#include <vulkan/vulkan.h>

namespace xrphoton
{
struct FrameResources;
struct RayTracingFunctions;
struct RtPipeline;
struct Swapchain;

// The renderer's view of everything the frame path uses. Owns nothing: the handles
// are copies of program-lifetime objects (owned by VulkanContext /
// AccelerationStructure, never recreated), and the structs are borrowed by pointer —
// Swapchain necessarily so, since its members are replaced on every recreate. A plain
// parameter bundle in the spirit of QueueFamilyIndices, not an RAII owner: no
// destructor, no idle wait, and no declaration-order constraint beyond not outliving
// what it borrows — main() creates it after everything it points at.
struct Renderer
{
    VkPhysicalDevice physicalDevice = VK_NULL_HANDLE;
    VkDevice device = VK_NULL_HANDLE;
    VkQueue traceQueue = VK_NULL_HANDLE;
    VkQueue presentQueue = VK_NULL_HANDLE;
    const FrameResources* frames = nullptr;
    VkAccelerationStructureKHR tlas = VK_NULL_HANDLE;
    const RayTracingFunctions* functions = nullptr;
    const RtPipeline* rtPipeline = nullptr;
    const Swapchain* swap = nullptr;
};

// The RT pipeline's two obligations whenever the swapchain (re)appears, kept as one
// code path: rewrite the descriptor set to the current storage image view (the view
// is recreated with the swapchain; the recreate's device-idle makes the rewrite
// race-free), and gate the trace dispatch dimensions against the device limits. The
// spec minimum for maxRayDispatchInvocationCount is 2^30, so any realistic swapchain
// passes — checked anyway to fail loudly rather than hit undefined behavior on an
// exotic driver.
bool prepareRtForSwapchain(const Renderer& renderer);

// Render and present one frame using frameIndex's command buffer and sync objects.
// Steps: wait the in-flight fence -> acquire an image -> record and submit the trace
// -> present. OUT_OF_DATE and SUBOPTIMAL are returned (not treated as errors) so the
// caller can trigger a swapchain recreate; a successful frame returns the acquire
// result so a SUBOPTIMAL acquire still propagates. Any other non-success VkResult is
// a hard error.
VkResult drawFrame(const Renderer& renderer, uint32_t frameIndex);
}
