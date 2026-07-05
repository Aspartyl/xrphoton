#include "acceleration_structure.hpp"
#include "renderer.hpp"
#include "rt_pipeline.hpp"
#include "swapchain.hpp"
#include "vulkan_context.hpp"

#include <cstdint>
#include <iostream>
#include <iterator>
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

    const VkResult commandBufferResult = allocateCommandBuffers(
        ctx.device,
        ctx.commandPool,
        &ctx.frames);

    if (commandBufferResult != VK_SUCCESS) {
        std::cerr << "Failed to allocate Vulkan per-frame command buffers.\n";
        return 1;
    }

    std::cout << "Allocated Vulkan per-frame command buffers.\n";

    const VkResult syncObjectsResult = createFrameSyncObjects(
        ctx.device,
        &ctx.frames);

    if (syncObjectsResult != VK_SUCCESS) {
        std::cerr << "Failed to create Vulkan frame sync objects.\n";
        return 1;
    }

    std::cout << "Created Vulkan frame sync objects.\n";

    // Declared after ctx so it destructs before the device it borrows; its destructor
    // waits for device idle itself, so ordering relative to swap is immaterial. The
    // build borrows frame 0's command buffer and in-flight fence before the render loop
    // starts, and returns them in the state the first drawFrame expects (fence
    // signaled, command buffer resettable).
    AccelerationStructure accelerationStructure;
    const VkResult accelerationStructureResult = buildAccelerationStructures(
        &accelerationStructure,
        physicalDevice,
        ctx.device,
        rayTracingFunctions,
        ctx.frames[0].commandBuffer,
        traceQueue,
        ctx.frames[0].inFlightFence);

    if (accelerationStructureResult != VK_SUCCESS) {
        std::cerr << "Failed to build Vulkan acceleration structures.\n";
        return 1;
    }

    std::cout << "Built Vulkan acceleration structures (triangle BLAS, single-instance TLAS).\n";

    // Declared after ctx so it destructs before the device it borrows; like the other
    // borrowing owners it waits for device idle itself, so its order relative to swap
    // and the acceleration structures is immaterial.
    RtPipeline rtPipeline;

    const VkResult descriptorSetResult = createRtDescriptorSet(&rtPipeline, ctx.device);

    if (descriptorSetResult != VK_SUCCESS) {
        std::cerr << "Failed to create Vulkan ray tracing descriptor set.\n";
        return 1;
    }

    std::cout << "Created Vulkan ray tracing descriptor set.\n";

    const VkResult rtPipelineResult = createRtPipeline(
        &rtPipeline,
        ctx.device,
        rayTracingFunctions);

    if (rtPipelineResult != VK_SUCCESS) {
        std::cerr << "Failed to create Vulkan ray tracing pipeline.\n";
        return 1;
    }

    std::cout << "Created Vulkan ray tracing pipeline.\n";

    const VkResult sbtResult = buildShaderBindingTable(
        &rtPipeline,
        physicalDevice,
        ctx.device,
        rayTracingFunctions);

    if (sbtResult != VK_SUCCESS) {
        std::cerr << "Failed to build Vulkan shader binding table.\n";
        return 1;
    }

    std::cout << "Built Vulkan shader binding table.\n";

    // The renderer's non-owning view over everything the frame path uses, created
    // last — after every handle it borrows exists. The handle members are copies of
    // program-lifetime objects; swap is a pointer because its members are replaced
    // on every recreate.
    const Renderer renderer{
        .physicalDevice = physicalDevice,
        .device = ctx.device,
        .traceQueue = traceQueue,
        .presentQueue = presentQueue,
        .frames = ctx.frames.data(),
        .tlas = accelerationStructure.tlas,
        .functions = &rayTracingFunctions,
        .rtPipeline = &rtPipeline,
        .swap = &swap,
    };

    if (!prepareRtForSwapchain(renderer)) {
        std::cerr << "Swapchain extent exceeds the device's ray dispatch limits.\n";
        return 1;
    }

    std::cout << "Wrote Vulkan ray tracing descriptor set (TLAS + storage image).\n";

    std::cout << "Entering GLFW event loop.\n";

    uint32_t currentFrame = 0;

    while (!glfwWindowShouldClose(ctx.window)) {
        glfwPollEvents();

        const VkResult frameResult = drawFrame(renderer, currentFrame);
        currentFrame = (currentFrame + 1) % MaxFramesInFlight;

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

            // The recreate rebuilt the storage image, so the RT pipeline's descriptor
            // set and dispatch-limit check must run again before the next trace.
            if (!prepareRtForSwapchain(renderer)) {
                std::cerr << "Swapchain extent exceeds the device's ray dispatch limits.\n";
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
