#include "renderer.hpp"

#include "acceleration_structure.hpp"
#include "gpu_lighting.hpp"
#include "lighting.hpp"
#include "rt_pipeline.hpp"
#include "scene_lighting.hpp"
#include "swapchain.hpp"
#include "tonemap_pipeline.hpp"
#include "vulkan_context.hpp"
#include "vk_mem_alloc.h"

#include <cstdint>
#include <cstring>
#include <iterator>
#include <limits>
#include <new>
#include <utility>

#include <vulkan/vulkan.h>

namespace xrphoton
{
namespace
{
constexpr uint32_t TraceTimestampBeginQuery = 0;
constexpr uint32_t TraceTimestampEndQuery = 1;

class TemporaryBuffer
{
public:
    VmaAllocator allocator = nullptr;
    VkBuffer buffer = VK_NULL_HANDLE;
    VmaAllocation allocation = nullptr;

    TemporaryBuffer() = default;
    TemporaryBuffer(const TemporaryBuffer&) = delete;
    TemporaryBuffer& operator=(const TemporaryBuffer&) = delete;

    ~TemporaryBuffer()
    {
        if (allocator != nullptr
            && buffer != VK_NULL_HANDLE
            && allocation != nullptr) {
            vmaDestroyBuffer(allocator, buffer, allocation);
        }
    }
};

class TemporaryFence
{
public:
    VkDevice device = VK_NULL_HANDLE;
    VkFence fence = VK_NULL_HANDLE;

    TemporaryFence() = default;
    TemporaryFence(const TemporaryFence&) = delete;
    TemporaryFence& operator=(const TemporaryFence&) = delete;

