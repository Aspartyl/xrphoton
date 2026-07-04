#include "acceleration_structure.hpp"
#include "swapchain.hpp"
#include "vulkan_context.hpp"

#include <cstdint>
#include <iostream>
#include <iterator>
#include <limits>
#include <vector>

#include <vulkan/vulkan.h>
#include <GLFW/glfw3.h>

using namespace xrphoton;

namespace
{
constexpr int WindowWidth = 1920;
constexpr int WindowHeight = 1080;
constexpr const char* WindowTitle = "xrPhoton";

// Compile-time request from the XRPHOTON_ENABLE_VALIDATION CMake option. The runtime
// decision (validationEnabled in main) additionally requires the layer and debug-utils
// extension to actually be present, so a build with validation on still runs on
// machines without the Vulkan SDK — just without validation coverage.
#ifdef XRPHOTON_ENABLE_VALIDATION
constexpr bool ValidationRequested = true;
#else
constexpr bool ValidationRequested = false;
#endif

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

// Record the entire (placeholder) frame into a one-time-submit command buffer:
//   1. barrier the storage image UNDEFINED -> TRANSFER_DST_OPTIMAL,
//   2. clear storage to a solid dark red,
//   3. barrier storage TRANSFER_DST_OPTIMAL -> TRANSFER_SRC_OPTIMAL,
//   4. barrier the acquired image UNDEFINED -> TRANSFER_DST_OPTIMAL,
//   5. blit storage into the acquired image,
//   6. barrier the acquired image TRANSFER_DST_OPTIMAL -> PRESENT_SRC_KHR.
// This is the seed of the renderer; real ray tracing output will replace the storage
// clear while the storage->swapchain presentation path remains.
VkResult recordClearSwapchainImageCommandBuffer(
    VkCommandBuffer commandBuffer,
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

    // Discard the previous storage contents and make the whole image writable by the
    // placeholder transfer clear. Future traceRays work replaces this clear.
    recordImageBarrier(
        commandBuffer,
        storageImage,
        VK_IMAGE_LAYOUT_UNDEFINED,
        VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
        0,
        VK_ACCESS_TRANSFER_WRITE_BIT,
        VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
        VK_PIPELINE_STAGE_TRANSFER_BIT,
        colorRange);

    // The placeholder frame color (linear RGBA): a dark red.
    VkClearColorValue clearColor = {
        {
            0.24f,
            0.02f,
            0.015f,
            1.0f,
        },
    };

    vkCmdClearColorImage(
        commandBuffer,
        storageImage,
        VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
        &clearColor,
        1,
        &colorRange);

    recordImageBarrier(
        commandBuffer,
        storageImage,
        VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
        VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
        VK_ACCESS_TRANSFER_WRITE_BIT,
        VK_ACCESS_TRANSFER_READ_BIT,
        VK_PIPELINE_STAGE_TRANSFER_BIT,
        VK_PIPELINE_STAGE_TRANSFER_BIT,
        colorRange);

    // The acquired image is first touched at TRANSFER. The submit waits on acquire at
    // TRANSFER, so both this swapchain transition and this step's storage clear/blit are
    // serialized behind acquire; true pre-acquire overlap begins once traceRays writes
    // storage at RAY_TRACING_SHADER instead of using a transfer clear.
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

// Render and present one frame (a single frame in flight). Steps: wait the in-flight
// fence -> acquire an image -> record and submit the clear -> present. OUT_OF_DATE and
// SUBOPTIMAL are returned (not treated as errors) so main() can trigger a swapchain
// recreate; a successful frame returns the acquire result so a SUBOPTIMAL acquire still
// propagates. Any other non-success VkResult is a hard error.
VkResult drawFrame(
    VulkanContext& ctx,
    Swapchain& swap,
    VkQueue traceQueue,
    VkQueue presentQueue)
{
    // Block until the previous frame's submission has completed before reusing its
    // command buffer and sync objects.
    VkResult result = vkWaitForFences(
        ctx.device,
        1,
        &ctx.inFlightFence,
        VK_TRUE,
        std::numeric_limits<uint64_t>::max());

    if (result != VK_SUCCESS) {
        return result;
    }

    uint32_t imageIndex = 0;
    result = vkAcquireNextImageKHR(
        ctx.device,
        swap.swapchain,
        std::numeric_limits<uint64_t>::max(),
        ctx.imageAvailableSemaphore,
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

    result = vkResetCommandBuffer(ctx.commandBuffer, 0);

    if (result != VK_SUCCESS) {
        return result;
    }

    result = recordClearSwapchainImageCommandBuffer(
        ctx.commandBuffer,
        swap.storageImage,
        swap.images[imageIndex],
        swap.extent);

    if (result != VK_SUCCESS) {
        return result;
    }

    // Reset the fence to unsignaled only now that recording succeeded and a submit is
    // guaranteed to follow — otherwise the next frame's wait would block forever.
    result = vkResetFences(ctx.device, 1, &ctx.inFlightFence);

    if (result != VK_SUCCESS) {
        return result;
    }

    // Submission waits on the image-available semaphore at the TRANSFER stage, matching
    // the first swapchain touch: the blit destination transition. In this placeholder,
    // the storage clear is also transfer work, so it waits too; future trace shader work
    // can run before acquire because it is outside this TRANSFER wait stage.
    const VkSemaphore waitSemaphores[] = {
        ctx.imageAvailableSemaphore,
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
    submitInfo.pCommandBuffers = &ctx.commandBuffer;
    submitInfo.signalSemaphoreCount = static_cast<uint32_t>(std::size(signalSemaphores));
    submitInfo.pSignalSemaphores = signalSemaphores;

    result = vkQueueSubmit(traceQueue, 1, &submitInfo, ctx.inFlightFence);

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

    result = vkQueuePresentKHR(presentQueue, &presentInfo);

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

} // namespace

// Program entry point and orchestration: bring up GLFW and Vulkan in dependency order,
// then run the render loop. Resources are owned by the RAII VulkanContext / Swapchain,
// so every failure path is a bare `return 1;` and cleanup happens in their destructors.
int main()
{
    std::cout << "xrPhoton booting...\n";

    // Declared first so it outlives (and is destroyed after) the Swapchain below; it
    // collects handles as they are created and tears them down on any early return.
    VulkanContext ctx;

    if (glfwInit() != GLFW_TRUE) {
        std::cerr << "Failed to initialize GLFW.\n";
        return 1;
    }

    ctx.glfwInitialized = true;

    if (glfwVulkanSupported() != GLFW_TRUE) {
        std::cerr << "GLFW reports Vulkan is not supported.\n";
        return 1;
    }

    std::cout << "Initialized GLFW with Vulkan support.\n";

    // GLFW_NO_API: Vulkan manages the surface, not GLFW's GL context. Keep the window
    // visible from creation so Wayland compositors can configure the drawable surface
    // before swapchain setup and first presentation.
    glfwWindowHint(GLFW_CLIENT_API, GLFW_NO_API);

    ctx.window = glfwCreateWindow(
        WindowWidth,
        WindowHeight,
        WindowTitle,
        nullptr,
        nullptr);

    if (ctx.window == nullptr) {
        std::cerr << "Failed to create GLFW window.\n";
        return 1;
    }

    std::cout << "Created GLFW window: "
              << WindowTitle << " ("
              << WindowWidth << 'x' << WindowHeight << ").\n";

    uint32_t instanceVersion = VK_API_VERSION_1_0;
    const VkResult result = vkEnumerateInstanceVersion(&instanceVersion);

    if (result != VK_SUCCESS) {
        std::cerr << "Failed to enumerate Vulkan instance version.\n";
        return 1;
    }

    std::cout << "Vulkan instance version: ";
    printVulkanVersion(instanceVersion);
    std::cout << '\n';

    if (instanceVersion < RequiredApiVersion) {
        std::cerr << "xrPhoton requires Vulkan 1.3 or newer.\n";
        return 1;
    }

    std::cout << "Using Vulkan API version: ";
    printVulkanVersion(RequiredApiVersion);
    std::cout << '\n';

    // Validation is best-effort, not a hard requirement: the layer only exists on
    // machines with the Vulkan SDK (or the layer package) installed, and the program is
    // equally correct without it. The debug-utils extension is tied to the same decision
    // because its only consumer is the validation messenger.
    const bool validationEnabled = ValidationRequested
        && isValidationLayerAvailable(ValidationLayerName)
        && isInstanceExtensionAvailable(VK_EXT_DEBUG_UTILS_EXTENSION_NAME);

    if (ValidationRequested && !validationEnabled) {
        std::cerr << "Vulkan validation layer is not available: " << ValidationLayerName
                  << " — continuing without validation.\n";
    }

    if (validationEnabled) {
        std::cout << "Using Vulkan validation layer: " << ValidationLayerName << '\n';
    }

    uint32_t glfwExtensionCount = 0;
    const char** glfwExtensions = glfwGetRequiredInstanceExtensions(&glfwExtensionCount);

    if (glfwExtensions == nullptr || glfwExtensionCount == 0) {
        std::cerr << "Failed to get GLFW required Vulkan instance extensions.\n";
        return 1;
    }

    // The instance extension set is GLFW's required surface extensions, plus debug-utils
    // (for the validation messenger) when validation is on. Each is verified available
    // before use; debug-utils availability was already part of the validation decision.
    std::vector<const char*> enabledExtensions(
        glfwExtensions,
        glfwExtensions + glfwExtensionCount);

    if (validationEnabled) {
        enabledExtensions.push_back(VK_EXT_DEBUG_UTILS_EXTENSION_NAME);
    }

    for (const char* enabledExtension : enabledExtensions) {
        if (!isInstanceExtensionAvailable(enabledExtension)) {
            std::cerr << "Required Vulkan instance extension is not available: "
                      << enabledExtension << '\n';
            return 1;
        }
    }

    std::cout << "Using Vulkan instance extensions:\n";
    for (const char* enabledExtension : enabledExtensions) {
        std::cout << "  " << enabledExtension << '\n';
    }

    const char* enabledLayers[] = {
        ValidationLayerName,
    };

    VkApplicationInfo applicationInfo{};
    applicationInfo.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
    applicationInfo.pApplicationName = "xrPhoton";
    applicationInfo.applicationVersion = VK_MAKE_VERSION(0, 1, 0);
    applicationInfo.pEngineName = "xrPhoton";
    applicationInfo.engineVersion = VK_MAKE_VERSION(0, 1, 0);
    applicationInfo.apiVersion = RequiredApiVersion;

    VkDebugUtilsMessengerCreateInfoEXT debugMessengerCreateInfo = makeDebugMessengerCreateInfo();

    // Chaining the debug-messenger info via pNext makes validation cover the instance's
    // own creation and destruction, before/after the standalone messenger exists.
    VkInstanceCreateInfo instanceCreateInfo{};
    instanceCreateInfo.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
    instanceCreateInfo.pNext = validationEnabled ? &debugMessengerCreateInfo : nullptr;
    instanceCreateInfo.pApplicationInfo = &applicationInfo;
    instanceCreateInfo.enabledLayerCount = validationEnabled
        ? static_cast<uint32_t>(std::size(enabledLayers))
        : 0;
    instanceCreateInfo.ppEnabledLayerNames = validationEnabled ? enabledLayers : nullptr;
    instanceCreateInfo.enabledExtensionCount = static_cast<uint32_t>(enabledExtensions.size());
    instanceCreateInfo.ppEnabledExtensionNames = enabledExtensions.data();

    const VkResult createResult = vkCreateInstance(&instanceCreateInfo, nullptr, &ctx.instance);

    if (createResult != VK_SUCCESS) {
        std::cerr << "Failed to create Vulkan instance.\n";
        return 1;
    }

    std::cout << "Created Vulkan instance.\n";

    // Without validation, ctx.debugMessenger stays null and the destructor's null guard
    // skips it.
    if (validationEnabled) {
        const VkResult debugMessengerResult = createDebugUtilsMessenger(
            ctx.instance,
            &debugMessengerCreateInfo,
            &ctx.debugMessenger);

        if (debugMessengerResult != VK_SUCCESS) {
            std::cerr << "Failed to create Vulkan debug messenger.\n";
            return 1;
        }

        std::cout << "Created Vulkan debug messenger.\n";
    }

    const VkResult surfaceResult = glfwCreateWindowSurface(
        ctx.instance,
        ctx.window,
        nullptr,
        &ctx.surface);

    if (surfaceResult != VK_SUCCESS) {
        std::cerr << "Failed to create Vulkan surface.\n";
        return 1;
    }

    std::cout << "Created Vulkan surface.\n";

    QueueFamilyIndices queueFamilies{};
    VkPhysicalDevice physicalDevice = pickPhysicalDevice(
        ctx.instance,
        ctx.surface,
        &queueFamilies);

    if (physicalDevice == VK_NULL_HANDLE) {
        return 1;
    }

    VkPhysicalDeviceProperties physicalDeviceProperties{};
    vkGetPhysicalDeviceProperties(physicalDevice, &physicalDeviceProperties);

    std::cout << "Selected Vulkan physical device: "
              << physicalDeviceProperties.deviceName << '\n';
    std::cout << "Physical device Vulkan API version: ";
    printVulkanVersion(physicalDeviceProperties.apiVersion);
    std::cout << '\n';
    std::cout << "Using trace queue family: "
              << queueFamilies.traceFamily << '\n';
    std::cout << "Using present queue family: "
              << queueFamilies.presentFamily << '\n';

    const VkResult deviceResult = createLogicalDevice(physicalDevice, queueFamilies, &ctx.device);

    if (deviceResult != VK_SUCCESS) {
        std::cerr << "Failed to create Vulkan logical device.\n";
        return 1;
    }

    std::cout << "Created Vulkan logical device with hardware ray tracing prerequisites.\n";

    RayTracingFunctions rayTracingFunctions{};

    if (!loadRayTracingFunctions(ctx.device, &rayTracingFunctions)) {
        std::cerr << "Failed to load required Vulkan ray tracing function pointers.\n";
        return 1;
    }

    std::cout << "Loaded Vulkan ray tracing function pointers.\n";

    VkQueue traceQueue = VK_NULL_HANDLE;
    vkGetDeviceQueue(ctx.device, queueFamilies.traceFamily, 0, &traceQueue);
    std::cout << "Retrieved Vulkan trace queue.\n";

    VkQueue presentQueue = VK_NULL_HANDLE;
    vkGetDeviceQueue(ctx.device, queueFamilies.presentFamily, 0, &presentQueue);
    std::cout << "Retrieved Vulkan present queue.\n";

    // Declared after ctx so it destructs first — before ctx's device/surface, which it
    // borrows but does not own.
    Swapchain swap;
    const VkResult swapchainResult = createSwapchainResources(
        &swap,
        physicalDevice,
        ctx.device,
        ctx.surface,
        ctx.window,
        queueFamilies);

    if (swapchainResult != VK_SUCCESS) {
        std::cerr << "Failed to create Vulkan swapchain.\n";
        return 1;
    }

    std::cout << "Created Vulkan swapchain with "
              << swap.images.size() << " images ("
              << swap.extent.width << 'x'
              << swap.extent.height << ").\n";

    const VkResult commandPoolResult = createCommandPool(
        ctx.device,
        queueFamilies,
        &ctx.commandPool);

    if (commandPoolResult != VK_SUCCESS) {
        std::cerr << "Failed to create Vulkan command pool.\n";
        return 1;
    }

    std::cout << "Created Vulkan command pool.\n";

    const VkResult commandBufferResult = allocateCommandBuffer(
        ctx.device,
        ctx.commandPool,
        &ctx.commandBuffer);

    if (commandBufferResult != VK_SUCCESS) {
        std::cerr << "Failed to allocate Vulkan command buffer.\n";
        return 1;
    }

    std::cout << "Allocated Vulkan command buffer.\n";

    const VkResult syncObjectsResult = createFrameSyncObjects(
        ctx.device,
        &ctx.imageAvailableSemaphore,
        &ctx.inFlightFence);

    if (syncObjectsResult != VK_SUCCESS) {
        std::cerr << "Failed to create Vulkan frame sync objects.\n";
        return 1;
    }

    std::cout << "Created Vulkan frame sync objects.\n";

    // Declared after ctx so it destructs before the device it borrows; its destructor
    // waits for device idle itself, so ordering relative to swap is immaterial. The
    // build borrows the frame command buffer and in-flight fence before the render loop
    // starts, and returns them in the state the first drawFrame expects (fence
    // signaled, command buffer resettable).
    AccelerationStructure accelerationStructure;
    const VkResult accelerationStructureResult = buildAccelerationStructures(
        &accelerationStructure,
        physicalDevice,
        ctx.device,
        rayTracingFunctions,
        ctx.commandBuffer,
        traceQueue,
        ctx.inFlightFence);

    if (accelerationStructureResult != VK_SUCCESS) {
        std::cerr << "Failed to build Vulkan acceleration structures.\n";
        return 1;
    }

    std::cout << "Built Vulkan acceleration structures (triangle BLAS, single-instance TLAS).\n";

    std::cout << "Entering GLFW event loop.\n";

    while (!glfwWindowShouldClose(ctx.window)) {
        glfwPollEvents();

        const VkResult frameResult = drawFrame(
            ctx,
            swap,
            traceQueue,
            presentQueue);

        // The surface no longer matches the swapchain (typically a resize): rebuild it
        // and skip presenting this frame.
        if (frameResult == VK_ERROR_OUT_OF_DATE_KHR
            || frameResult == VK_SUBOPTIMAL_KHR) {
            const VkResult recreateResult = recreateSwapchain(
                &swap,
                physicalDevice,
                ctx.device,
                ctx.surface,
                ctx.window,
                queueFamilies);

            if (recreateResult != VK_SUCCESS) {
                std::cerr << "Failed to recreate Vulkan swapchain.\n";
                return 1;
            }

            continue;
        }

        if (frameResult != VK_SUCCESS) {
            std::cerr << "Failed to draw Vulkan frame.\n";
            return 1;
        }

    }

    std::cout << "Exited GLFW event loop.\n";

    return 0;
}
