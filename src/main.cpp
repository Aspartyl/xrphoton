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

VkResult recordClearSwapchainImageCommandBuffer(
    VkCommandBuffer commandBuffer,
    VkImage swapchainImage)
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

    VkImageMemoryBarrier transferBarrier{};
    transferBarrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    transferBarrier.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    transferBarrier.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    transferBarrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    transferBarrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    transferBarrier.image = swapchainImage;
    transferBarrier.subresourceRange = colorRange;
    transferBarrier.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;

    vkCmdPipelineBarrier(
        commandBuffer,
        VK_PIPELINE_STAGE_TRANSFER_BIT,
        VK_PIPELINE_STAGE_TRANSFER_BIT,
        0,
        0,
        nullptr,
        0,
        nullptr,
        1,
        &transferBarrier);

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
        swapchainImage,
        VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
        &clearColor,
        1,
        &colorRange);

    VkImageMemoryBarrier presentBarrier{};
    presentBarrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    presentBarrier.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    presentBarrier.newLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
    presentBarrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    presentBarrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    presentBarrier.image = swapchainImage;
    presentBarrier.subresourceRange = colorRange;
    presentBarrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;

    vkCmdPipelineBarrier(
        commandBuffer,
        VK_PIPELINE_STAGE_TRANSFER_BIT,
        VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT,
        0,
        0,
        nullptr,
        0,
        nullptr,
        1,
        &presentBarrier);

    return vkEndCommandBuffer(commandBuffer);
}

VkResult drawFrame(
    VulkanContext& ctx,
    Swapchain& swap,
    VkQueue traceQueue,
    VkQueue presentQueue)
{
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

    const VkResult acquireResult = result;

    if (imageIndex >= swap.images.size()
        || imageIndex >= swap.renderFinishedSemaphores.size()) {
        return VK_ERROR_INITIALIZATION_FAILED;
    }

    const VkSemaphore renderFinishedSemaphore = swap.renderFinishedSemaphores[imageIndex];

    result = vkResetCommandBuffer(ctx.commandBuffer, 0);

    if (result != VK_SUCCESS) {
        return result;
    }

    result = recordClearSwapchainImageCommandBuffer(
        ctx.commandBuffer,
        swap.images[imageIndex]);

    if (result != VK_SUCCESS) {
        return result;
    }

    result = vkResetFences(ctx.device, 1, &ctx.inFlightFence);

    if (result != VK_SUCCESS) {
        return result;
    }

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

    return acquireResult;
}

} // namespace

int main()
{
    std::cout << "xrPhoton booting...\n";

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

    glfwWindowHint(GLFW_CLIENT_API, GLFW_NO_API);
    glfwWindowHint(GLFW_VISIBLE, GLFW_FALSE);

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

    if (!isValidationLayerAvailable(ValidationLayerName)) {
        std::cerr << "Required Vulkan validation layer is not available: "
                  << ValidationLayerName << '\n';
        return 1;
    }

    std::cout << "Using Vulkan validation layer: " << ValidationLayerName << '\n';

    uint32_t glfwExtensionCount = 0;
    const char** glfwExtensions = glfwGetRequiredInstanceExtensions(&glfwExtensionCount);

    if (glfwExtensions == nullptr || glfwExtensionCount == 0) {
        std::cerr << "Failed to get GLFW required Vulkan instance extensions.\n";
        return 1;
    }

    std::vector<const char*> enabledExtensions(
        glfwExtensions,
        glfwExtensions + glfwExtensionCount);
    enabledExtensions.push_back(VK_EXT_DEBUG_UTILS_EXTENSION_NAME);

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

    VkInstanceCreateInfo instanceCreateInfo{};
    instanceCreateInfo.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
    instanceCreateInfo.pNext = &debugMessengerCreateInfo;
    instanceCreateInfo.pApplicationInfo = &applicationInfo;
    instanceCreateInfo.enabledLayerCount = static_cast<uint32_t>(std::size(enabledLayers));
    instanceCreateInfo.ppEnabledLayerNames = enabledLayers;
    instanceCreateInfo.enabledExtensionCount = static_cast<uint32_t>(enabledExtensions.size());
    instanceCreateInfo.ppEnabledExtensionNames = enabledExtensions.data();

    const VkResult createResult = vkCreateInstance(&instanceCreateInfo, nullptr, &ctx.instance);

    if (createResult != VK_SUCCESS) {
        std::cerr << "Failed to create Vulkan instance.\n";
        return 1;
    }

    std::cout << "Created Vulkan instance.\n";

    const VkResult debugMessengerResult = createDebugUtilsMessenger(
        ctx.instance,
        &debugMessengerCreateInfo,
        &ctx.debugMessenger);

    if (debugMessengerResult != VK_SUCCESS) {
        std::cerr << "Failed to create Vulkan debug messenger.\n";
        return 1;
    }

    std::cout << "Created Vulkan debug messenger.\n";

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

    VkPhysicalDevice physicalDevice = pickPhysicalDevice(ctx.instance, ctx.surface);

    if (physicalDevice == VK_NULL_HANDLE) {
        return 1;
    }

    VkPhysicalDeviceProperties physicalDeviceProperties{};
    vkGetPhysicalDeviceProperties(physicalDevice, &physicalDeviceProperties);

    const QueueFamilyIndices queueFamilies = findQueueFamilies(physicalDevice, ctx.surface);
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

    // Declared after ctx so it destructs first — before ctx's device/surface.
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

    std::cout << "Entering GLFW event loop.\n";

    bool windowShown = false;

    while (!glfwWindowShouldClose(ctx.window)) {
        glfwPollEvents();

        const VkResult frameResult = drawFrame(
            ctx,
            swap,
            traceQueue,
            presentQueue);

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

        if (!windowShown) {
            glfwShowWindow(ctx.window);
            windowShown = true;
        }
    }

    std::cout << "Exited GLFW event loop.\n";

    return 0;
}
