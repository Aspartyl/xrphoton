#pragma once

#include <cstdint>
#include <vector>

#include <vulkan/vulkan.h>

#include "vma_fwd.hpp"

namespace xrphoton
{
struct AccelerationStructure;
struct FrameResources;
struct FrameLighting;
struct GpuLighting;
struct RaygenPushConstants;
struct RayTracingFunctions;
struct RtPipeline;
struct SceneData;
struct Swapchain;
struct TonemapPipeline;

// The renderer's view of everything the frame path uses. Owns nothing: Vulkan handles
// are borrowed copies from VulkanContext, while scene, acceleration-structure, pipeline,
// and swapchain owners are borrowed by pointer (the swapchain's members are replaced on
// every recreate). A plain parameter bundle in the spirit of QueueFamilyIndices, not
// an RAII owner: no destructor, no idle wait, and no declaration-order constraint
// beyond not outliving what it borrows — main() creates it after every pointee.
struct Renderer
{
    VkPhysicalDevice physicalDevice = VK_NULL_HANDLE;
    VkDevice device = VK_NULL_HANDLE;
    VmaAllocator allocator = nullptr;
    VkQueue traceQueue = VK_NULL_HANDLE;
    VkQueue presentQueue = VK_NULL_HANDLE;
    float traceTimestampPeriod = 0.0f;
    std::uint32_t traceTimestampValidBits = 0;
    const FrameResources* frames = nullptr;
    GpuLighting* gpuLighting = nullptr;
    AccelerationStructure* accel = nullptr;
    const SceneData* scene = nullptr;
    const RayTracingFunctions* functions = nullptr;
    const RtPipeline* rtPipeline = nullptr;
    const TonemapPipeline* tonemapPipeline = nullptr;
    const Swapchain* swap = nullptr;
};

// CPU-owned copy of the shared resize-bound LDR output image. Pixels are tightly packed
// row-major R8G8B8A8_UNORM bytes in linear space; no Vulkan/VMA handle escapes with it.
struct StorageImageReadback
{
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    std::vector<std::uint8_t> rgba8;
};

// Rewrite the RT and tonemap descriptors to the current HDR/LDR views and gate both
// trace and compute dispatch dimensions against device limits. Resize idles the device,
// so rewriting these program-lifetime descriptor sets is race-free.
bool prepareRtForSwapchain(const Renderer& renderer);

// Render and present one frame using frameIndex's command buffer and sync objects,
// pushing the raygen payload into that command buffer before tracing. Steps: wait the
// in-flight fence -> rewrite that slot's TLAS instances -> acquire an image -> record
// the TLAS rebuild and trace -> submit -> present.
// OUT_OF_DATE and SUBOPTIMAL are returned (not treated as errors) so the caller can
// trigger a swapchain recreate; a successful frame returns the acquire result so a
// SUBOPTIMAL acquire still propagates. Any other non-success VkResult is a hard
// error.
VkResult drawFrame(
    const Renderer& renderer,
    uint32_t frameIndex,
    const RaygenPushConstants& pushConstants,
    const FrameLighting& frameLighting);

// Wait for and read the two GPU timestamps surrounding the most recently submitted
// trace dispatch in frameSlot. The timestamp-period conversion and valid-bit wrap are
// handled here; on failure *milliseconds is unchanged.
VkResult readTraceTimestampMilliseconds(
    const Renderer& renderer,
    std::uint32_t frameSlot,
    double* milliseconds);

// Copy the tonemapped LDR image produced by the latest submitted frame into host memory.
// Rendering must have stopped immediately after finalSubmittedFrameSlot's successful
// draw: at least one draw must have succeeded in that slot, no later frame may be
// queued, no swapchain recreation may intervene, and no thread may concurrently use
// the trace queue or frame command pool. The function waits that slot's render fence,
// performs one semaphore-free copy submission with a private fence on traceQueue, and
// leaves the LDR image in TRANSFER_SRC_OPTIMAL. On failure, *output is unchanged.
VkResult readbackStorageImage(
    const Renderer& renderer,
    std::uint32_t finalSubmittedFrameSlot,
    StorageImageReadback* output);
}