    ~TemporaryFence()
    {
        if (device != VK_NULL_HANDLE && fence != VK_NULL_HANDLE) {
            vkDestroyFence(device, fence, nullptr);
        }
    }
};

bool checkedReadbackByteCount(
    VkExtent2D extent,
    VkDeviceSize* deviceByteCount,
    std::size_t* hostByteCount)
{
    if (deviceByteCount == nullptr
        || hostByteCount == nullptr
        || extent.width == 0
        || extent.height == 0) {
        return false;
    }

    constexpr std::uint64_t ChannelCount = 4;
    const std::uint64_t width = extent.width;
    const std::uint64_t height = extent.height;
    if (width > std::numeric_limits<std::uint64_t>::max() / height) {
        return false;
    }
    const std::uint64_t pixelCount = width * height;
    if (pixelCount > std::numeric_limits<std::uint64_t>::max() / ChannelCount) {
        return false;
    }
    const std::uint64_t byteCount = pixelCount * ChannelCount;
    if (byteCount > std::numeric_limits<VkDeviceSize>::max()
        || byteCount > std::numeric_limits<std::size_t>::max()) {
        return false;
    }

    *deviceByteCount = static_cast<VkDeviceSize>(byteCount);
    *hostByteCount = static_cast<std::size_t>(byteCount);
    return true;
}

void recordImageBarrier(
    VkCommandBuffer commandBuffer,
    VkImage image,
    VkImageLayout oldLayout,
    VkImageLayout newLayout,
    VkAccessFlags srcAccessMask,
    VkAccessFlags dstAccessMask,
    VkPipelineStageFlags srcStageMask,
    VkPipelineStageFlags dstStageMask,
    const VkImageSubresourceRange& subresourceRange)
{
    VkImageMemoryBarrier barrier{};
    barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    barrier.oldLayout = oldLayout;
    barrier.newLayout = newLayout;
    barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.image = image;
    barrier.subresourceRange = subresourceRange;
    barrier.srcAccessMask = srcAccessMask;
    barrier.dstAccessMask = dstAccessMask;

    vkCmdPipelineBarrier(
        commandBuffer,
        srcStageMask,
        dstStageMask,
        0,
        0,
        nullptr,
        0,
        nullptr,
        1,
        &barrier);
}

void recordExecutionBarrier(
    VkCommandBuffer commandBuffer,
    VkPipelineStageFlags srcStageMask,
    VkPipelineStageFlags dstStageMask)
{
    vkCmdPipelineBarrier(
        commandBuffer,
        srcStageMask,
        dstStageMask,
        0,
        0,
        nullptr,
        0,
        nullptr,
        0,
        nullptr);
}

// Record the entire frame into a one-time-submit command buffer:
//   1. rebuild the TLAS between its cross-frame and traversal barriers,
//   2. discard/transition HDR and the G-buffer images, then trace one multi-vertex
//      path per pixel (raygen also writes the primary-hit G-buffer),
//   3. make HDR writes visible to compute and discard/transition the LDR output,
//   4. compute-tonemap HDR radiance into the 8-bit LDR output,
//   5. make LDR writes visible to transfer and blit it into the acquired image,
//   6. close both shared-image cross-frame hazards with execution dependencies,
//   7. transition the acquired image to PRESENT_SRC_KHR.
VkResult recordTraceCommandBuffer(
    VkCommandBuffer commandBuffer,
    VkQueryPool traceTimestampQueryPool,
    const RayTracingFunctions& functions,
    const RtPipeline& rt,
    const TonemapPipeline& tonemap,
    const AccelerationStructure& accel,
    uint32_t frameSlot,
    uint32_t lightingDynamicOffset,
    const RaygenPushConstants& pushConstants,
    VkImage hdrRadianceImage,
    VkImage ldrOutputImage,
    VkImage gbufferNormalDepthImage,
    VkImage gbufferAlbedoImage,
    VkImage gbufferInstanceIdImage,
    VkImage swapchainImage,
    VkExtent2D extent)
{
    VkCommandBufferBeginInfo beginInfo{};
    beginInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    beginInfo.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;

    VkResult result = vkBeginCommandBuffer(commandBuffer, &beginInfo);

    if (result != VK_SUCCESS) {
        return result;
    }

    if (traceTimestampQueryPool != VK_NULL_HANDLE) {
        vkCmdResetQueryPool(
            commandBuffer,
            traceTimestampQueryPool,
            0,
            TraceTimestampQueryCount);
    }

    // writeTlasInstances already validated and populated this frame slot after its
    // fence wait. Rebuild the shared TLAS before any traversal in this command buffer.
    recordTlasRebuild(commandBuffer, functions, accel, frameSlot);

    VkImageSubresourceRange colorRange{};
    colorRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    colorRange.baseMipLevel = 0;
    colorRange.levelCount = 1;
    colorRange.baseArrayLayer = 0;
    colorRange.layerCount = 1;

    // Discard the previous HDR and G-buffer contents and hand the whole images to
    // the raygen shader. The source stage chains from the previous frame's trailing
    // execution barrier without intersecting the acquire wait's TRANSFER stage, so
    // tracing can still run before this frame's swapchain image is acquired. The
    // G-buffer images are written only at RAY_TRACING_SHADER, so the same
    // raygen-to-raygen ordering also closes their cross-frame write-after-write
    // hazard; the discard makes memory availability irrelevant.
    const VkImage raygenStorageImages[] = {
        hdrRadianceImage,
        gbufferNormalDepthImage,
        gbufferAlbedoImage,
        gbufferInstanceIdImage,
    };
    for (VkImage raygenStorageImage : raygenStorageImages) {
        recordImageBarrier(
            commandBuffer,
            raygenStorageImage,
            VK_IMAGE_LAYOUT_UNDEFINED,
            VK_IMAGE_LAYOUT_GENERAL,
            0,
            VK_ACCESS_SHADER_WRITE_BIT,
            VK_PIPELINE_STAGE_RAY_TRACING_SHADER_BIT_KHR,
            VK_PIPELINE_STAGE_RAY_TRACING_SHADER_BIT_KHR,
            colorRange);
    }

    vkCmdBindPipeline(commandBuffer, VK_PIPELINE_BIND_POINT_RAY_TRACING_KHR, rt.pipeline);

    vkCmdBindDescriptorSets(
        commandBuffer,
        VK_PIPELINE_BIND_POINT_RAY_TRACING_KHR,
        rt.pipelineLayout,
        0,
        1,
        &rt.descriptorSet,
        1,
        &lightingDynamicOffset);

    vkCmdPushConstants(
        commandBuffer,
        rt.pipelineLayout,
        VK_SHADER_STAGE_RAYGEN_BIT_KHR,
        0,
        sizeof(RaygenPushConstants),
        &pushConstants);

    // recordTlasRebuild's in-frame post-build barrier made the fresh TLAS visible to
    // this traversal. The dispatch dimensions were gated against the device limits
    // when the swapchain (re)appeared.
    if (traceTimestampQueryPool != VK_NULL_HANDLE) {
        vkCmdWriteTimestamp(
            commandBuffer,
            VK_PIPELINE_STAGE_RAY_TRACING_SHADER_BIT_KHR,
            traceTimestampQueryPool,
            TraceTimestampBeginQuery);
    }

    functions.cmdTraceRays(
        commandBuffer,
        &rt.raygenRegion,
        &rt.missRegion,
        &rt.hitRegion,
        &rt.callableRegion,
        extent.width,
        extent.height,
        1);

    if (traceTimestampQueryPool != VK_NULL_HANDLE) {
        vkCmdWriteTimestamp(
            commandBuffer,
            VK_PIPELINE_STAGE_RAY_TRACING_SHADER_BIT_KHR,
            traceTimestampQueryPool,
            TraceTimestampEndQuery);
    }

    // Preserve HDR radiance in GENERAL while making raygen writes visible to the
    // tonemap compute shader's storage-image reads.
    recordImageBarrier(
        commandBuffer,
        hdrRadianceImage,
        VK_IMAGE_LAYOUT_GENERAL,
        VK_IMAGE_LAYOUT_GENERAL,
        VK_ACCESS_SHADER_WRITE_BIT,
        VK_ACCESS_SHADER_READ_BIT,
        VK_PIPELINE_STAGE_RAY_TRACING_SHADER_BIT_KHR,
        VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
        colorRange);

    // This pass fully overwrites LDR. UNDEFINED intentionally discards last frame's
    // pixels; the previous frame's trailing transfer->compute execution dependency
    // still orders this write after its blit/read completes.
    recordImageBarrier(
        commandBuffer,
        ldrOutputImage,
        VK_IMAGE_LAYOUT_UNDEFINED,
        VK_IMAGE_LAYOUT_GENERAL,
        0,
        VK_ACCESS_SHADER_WRITE_BIT,
        VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
        VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
        colorRange);

    vkCmdBindPipeline(commandBuffer, VK_PIPELINE_BIND_POINT_COMPUTE, tonemap.pipeline);
    vkCmdBindDescriptorSets(
        commandBuffer,
        VK_PIPELINE_BIND_POINT_COMPUTE,
        tonemap.pipelineLayout,
        0,
        1,
        &tonemap.descriptorSet,
        0,
        nullptr);
    vkCmdPushConstants(
        commandBuffer,
        tonemap.pipelineLayout,
        VK_SHADER_STAGE_COMPUTE_BIT,
        0,
        sizeof(TonemapState),
        &tonemap.state);

    const TonemapDispatch tonemapDispatch = makeTonemapDispatch(extent.width, extent.height);
    vkCmdDispatch(commandBuffer, tonemapDispatch.x, tonemapDispatch.y, 1);

    recordImageBarrier(
        commandBuffer,
        ldrOutputImage,
        VK_IMAGE_LAYOUT_GENERAL,
        VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
        VK_ACCESS_SHADER_WRITE_BIT,
        VK_ACCESS_TRANSFER_READ_BIT,
        VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
        VK_PIPELINE_STAGE_TRANSFER_BIT,
        colorRange);

    // The acquired image is first touched at TRANSFER. The submit waits on acquire at
    // TRANSFER, so this transition and the blit are serialized behind acquire — but the
    // trace above runs at RAY_TRACING_SHADER, outside that wait stage, so the GPU may
    // trace before the image is even acquired.
    recordImageBarrier(
        commandBuffer,
        swapchainImage,
        VK_IMAGE_LAYOUT_UNDEFINED,
        VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
        0,
        VK_ACCESS_TRANSFER_WRITE_BIT,
        VK_PIPELINE_STAGE_TRANSFER_BIT,
        VK_PIPELINE_STAGE_TRANSFER_BIT,
        colorRange);

    VkImageBlit blitRegion{};
    blitRegion.srcSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    blitRegion.srcSubresource.baseArrayLayer = 0;
    blitRegion.srcSubresource.layerCount = 1;
    blitRegion.srcOffsets[1] = {
        static_cast<int32_t>(extent.width),
        static_cast<int32_t>(extent.height),
        1,
    };
    blitRegion.dstSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    blitRegion.dstSubresource.baseArrayLayer = 0;
    blitRegion.dstSubresource.layerCount = 1;
    blitRegion.dstOffsets[1] = {
        static_cast<int32_t>(extent.width),
        static_cast<int32_t>(extent.height),
        1,
    };

    // Keep this as a blit, not a copy: blit performs format conversion. The selected
    // swapchain format is sRGB, so the LDR UNORM value is encoded for presentation.
    vkCmdBlitImage(
        commandBuffer,
        ldrOutputImage,
        VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
        swapchainImage,
        VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
        1,
        &blitRegion,
        VK_FILTER_NEAREST);

    // Both resize-bound images are shared by frames in flight. Their next uses discard
    // and overwrite, so write-after-read only needs execution ordering: compute's HDR
    // read must finish before next frame's raygen write, and transfer's LDR read must
    // finish before next frame's compute write. Keep these after each image's last read.
    recordExecutionBarrier(
        commandBuffer,
        VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
        VK_PIPELINE_STAGE_RAY_TRACING_SHADER_BIT_KHR);
    recordExecutionBarrier(
        commandBuffer,
        VK_PIPELINE_STAGE_TRANSFER_BIT,
        VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT);

    // Transition into the layout the presentation engine requires. The dstStageMask is
    // BOTTOM_OF_PIPE because no further GPU stage consumes the image; the render-finished
    // semaphore signaled at submit is what the present actually waits on.
    recordImageBarrier(
        commandBuffer,
        swapchainImage,
        VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
        VK_IMAGE_LAYOUT_PRESENT_SRC_KHR,
        VK_ACCESS_TRANSFER_WRITE_BIT,
        0,
        VK_PIPELINE_STAGE_TRANSFER_BIT,
        VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT,
        colorRange);

    return vkEndCommandBuffer(commandBuffer);
}

} // namespace

bool prepareRtForSwapchain(const Renderer& renderer)
{
    const RtPipeline& rt = *renderer.rtPipeline;
    const TonemapPipeline& tonemap = *renderer.tonemapPipeline;
    const Swapchain& swap = *renderer.swap;

    writeRtDescriptorSet(
        renderer.device,
        rt.descriptorSet,
        renderer.accel->tlas,
        swap.hdrRadianceImageView,
        swap.gbufferNormalDepthImageView,
        swap.gbufferAlbedoImageView,
        swap.gbufferInstanceIdImageView);
    writeTonemapDescriptorSet(
        renderer.device,
        tonemap.descriptorSet,
        swap.hdrRadianceImageView,
        swap.ldrOutputImageView);

    VkPhysicalDeviceRayTracingPipelinePropertiesKHR rtProperties{};
    rtProperties.sType =
        VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_RAY_TRACING_PIPELINE_PROPERTIES_KHR;

    VkPhysicalDeviceProperties2 properties{};
    properties.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2;
    properties.pNext = &rtProperties;

    vkGetPhysicalDeviceProperties2(renderer.physicalDevice, &properties);

    // The vkCmdTraceRaysKHR VUIDs bound each dispatch dimension by the corresponding
    // compute work-group limits (count × size) and the product by
    // maxRayDispatchInvocationCount. Depth is a constant 1, which every device allows.
    const VkPhysicalDeviceLimits& limits = properties.properties.limits;
    const uint64_t width = swap.extent.width;
    const uint64_t height = swap.extent.height;
    const TonemapDispatch tonemapDispatch = makeTonemapDispatch(
        swap.extent.width,
        swap.extent.height);

    return TonemapLocalSizeX <= limits.maxComputeWorkGroupSize[0]
        && TonemapLocalSizeY <= limits.maxComputeWorkGroupSize[1]
        && TonemapLocalSizeX * TonemapLocalSizeY <= limits.maxComputeWorkGroupInvocations
        && tonemapDispatch.x <= limits.maxComputeWorkGroupCount[0]
        && tonemapDispatch.y <= limits.maxComputeWorkGroupCount[1]
        && width <= static_cast<uint64_t>(limits.maxComputeWorkGroupCount[0])
            * limits.maxComputeWorkGroupSize[0]
        && height <= static_cast<uint64_t>(limits.maxComputeWorkGroupCount[1])
            * limits.maxComputeWorkGroupSize[1]
        && width * height <= rtProperties.maxRayDispatchInvocationCount;
}

VkResult drawFrame(
    const Renderer& renderer,
    uint32_t frameIndex,
    const RaygenPushConstants& pushConstants,
    const FrameLighting& frameLighting)
{
    const Swapchain& swap = *renderer.swap;
    const FrameResources& frame = renderer.frames[frameIndex];

    // Block until this slot's previous submission (MaxFramesInFlight frames ago) has
    // completed before reusing its command buffer and sync objects. The other slots'
    // frames deliberately stay in flight — cross-frame ordering on the shared storage
    // image is the barrier chain's job, not this wait's.
    VkResult result = vkWaitForFences(
        renderer.device,
        1,
        &frame.inFlightFence,
        VK_TRUE,
        std::numeric_limits<uint64_t>::max());

    if (result != VK_SUCCESS) {
        return result;
    }

    // The slot fence retires every prior read of this slot's mapped instance and
    // lighting subranges. Keep both fallible writes before image acquisition so a
    // rejected runtime publication cannot strand an image or consumed semaphore.
    uint32_t lightingDynamicOffset = 0;
    if (!writeFrameLightingSlot(
            renderer.gpuLighting,
            frameIndex,
            frameLighting,
            &lightingDynamicOffset)) {
        return VK_ERROR_INITIALIZATION_FAILED;
    }
    if (!writeTlasInstances(renderer.accel, *renderer.scene, frameIndex)) {
        return VK_ERROR_INITIALIZATION_FAILED;
    }

    uint32_t imageIndex = 0;
    result = vkAcquireNextImageKHR(
        renderer.device,
        swap.swapchain,
        std::numeric_limits<uint64_t>::max(),
        frame.imageAvailableSemaphore,
        VK_NULL_HANDLE,
        &imageIndex);

    if (result == VK_ERROR_OUT_OF_DATE_KHR) {
        return result;
    }

    if (result != VK_SUCCESS && result != VK_SUBOPTIMAL_KHR) {
        return result;
    }

    // Preserve the acquire result (may be SUBOPTIMAL) to return on the success path.
    const VkResult acquireResult = result;

    // Defend against a driver returning an out-of-range index before indexing the
    // per-image vectors.
    if (imageIndex >= swap.images.size()
        || imageIndex >= swap.renderFinishedSemaphores.size()) {
        return VK_ERROR_INITIALIZATION_FAILED;
    }

    // Signal completion on the semaphore tied to this specific image (see
    // createRenderFinishedSemaphores), which present then waits on.
    const VkSemaphore renderFinishedSemaphore = swap.renderFinishedSemaphores[imageIndex];

    result = vkResetCommandBuffer(frame.commandBuffer, 0);

    if (result != VK_SUCCESS) {
        return result;
    }

    result = recordTraceCommandBuffer(
        frame.commandBuffer,
        frame.traceTimestampQueryPool,
        *renderer.functions,
        *renderer.rtPipeline,
        *renderer.tonemapPipeline,
        *renderer.accel,
        frameIndex,
        lightingDynamicOffset,
        pushConstants,
        swap.hdrRadianceImage,
        swap.ldrOutputImage,
        swap.gbufferNormalDepthImage,
        swap.gbufferAlbedoImage,
        swap.gbufferInstanceIdImage,
        swap.images[imageIndex],
        swap.extent);

    if (result != VK_SUCCESS) {
        return result;
    }

    // Reset the fence to unsignaled only now that recording succeeded and a submit is
    // guaranteed to follow — otherwise the next frame's wait would block forever.
    result = vkResetFences(renderer.device, 1, &frame.inFlightFence);

    if (result != VK_SUCCESS) {
        return result;
    }

    // Submission waits on the image-available semaphore at the TRANSFER stage, matching
    // the first swapchain touch: the blit destination transition. The trace runs at
    // RAY_TRACING_SHADER, outside that wait stage, so the GPU may overlap it with (or
    // run it before) the acquire — only the blit onto the swapchain image waits.
    const VkSemaphore waitSemaphores[] = {
        frame.imageAvailableSemaphore,
    };
    const VkPipelineStageFlags waitStages[] = {
        VK_PIPELINE_STAGE_TRANSFER_BIT,
    };
    const VkSemaphore signalSemaphores[] = {
        renderFinishedSemaphore,
    };

    VkSubmitInfo submitInfo{};
    submitInfo.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    submitInfo.waitSemaphoreCount = static_cast<uint32_t>(std::size(waitSemaphores));
    submitInfo.pWaitSemaphores = waitSemaphores;
    submitInfo.pWaitDstStageMask = waitStages;
    submitInfo.commandBufferCount = 1;
    submitInfo.pCommandBuffers = &frame.commandBuffer;
    submitInfo.signalSemaphoreCount = static_cast<uint32_t>(std::size(signalSemaphores));
    submitInfo.pSignalSemaphores = signalSemaphores;

    result = vkQueueSubmit(renderer.traceQueue, 1, &submitInfo, frame.inFlightFence);

    if (result != VK_SUCCESS) {
        return result;
    }

    const VkSwapchainKHR swapchains[] = {
        swap.swapchain,
    };

    VkPresentInfoKHR presentInfo{};
    presentInfo.sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR;
    presentInfo.waitSemaphoreCount = static_cast<uint32_t>(std::size(signalSemaphores));
    presentInfo.pWaitSemaphores = signalSemaphores;
    presentInfo.swapchainCount = static_cast<uint32_t>(std::size(swapchains));
    presentInfo.pSwapchains = swapchains;
    presentInfo.pImageIndices = &imageIndex;

    result = vkQueuePresentKHR(renderer.presentQueue, &presentInfo);

    if (result == VK_ERROR_OUT_OF_DATE_KHR || result == VK_SUBOPTIMAL_KHR) {
        return result;
    }

    if (result != VK_SUCCESS) {
        return result;
    }

    // Frame succeeded; surface a SUBOPTIMAL acquire (if any) so the caller can still
    // decide to recreate the swapchain.
    return acquireResult;
}

VkResult readTraceTimestampMilliseconds(
    const Renderer& renderer,
    std::uint32_t frameSlot,
    double* milliseconds)
{
    if (milliseconds == nullptr
        || frameSlot >= MaxFramesInFlight
        || renderer.device == VK_NULL_HANDLE
        || renderer.frames == nullptr
        || renderer.frames[frameSlot].traceTimestampQueryPool == VK_NULL_HANDLE
        || renderer.traceTimestampPeriod <= 0.0f
        || renderer.traceTimestampValidBits == 0
        || renderer.traceTimestampValidBits > 64) {
        return VK_ERROR_INITIALIZATION_FAILED;
    }

    uint64_t timestamps[TraceTimestampQueryCount]{};
    const VkResult result = vkGetQueryPoolResults(
        renderer.device,
        renderer.frames[frameSlot].traceTimestampQueryPool,
        0,
        TraceTimestampQueryCount,
        sizeof(timestamps),
        timestamps,
        sizeof(timestamps[0]),
        VK_QUERY_RESULT_64_BIT | VK_QUERY_RESULT_WAIT_BIT);
    if (result != VK_SUCCESS) {
        return result;
    }

    const uint64_t validMask = renderer.traceTimestampValidBits == 64
        ? std::numeric_limits<uint64_t>::max()
        : (uint64_t{1} << renderer.traceTimestampValidBits) - 1;
    const uint64_t elapsedTicks =
        (timestamps[TraceTimestampEndQuery]
            - timestamps[TraceTimestampBeginQuery])
        & validMask;
    const double elapsedMilliseconds = static_cast<double>(elapsedTicks)
        * static_cast<double>(renderer.traceTimestampPeriod)
        / 1'000'000.0;

    *milliseconds = elapsedMilliseconds;
    return VK_SUCCESS;
}

VkResult readbackStorageImage(
    const Renderer& renderer,
    std::uint32_t finalSubmittedFrameSlot,
    StorageImageReadback* output)
{
    if (output == nullptr
        || finalSubmittedFrameSlot >= MaxFramesInFlight
        || renderer.device == VK_NULL_HANDLE
        || renderer.allocator == nullptr
        || renderer.traceQueue == VK_NULL_HANDLE
        || renderer.frames == nullptr
        || renderer.swap == nullptr
        || renderer.swap->ldrOutputImage == VK_NULL_HANDLE) {
        return VK_ERROR_INITIALIZATION_FAILED;
    }

    const Swapchain& swap = *renderer.swap;
    VkDeviceSize deviceByteCount = 0;
    std::size_t hostByteCount = 0;
    if (!checkedReadbackByteCount(
            swap.extent,
            &deviceByteCount,
            &hostByteCount)) {
        return VK_ERROR_INITIALIZATION_FAILED;
    }

    const FrameResources& frame = renderer.frames[finalSubmittedFrameSlot];
    if (frame.commandBuffer == VK_NULL_HANDLE
        || frame.inFlightFence == VK_NULL_HANDLE) {
        return VK_ERROR_INITIALIZATION_FAILED;
    }

    try {
        VkResult result = vkWaitForFences(
            renderer.device,
            1,
            &frame.inFlightFence,
            VK_TRUE,
            std::numeric_limits<std::uint64_t>::max());
        if (result != VK_SUCCESS) {
            return result;
        }

        StorageImageReadback candidate;
        candidate.width = swap.extent.width;
        candidate.height = swap.extent.height;
        if (hostByteCount > candidate.rgba8.max_size()) {
            return VK_ERROR_OUT_OF_HOST_MEMORY;
        }
        candidate.rgba8.resize(hostByteCount);

        TemporaryBuffer readbackBuffer;
        readbackBuffer.allocator = renderer.allocator;
        result = createBuffer(
            renderer.allocator,
            deviceByteCount,
            VK_BUFFER_USAGE_TRANSFER_DST_BIT,
            VMA_ALLOCATION_CREATE_HOST_ACCESS_RANDOM_BIT
                | VMA_ALLOCATION_CREATE_MAPPED_BIT,
            &readbackBuffer.buffer,
            &readbackBuffer.allocation);
        if (result != VK_SUCCESS) {
            return result;
        }

        VmaAllocationInfo allocationInfo{};
        vmaGetAllocationInfo(
            renderer.allocator,
            readbackBuffer.allocation,
            &allocationInfo);
        if (allocationInfo.pMappedData == nullptr) {
            return VK_ERROR_MEMORY_MAP_FAILED;
        }

        result = vkResetCommandBuffer(frame.commandBuffer, 0);
        if (result != VK_SUCCESS) {
            return result;
        }

        VkCommandBufferBeginInfo beginInfo{};
        beginInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
        beginInfo.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        result = vkBeginCommandBuffer(frame.commandBuffer, &beginInfo);
        if (result != VK_SUCCESS) {
            return result;
        }

        VkBufferImageCopy copyRegion{};
        copyRegion.bufferOffset = 0;
        copyRegion.bufferRowLength = 0;
        copyRegion.bufferImageHeight = 0;
        copyRegion.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        copyRegion.imageSubresource.mipLevel = 0;
        copyRegion.imageSubresource.baseArrayLayer = 0;
        copyRegion.imageSubresource.layerCount = 1;
        copyRegion.imageOffset = {0, 0, 0};
        copyRegion.imageExtent = {
            swap.extent.width,
            swap.extent.height,
            1,
        };

        vkCmdCopyImageToBuffer(
            frame.commandBuffer,
            swap.ldrOutputImage,
            VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
            readbackBuffer.buffer,
            1,
            &copyRegion);

        VkBufferMemoryBarrier hostReadBarrier{};
        hostReadBarrier.sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER;
        hostReadBarrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        hostReadBarrier.dstAccessMask = VK_ACCESS_HOST_READ_BIT;
        hostReadBarrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        hostReadBarrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        hostReadBarrier.buffer = readbackBuffer.buffer;
        hostReadBarrier.offset = 0;
        hostReadBarrier.size = deviceByteCount;

        vkCmdPipelineBarrier(
            frame.commandBuffer,
            VK_PIPELINE_STAGE_TRANSFER_BIT,
            VK_PIPELINE_STAGE_HOST_BIT,
            0,
            0,
            nullptr,
            1,
            &hostReadBarrier,
            0,
            nullptr);

        result = vkEndCommandBuffer(frame.commandBuffer);
        if (result != VK_SUCCESS) {
            return result;
        }

        // Use a private fence for the one-shot copy. The frame slot's render fence
        // remains signaled, so a failed readback submit cannot poison later slot reuse
        // even though capture itself treats every readback failure as terminal.
        TemporaryFence copyFence;
        copyFence.device = renderer.device;
        VkFenceCreateInfo fenceCreateInfo{};
        fenceCreateInfo.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
        result = vkCreateFence(
            renderer.device,
            &fenceCreateInfo,
            nullptr,
            &copyFence.fence);
        if (result != VK_SUCCESS) {
            return result;
        }

        VkSubmitInfo submitInfo{};
        submitInfo.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
        submitInfo.commandBufferCount = 1;
        submitInfo.pCommandBuffers = &frame.commandBuffer;
        result = vkQueueSubmit(
            renderer.traceQueue,
            1,
            &submitInfo,
            copyFence.fence);
        if (result != VK_SUCCESS) {
            return result;
        }

        result = vkWaitForFences(
            renderer.device,
            1,
            &copyFence.fence,
            VK_TRUE,
            std::numeric_limits<std::uint64_t>::max());
        if (result != VK_SUCCESS) {
            // DEVICE_LOST is equivalent to retirement for resource-lifetime purposes.
            // For another wait failure, make a bounded retirement attempt before the
            // temporary owners unwind; device-idle is the fallback if queue-idle
            // itself fails.
            if (result != VK_ERROR_DEVICE_LOST) {
                VkResult retirementResult =
                    vkQueueWaitIdle(renderer.traceQueue);
                if (retirementResult != VK_SUCCESS
                    && retirementResult != VK_ERROR_DEVICE_LOST) {
                    retirementResult = vkDeviceWaitIdle(renderer.device);
                }
                if (retirementResult == VK_ERROR_DEVICE_LOST) {
                    return VK_ERROR_DEVICE_LOST;
                }
            }
            return result;
        }

        result = vmaInvalidateAllocation(
            renderer.allocator,
            readbackBuffer.allocation,
            0,
            hostByteCount);
        if (result != VK_SUCCESS) {
            return result;
        }

        std::memcpy(
            candidate.rgba8.data(),
            allocationInfo.pMappedData,
            hostByteCount);
        *output = std::move(candidate);
        return VK_SUCCESS;
    } catch (const std::bad_alloc&) {
        return VK_ERROR_OUT_OF_HOST_MEMORY;
    } catch (...) {
        return VK_ERROR_UNKNOWN;
    }
}

VkResult readbackHdrImage(
    const Renderer& renderer,
    std::uint32_t submittedFrameSlot,
    HdrImageReadback* output)
{
    if (output == nullptr
        || submittedFrameSlot >= MaxFramesInFlight
        || renderer.device == VK_NULL_HANDLE
        || renderer.allocator == nullptr
        || renderer.traceQueue == VK_NULL_HANDLE
        || renderer.frames == nullptr
        || renderer.swap == nullptr
        || renderer.swap->hdrRadianceImage == VK_NULL_HANDLE) {
        return VK_ERROR_INITIALIZATION_FAILED;
    }

    const Swapchain& swap = *renderer.swap;
    const std::uint64_t pixelCount =
        static_cast<std::uint64_t>(swap.extent.width) * swap.extent.height;
    constexpr std::uint64_t BytesPerPixel = 8;
    if (swap.extent.width == 0 || swap.extent.height == 0
        || pixelCount > std::numeric_limits<std::uint64_t>::max() / BytesPerPixel
        || pixelCount * BytesPerPixel > std::numeric_limits<VkDeviceSize>::max()
        || pixelCount * 4 > std::numeric_limits<std::size_t>::max()) {
        return VK_ERROR_INITIALIZATION_FAILED;
    }
    const VkDeviceSize byteCount = pixelCount * BytesPerPixel;
    const std::size_t wordCount = static_cast<std::size_t>(pixelCount * 4);
    const FrameResources& frame = renderer.frames[submittedFrameSlot];
    if (frame.commandBuffer == VK_NULL_HANDLE
        || frame.inFlightFence == VK_NULL_HANDLE) {
        return VK_ERROR_INITIALIZATION_FAILED;
    }

    try {
        VkResult result = vkWaitForFences(
            renderer.device,
            1,
            &frame.inFlightFence,
            VK_TRUE,
            std::numeric_limits<std::uint64_t>::max());
        if (result != VK_SUCCESS) {
            return result;
        }

        HdrImageReadback candidate;
        candidate.width = swap.extent.width;
        candidate.height = swap.extent.height;
        if (wordCount > candidate.rgba16.max_size()) {
            return VK_ERROR_OUT_OF_HOST_MEMORY;
        }
        candidate.rgba16.resize(wordCount);

        TemporaryBuffer readbackBuffer;
        readbackBuffer.allocator = renderer.allocator;
        result = createBuffer(
            renderer.allocator,
            byteCount,
            VK_BUFFER_USAGE_TRANSFER_DST_BIT,
            VMA_ALLOCATION_CREATE_HOST_ACCESS_RANDOM_BIT
                | VMA_ALLOCATION_CREATE_MAPPED_BIT,
            &readbackBuffer.buffer,
            &readbackBuffer.allocation);
        if (result != VK_SUCCESS) {
            return result;
        }
        VmaAllocationInfo allocationInfo{};
        vmaGetAllocationInfo(
            renderer.allocator,
            readbackBuffer.allocation,
            &allocationInfo);
        if (allocationInfo.pMappedData == nullptr) {
            return VK_ERROR_MEMORY_MAP_FAILED;
        }

        result = vkResetCommandBuffer(frame.commandBuffer, 0);
        if (result != VK_SUCCESS) {
            return result;
        }
        VkCommandBufferBeginInfo beginInfo{};
        beginInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
        beginInfo.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        result = vkBeginCommandBuffer(frame.commandBuffer, &beginInfo);
        if (result != VK_SUCCESS) {
            return result;
        }

        VkImageSubresourceRange colorRange{};
        colorRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        colorRange.levelCount = 1;
        colorRange.layerCount = 1;
        recordImageBarrier(
            frame.commandBuffer,
            swap.hdrRadianceImage,
            VK_IMAGE_LAYOUT_GENERAL,
            VK_IMAGE_LAYOUT_GENERAL,
            VK_ACCESS_SHADER_WRITE_BIT | VK_ACCESS_SHADER_READ_BIT,
            VK_ACCESS_TRANSFER_READ_BIT,
            VK_PIPELINE_STAGE_RAY_TRACING_SHADER_BIT_KHR
                | VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
            VK_PIPELINE_STAGE_TRANSFER_BIT,
            colorRange);

        VkBufferImageCopy copyRegion{};
        copyRegion.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        copyRegion.imageSubresource.layerCount = 1;
        copyRegion.imageExtent = {
            swap.extent.width,
            swap.extent.height,
            1,
        };
        vkCmdCopyImageToBuffer(
            frame.commandBuffer,
            swap.hdrRadianceImage,
            VK_IMAGE_LAYOUT_GENERAL,
            readbackBuffer.buffer,
            1,
            &copyRegion);

        VkBufferMemoryBarrier hostReadBarrier{};
        hostReadBarrier.sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER;
        hostReadBarrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        hostReadBarrier.dstAccessMask = VK_ACCESS_HOST_READ_BIT;
        hostReadBarrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        hostReadBarrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        hostReadBarrier.buffer = readbackBuffer.buffer;
        hostReadBarrier.size = byteCount;
        vkCmdPipelineBarrier(
            frame.commandBuffer,
            VK_PIPELINE_STAGE_TRANSFER_BIT,
            VK_PIPELINE_STAGE_HOST_BIT,
            0,
            0,
            nullptr,
            1,
            &hostReadBarrier,
            0,
            nullptr);
        recordExecutionBarrier(
            frame.commandBuffer,
            VK_PIPELINE_STAGE_TRANSFER_BIT,
            VK_PIPELINE_STAGE_RAY_TRACING_SHADER_BIT_KHR);

        result = vkEndCommandBuffer(frame.commandBuffer);
        if (result != VK_SUCCESS) {
            return result;
        }

        TemporaryFence copyFence;
        copyFence.device = renderer.device;
        VkFenceCreateInfo fenceCreateInfo{};
        fenceCreateInfo.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
        result = vkCreateFence(
            renderer.device,
            &fenceCreateInfo,
            nullptr,
            &copyFence.fence);
        if (result != VK_SUCCESS) {
            return result;
        }
        VkSubmitInfo submitInfo{};
        submitInfo.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
        submitInfo.commandBufferCount = 1;
        submitInfo.pCommandBuffers = &frame.commandBuffer;
        result = vkQueueSubmit(
            renderer.traceQueue,
            1,
            &submitInfo,
            copyFence.fence);
        if (result != VK_SUCCESS) {
            return result;
        }
        result = vkWaitForFences(
            renderer.device,
            1,
            &copyFence.fence,
            VK_TRUE,
            std::numeric_limits<std::uint64_t>::max());
        if (result != VK_SUCCESS) {
            if (result != VK_ERROR_DEVICE_LOST) {
                (void)vkQueueWaitIdle(renderer.traceQueue);
            }
            return result;
        }

        result = vmaInvalidateAllocation(
            renderer.allocator,
            readbackBuffer.allocation,
            0,
            byteCount);
        if (result != VK_SUCCESS) {
            return result;
        }

        std::memcpy(
            candidate.rgba16.data(),
            allocationInfo.pMappedData,
            static_cast<std::size_t>(byteCount));
        *output = std::move(candidate);
        return VK_SUCCESS;
    } catch (const std::bad_alloc&) {
        return VK_ERROR_OUT_OF_HOST_MEMORY;
    } catch (...) {
        return VK_ERROR_UNKNOWN;
    }
}

VkResult readbackGBufferImages(
    const Renderer& renderer,
    std::uint32_t submittedFrameSlot,
    GBufferReadback* output)
{
    if (output == nullptr
        || submittedFrameSlot >= MaxFramesInFlight
        || renderer.device == VK_NULL_HANDLE
        || renderer.allocator == nullptr
        || renderer.traceQueue == VK_NULL_HANDLE
        || renderer.frames == nullptr
        || renderer.swap == nullptr
        || renderer.swap->gbufferNormalDepthImage == VK_NULL_HANDLE
        || renderer.swap->gbufferAlbedoImage == VK_NULL_HANDLE
        || renderer.swap->gbufferInstanceIdImage == VK_NULL_HANDLE) {
        return VK_ERROR_INITIALIZATION_FAILED;
    }

    const Swapchain& swap = *renderer.swap;
    const std::uint64_t pixelCount =
        static_cast<std::uint64_t>(swap.extent.width) * swap.extent.height;
    // One staging buffer holds all three images: 8 bytes of normal + depth, then
    // 4 bytes of albedo, then 4 bytes of instance ID per pixel. The section offsets
    // 0 / 8N / 12N are all multiples of their section's texel size, as
    // vkCmdCopyImageToBuffer requires.
    constexpr std::uint64_t NormalDepthBytesPerPixel = 8;
    constexpr std::uint64_t AlbedoBytesPerPixel = 4;
    constexpr std::uint64_t InstanceIdBytesPerPixel = 4;
    constexpr std::uint64_t TotalBytesPerPixel = NormalDepthBytesPerPixel
        + AlbedoBytesPerPixel
        + InstanceIdBytesPerPixel;
    if (swap.extent.width == 0 || swap.extent.height == 0
        || pixelCount > std::numeric_limits<std::uint64_t>::max() / TotalBytesPerPixel
        || pixelCount * TotalBytesPerPixel > std::numeric_limits<VkDeviceSize>::max()
        || pixelCount * TotalBytesPerPixel
            > std::numeric_limits<std::size_t>::max()) {
        return VK_ERROR_INITIALIZATION_FAILED;
    }
    const VkDeviceSize byteCount = pixelCount * TotalBytesPerPixel;
    const VkDeviceSize albedoOffset = pixelCount * NormalDepthBytesPerPixel;
    const VkDeviceSize instanceIdOffset = albedoOffset
        + pixelCount * AlbedoBytesPerPixel;
    const FrameResources& frame = renderer.frames[submittedFrameSlot];
    if (frame.commandBuffer == VK_NULL_HANDLE
        || frame.inFlightFence == VK_NULL_HANDLE) {
        return VK_ERROR_INITIALIZATION_FAILED;
    }

    try {
        VkResult result = vkWaitForFences(
            renderer.device,
            1,
            &frame.inFlightFence,
            VK_TRUE,
            std::numeric_limits<std::uint64_t>::max());
        if (result != VK_SUCCESS) {
            return result;
        }

        GBufferReadback candidate;
        candidate.width = swap.extent.width;
        candidate.height = swap.extent.height;
        const std::size_t normalDepthWordCount =
            static_cast<std::size_t>(pixelCount * 4);
        const std::size_t albedoByteCount =
            static_cast<std::size_t>(pixelCount * AlbedoBytesPerPixel);
        const std::size_t instanceIdCount = static_cast<std::size_t>(pixelCount);
        if (normalDepthWordCount > candidate.normalDepthRgba16.max_size()
            || albedoByteCount > candidate.albedoRgba8.max_size()
            || instanceIdCount > candidate.instanceIds.max_size()) {
            return VK_ERROR_OUT_OF_HOST_MEMORY;
        }
        candidate.normalDepthRgba16.resize(normalDepthWordCount);
        candidate.albedoRgba8.resize(albedoByteCount);
        candidate.instanceIds.resize(instanceIdCount);

        TemporaryBuffer readbackBuffer;
        readbackBuffer.allocator = renderer.allocator;
        result = createBuffer(
            renderer.allocator,
            byteCount,
            VK_BUFFER_USAGE_TRANSFER_DST_BIT,
            VMA_ALLOCATION_CREATE_HOST_ACCESS_RANDOM_BIT
                | VMA_ALLOCATION_CREATE_MAPPED_BIT,
            &readbackBuffer.buffer,
            &readbackBuffer.allocation);
        if (result != VK_SUCCESS) {
            return result;
        }
        VmaAllocationInfo allocationInfo{};
        vmaGetAllocationInfo(
            renderer.allocator,
            readbackBuffer.allocation,
            &allocationInfo);
        if (allocationInfo.pMappedData == nullptr) {
            return VK_ERROR_MEMORY_MAP_FAILED;
        }

        result = vkResetCommandBuffer(frame.commandBuffer, 0);
        if (result != VK_SUCCESS) {
            return result;
        }
        VkCommandBufferBeginInfo beginInfo{};
        beginInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
        beginInfo.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        result = vkBeginCommandBuffer(frame.commandBuffer, &beginInfo);
        if (result != VK_SUCCESS) {
            return result;
        }

        VkImageSubresourceRange colorRange{};
        colorRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        colorRange.levelCount = 1;
        colorRange.layerCount = 1;

        const VkImage gbufferImages[] = {
            swap.gbufferNormalDepthImage,
            swap.gbufferAlbedoImage,
            swap.gbufferInstanceIdImage,
        };
        const VkDeviceSize sectionOffsets[] = {
            0,
            albedoOffset,
            instanceIdOffset,
        };
        for (const VkImage gbufferImage : gbufferImages) {
            // Raygen is the only writer of these images, so making its writes
            // visible to transfer is the whole hazard.
            recordImageBarrier(
                frame.commandBuffer,
                gbufferImage,
                VK_IMAGE_LAYOUT_GENERAL,
                VK_IMAGE_LAYOUT_GENERAL,
                VK_ACCESS_SHADER_WRITE_BIT,
                VK_ACCESS_TRANSFER_READ_BIT,
                VK_PIPELINE_STAGE_RAY_TRACING_SHADER_BIT_KHR,
                VK_PIPELINE_STAGE_TRANSFER_BIT,
                colorRange);
        }
        for (std::size_t imageIndex = 0;
             imageIndex < std::size(gbufferImages);
             ++imageIndex) {
            VkBufferImageCopy copyRegion{};
            copyRegion.bufferOffset = sectionOffsets[imageIndex];
            copyRegion.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
            copyRegion.imageSubresource.layerCount = 1;
            copyRegion.imageExtent = {
                swap.extent.width,
                swap.extent.height,
                1,
            };
            vkCmdCopyImageToBuffer(
                frame.commandBuffer,
                gbufferImages[imageIndex],
                VK_IMAGE_LAYOUT_GENERAL,
                readbackBuffer.buffer,
                1,
                &copyRegion);
        }

        VkBufferMemoryBarrier hostReadBarrier{};
        hostReadBarrier.sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER;
        hostReadBarrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        hostReadBarrier.dstAccessMask = VK_ACCESS_HOST_READ_BIT;
        hostReadBarrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        hostReadBarrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        hostReadBarrier.buffer = readbackBuffer.buffer;
        hostReadBarrier.size = byteCount;
        vkCmdPipelineBarrier(
            frame.commandBuffer,
            VK_PIPELINE_STAGE_TRANSFER_BIT,
            VK_PIPELINE_STAGE_HOST_BIT,
            0,
            0,
            nullptr,
            1,
            &hostReadBarrier,
            0,
            nullptr);
        recordExecutionBarrier(
            frame.commandBuffer,
            VK_PIPELINE_STAGE_TRANSFER_BIT,
            VK_PIPELINE_STAGE_RAY_TRACING_SHADER_BIT_KHR);

        result = vkEndCommandBuffer(frame.commandBuffer);
        if (result != VK_SUCCESS) {
            return result;
        }

        TemporaryFence copyFence;
        copyFence.device = renderer.device;
        VkFenceCreateInfo fenceCreateInfo{};
        fenceCreateInfo.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
        result = vkCreateFence(
            renderer.device,
            &fenceCreateInfo,
            nullptr,
            &copyFence.fence);
        if (result != VK_SUCCESS) {
            return result;
        }
        VkSubmitInfo submitInfo{};
        submitInfo.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
        submitInfo.commandBufferCount = 1;
        submitInfo.pCommandBuffers = &frame.commandBuffer;
        result = vkQueueSubmit(
            renderer.traceQueue,
            1,
            &submitInfo,
            copyFence.fence);
        if (result != VK_SUCCESS) {
            return result;
        }
        result = vkWaitForFences(
            renderer.device,
            1,
            &copyFence.fence,
            VK_TRUE,
            std::numeric_limits<std::uint64_t>::max());
        if (result != VK_SUCCESS) {
            if (result != VK_ERROR_DEVICE_LOST) {
                (void)vkQueueWaitIdle(renderer.traceQueue);
            }
            return result;
        }

        result = vmaInvalidateAllocation(
            renderer.allocator,
            readbackBuffer.allocation,
            0,
            byteCount);
        if (result != VK_SUCCESS) {
            return result;
        }

        const std::uint8_t* mappedBytes =
            static_cast<const std::uint8_t*>(allocationInfo.pMappedData);
        std::memcpy(
            candidate.normalDepthRgba16.data(),
            mappedBytes,
            static_cast<std::size_t>(albedoOffset));
        std::memcpy(
            candidate.albedoRgba8.data(),
            mappedBytes + albedoOffset,
            albedoByteCount);
        std::memcpy(
            candidate.instanceIds.data(),
            mappedBytes + instanceIdOffset,
            instanceIdCount * sizeof(std::uint32_t));
        *output = std::move(candidate);
        return VK_SUCCESS;
    } catch (const std::bad_alloc&) {
        return VK_ERROR_OUT_OF_HOST_MEMORY;
    } catch (...) {
        return VK_ERROR_UNKNOWN;
    }
}
}
