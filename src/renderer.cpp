#include "renderer.hpp"

#include "camera.hpp"
#include "rt_pipeline.hpp"
#include "swapchain.hpp"
#include "vulkan_context.hpp"

#include <cstdint>
#include <iterator>
#include <limits>

#include <vulkan/vulkan.h>

namespace xrphoton
{
namespace
{
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
//   1. barrier the storage image UNDEFINED -> GENERAL,
//   2. trace: one ray per pixel writes the storage image (triangle over dark red),
//   3. barrier storage GENERAL -> TRANSFER_SRC_OPTIMAL,
//   4. barrier the acquired image UNDEFINED -> TRANSFER_DST_OPTIMAL,
//   5. blit storage into the acquired image,
//   6. chain the next trace behind this frame's storage-image read,
//   7. barrier the acquired image TRANSFER_DST_OPTIMAL -> PRESENT_SRC_KHR.
VkResult recordTraceCommandBuffer(
    VkCommandBuffer commandBuffer,
    const RayTracingFunctions& functions,
    const RtPipeline& rt,
    const CameraPushConstants& camera,
    VkImage storageImage,
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

    VkImageSubresourceRange colorRange{};
    colorRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    colorRange.baseMipLevel = 0;
    colorRange.levelCount = 1;
    colorRange.baseArrayLayer = 0;
    colorRange.layerCount = 1;

    // Discard the previous storage contents and hand the whole image to the raygen
    // shader. The source stage chains from the previous frame's trailing execution
    // barrier without intersecting the acquire wait's TRANSFER stage, so tracing can
    // still run before this frame's swapchain image is acquired.
    recordImageBarrier(
        commandBuffer,
        storageImage,
        VK_IMAGE_LAYOUT_UNDEFINED,
        VK_IMAGE_LAYOUT_GENERAL,
        0,
        VK_ACCESS_SHADER_WRITE_BIT,
        VK_PIPELINE_STAGE_RAY_TRACING_SHADER_BIT_KHR,
        VK_PIPELINE_STAGE_RAY_TRACING_SHADER_BIT_KHR,
        colorRange);

    vkCmdBindPipeline(commandBuffer, VK_PIPELINE_BIND_POINT_RAY_TRACING_KHR, rt.pipeline);

    vkCmdBindDescriptorSets(
        commandBuffer,
        VK_PIPELINE_BIND_POINT_RAY_TRACING_KHR,
        rt.pipelineLayout,
        0,
        1,
        &rt.descriptorSet,
        0,
        nullptr);

    vkCmdPushConstants(
        commandBuffer,
        rt.pipelineLayout,
        VK_SHADER_STAGE_RAYGEN_BIT_KHR,
        0,
        sizeof(CameraPushConstants),
        &camera);

    // No acceleration-structure barrier here: the AS build's trailing barrier already
    // made the TLAS visible to every future RAY_TRACING_SHADER read. The dispatch
    // dimensions were gated against the device limits when the swapchain (re)appeared.
    functions.cmdTraceRays(
        commandBuffer,
        &rt.raygenRegion,
        &rt.missRegion,
        &rt.hitRegion,
        &rt.callableRegion,
        extent.width,
        extent.height,
        1);

    // Trace writes become visible to the blit's reads.
    recordImageBarrier(
        commandBuffer,
        storageImage,
        VK_IMAGE_LAYOUT_GENERAL,
        VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
        VK_ACCESS_SHADER_WRITE_BIT,
        VK_ACCESS_TRANSFER_READ_BIT,
        VK_PIPELINE_STAGE_RAY_TRACING_SHADER_BIT_KHR,
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

    // Keep this as a blit, not a copy: blit performs format conversion. If the selected
    // swapchain format is sRGB, the storage UNORM value is encoded for presentation here.
    vkCmdBlitImage(
        commandBuffer,
        storageImage,
        VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
        swapchainImage,
        VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
        1,
        &blitRegion,
        VK_FILTER_NEAREST);

    // A later frame may discard/transition the shared storage image before this frame
    // retires. Chain that next trace behind this blit's storage-image read without
    // creating a memory dependency; a write-after-read hazard only needs execution
    // ordering, and using RAY_TRACING_SHADER as the destination avoids the acquire wait's
    // TRANSFER stage. This barrier must stay after the frame's LAST storage-image read:
    // a read recorded below it would sit outside the dependency and silently reopen the
    // cross-frame hazard.
    recordExecutionBarrier(
        commandBuffer,
        VK_PIPELINE_STAGE_TRANSFER_BIT,
        VK_PIPELINE_STAGE_RAY_TRACING_SHADER_BIT_KHR);

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
    const Swapchain& swap = *renderer.swap;

    writeRtDescriptorSet(renderer.device, rt.descriptorSet, renderer.tlas, swap.storageImageView);

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

    return width <= static_cast<uint64_t>(limits.maxComputeWorkGroupCount[0])
            * limits.maxComputeWorkGroupSize[0]
        && height <= static_cast<uint64_t>(limits.maxComputeWorkGroupCount[1])
            * limits.maxComputeWorkGroupSize[1]
        && width * height <= rtProperties.maxRayDispatchInvocationCount;
}

VkResult drawFrame(
    const Renderer& renderer,
    uint32_t frameIndex,
    const CameraPushConstants& camera)
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
        *renderer.functions,
        *renderer.rtPipeline,
        camera,
        swap.storageImage,
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
}
