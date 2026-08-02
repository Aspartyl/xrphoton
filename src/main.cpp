#include "acceleration_structure.hpp"
#include "camera.hpp"
#include "capture.hpp"
#include "gallery.hpp"
#include "gpu_lighting.hpp"
#include "gpu_scene.hpp"
#include "lighting.hpp"
#include "physics.hpp"
#include "player.hpp"
#include "renderer.hpp"
#include "rt_pipeline.hpp"
#include "scene.hpp"
#include "scene_lighting.hpp"
#include "swapchain.hpp"
#include "tonemap_pipeline.hpp"
#include "vulkan_context.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <iomanip>
#include <iostream>
#include <iterator>
#include <limits>
#include <string>
#include <utility>
#include <vector>

#include <vulkan/vulkan.h>
#include <GLFW/glfw3.h>

using namespace xrphoton;

namespace
{
constexpr int WindowWidth = 1920;
constexpr int WindowHeight = 1080;
constexpr const char* WindowTitle = "xrPhoton";
constexpr float InteractiveDayCycleHoursPerSecond = 1.0f / 60.0f;
bool framebufferResized = false;

void markFramebufferResized(GLFWwindow*, int, int)
{
    framebufferResized = true;
}

bool packPublishedFrameLighting(
    const SceneLighting& sceneLighting,
    std::uint32_t instanceCount,
    bool referenceMode,
    EstimatorMode estimator,
    FrameLighting* frameLighting)
{
    if (!makeFrameLighting(sceneLighting, instanceCount, frameLighting)) {
        return false;
    }
    // Glass transport is a permanent pipeline capability. Keep the published feature
    // bit stable for capture/debug consumers without deriving renderer behavior from
    // the current scene's material inventory.
    frameLighting->flags |= FrameLightingGlassBit;
    if (referenceMode) {
        frameLighting->flags =
            (frameLighting->flags & ~FrameLightingEstimatorMask)
            | estimatorFlags(estimator);
    }
    return hasValidFrameLightingFlags(frameLighting->flags);
}

} // namespace

// Program entry point and orchestration: bring up GLFW and Vulkan in dependency order,
// then run the render loop. Resources are owned by the RAII VulkanContext / Swapchain,
// so every failure path is a bare `return 1;` and cleanup happens in their destructors.
int main(int argumentCount, char** arguments)
{
    CommandLineOptions commandLine;
    std::string commandLineError;
    if (!parseCommandLine(
            argumentCount,
            arguments,
            &commandLine,
            &commandLineError)) {
        std::cerr << commandLineError << '\n';
        return 1;
    }
    const bool captureMode = commandLine.mode == CommandLineMode::Capture;
    const bool referenceMode = commandLine.mode == CommandLineMode::Reference;
    const bool offlineMode = captureMode || referenceMode;

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
        std::cerr << "Failed to enumerate Vulkan instance version: "
                  << formatVkResult(result) << ".\n";
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
    const bool validationEnabled = commandLine.validationRequested
        && isValidationLayerAvailable(ValidationLayerName)
        && isInstanceExtensionAvailable(VK_EXT_DEBUG_UTILS_EXTENSION_NAME);

    if (commandLine.validationRequested && !validationEnabled) {
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
        std::cerr << "Failed to create Vulkan instance: "
                  << formatVkResult(createResult) << ".\n";
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
            std::cerr << "Failed to create Vulkan debug messenger: "
                      << formatVkResult(debugMessengerResult) << ".\n";
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
        std::cerr << "Failed to create Vulkan surface: "
                  << formatVkResult(surfaceResult) << ".\n";
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
    const bool traceTimestampsSupported =
        queueFamilies.traceTimestampValidBits != 0
        && physicalDeviceProperties.limits.timestampPeriod > 0.0f;

    if (offlineMode && !traceTimestampsSupported) {
        std::cerr << "Capture/reference requires timestamp support on the selected Vulkan "
                     "trace queue.\n";
        return 1;
    }

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
        std::cerr << "Failed to create Vulkan logical device: "
                  << formatVkResult(deviceResult) << ".\n";
        return 1;
    }

    std::cout << "Created Vulkan logical device with renderer feature prerequisites.\n";

    const VkResult allocatorResult = createAllocator(
        ctx.instance,
        physicalDevice,
        ctx.device,
        &ctx.allocator);

    if (allocatorResult != VK_SUCCESS) {
        std::cerr << "Failed to create Vulkan memory allocator: "
                  << formatVkResult(allocatorResult) << ".\n";
        return 1;
    }

    std::cout << "Created Vulkan memory allocator.\n";

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
        ctx.allocator,
        ctx.surface,
        ctx.window,
        queueFamilies);

    if (swapchainResult != VK_SUCCESS) {
        std::cerr << "Failed to create Vulkan swapchain: "
                  << formatVkResult(swapchainResult) << ".\n";
        return 1;
    }

    std::cout << "Created Vulkan swapchain with "
              << swap.images.size() << " images ("
              << swap.extent.width << 'x'
              << swap.extent.height << ").\n";

    // Do not rely solely on acquire/present to report a stale Wayland swapchain:
    // some compositor/driver pairs keep accepting and scaling it after a resize.
    framebufferResized = false;
    glfwSetFramebufferSizeCallback(ctx.window, markFramebufferResized);

    const VkResult commandPoolResult = createCommandPool(
        ctx.device,
        queueFamilies,
        &ctx.commandPool);

    if (commandPoolResult != VK_SUCCESS) {
        std::cerr << "Failed to create Vulkan command pool: "
                  << formatVkResult(commandPoolResult) << ".\n";
        return 1;
    }

    std::cout << "Created Vulkan command pool.\n";

    const VkResult commandBufferResult = allocateCommandBuffers(
        ctx.device,
        ctx.commandPool,
        &ctx.frames);

    if (commandBufferResult != VK_SUCCESS) {
        std::cerr << "Failed to allocate Vulkan per-frame command buffers: "
                  << formatVkResult(commandBufferResult) << ".\n";
        return 1;
    }

    std::cout << "Allocated Vulkan per-frame command buffers.\n";

    const VkResult syncObjectsResult = createFrameSyncObjects(
        ctx.device,
        traceTimestampsSupported,
        &ctx.frames);

    if (syncObjectsResult != VK_SUCCESS) {
        std::cerr << "Failed to create Vulkan frame sync objects: "
                  << formatVkResult(syncObjectsResult) << ".\n";
        return 1;
    }

    std::cout << "Created Vulkan frame sync objects";
    if (traceTimestampsSupported) {
        std::cout << " and trace timing objects";
    }
    std::cout << ".\n";

    GalleryLoadResult loadedGallery = loadGalleryScene(
        referenceMode
            ? GallerySceneProfile::EstimatorReference
            : GallerySceneProfile::Complete);
    if (!loadedGallery) {
        std::cerr << loadedGallery.error << '\n';
        return 1;
    }

    SceneData sceneData = std::move(loadedGallery.scene);
    if (sceneData.instances.size() > std::numeric_limits<std::uint32_t>::max()) {
        std::cerr << "Scene instance count exceeds the FrameLighting ABI.\n";
        return 1;
    }
    float timeOfDayHours = commandLine.timeOfDayHours.value_or(
        DefaultTimeOfDayHours);
    SceneLighting sceneLighting = DefaultSceneLighting;
    if (!updateSceneLightingTimeOfDay(timeOfDayHours, &sceneLighting)) {
        std::cerr << "Failed to configure scene time of day.\n";
        return 1;
    }
    std::string sceneLightingError;
    if (!buildSceneLighting(
            sceneData,
            loadedGallery.dynamicInstances,
            &sceneLighting,
            &sceneLightingError)) {
        std::cerr << "Failed to build scene lighting: "
                  << sceneLightingError << '\n';
        return 1;
    }
    FrameLighting frameLighting;
    if (!packPublishedFrameLighting(
            sceneLighting,
            static_cast<std::uint32_t>(sceneData.instances.size()),
            referenceMode,
            commandLine.estimator,
            &frameLighting)) {
        std::cerr << "Failed to pack scene lighting.\n";
        return 1;
    }

    // Declared after the borrowed scene so reverse destruction tears physics down
    // first. Physics owns dynamic transform writes but no Vulkan state.
    PhysicsWorld physicsWorld;
    if (!createPhysicsWorld(
            &physicsWorld,
            &sceneData,
            loadedGallery.dynamicInstances)) {
        std::cerr << "Failed to create physics world.\n";
        return 1;
    }

    std::cout << "Created physics world (dynamic bodies: "
              << loadedGallery.dynamicInstances.size() << ").\n";

    const std::array<float, 3> characterSpawn{
        loadedGallery.spawn.position.x,
        loadedGallery.spawn.position.y - PlayerEyeHeight,
        loadedGallery.spawn.position.z,
    };
    if (!createPhysicsCharacter(&physicsWorld, characterSpawn)) {
        std::cerr << "Failed to create player character.\n";
        return 1;
    }
    std::cout << "Created capsule player character.\n";

    GpuScene gpuScene;
    const VkResult gpuSceneResult = createGpuScene(
        &gpuScene,
        sceneData,
        physicalDevice,
        ctx.device,
        ctx.allocator,
        rayTracingFunctions,
        ctx.frames[0].commandBuffer,
        traceQueue,
        ctx.frames[0].inFlightFence);

    if (gpuSceneResult != VK_SUCCESS) {
        std::cerr << "Failed to create Vulkan GPU scene: "
                  << formatVkResult(gpuSceneResult) << ".\n";
        return 1;
    }

    std::cout << "Created Vulkan GPU scene (meshes: " << sceneData.meshes.size()
              << ", geometries: " << sceneData.geometries.size()
              << ", materials: " << sceneData.materials.size() << ").\n";

    // Declared before the ray tracing pipeline so bindings 5-8 never outlive their
    // buffers. Each frame slot is written only after its fence wait.
    GpuLighting gpuLighting;
    const VkResult gpuLightingResult = createGpuLighting(
        &gpuLighting,
        physicalDevice,
        ctx.device,
        ctx.allocator,
        MaxFramesInFlight,
        sceneLighting,
        ctx.frames[0].commandBuffer,
        traceQueue,
        ctx.frames[0].inFlightFence);
    if (gpuLightingResult != VK_SUCCESS) {
        std::cerr << "Failed to create Vulkan GPU lighting: "
                  << formatVkResult(gpuLightingResult) << ".\n";
        return 1;
    }
    std::cout << "Created Vulkan lighting buffers (emitting triangles: "
              << sceneLighting.lights.size() << ").\n";

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
        ctx.allocator,
        rayTracingFunctions,
        sceneData,
        gpuScene,
        MaxFramesInFlight,
        ctx.frames[0].commandBuffer,
        traceQueue,
        ctx.frames[0].inFlightFence);

    if (accelerationStructureResult != VK_SUCCESS) {
        std::cerr << "Failed to build Vulkan acceleration structures: "
                  << formatVkResult(accelerationStructureResult) << ".\n";
        return 1;
    }

    std::cout << "Built Vulkan acceleration structures (BLASes: "
              << sceneData.meshes.size() << ", TLAS instances: "
              << sceneData.instances.size() << ").\n";

    // Declared after ctx so it destructs before the device it borrows; like the other
    // borrowing owners it waits for device idle itself, so its order relative to swap
    // and the acceleration structures is immaterial.
    RtPipeline rtPipeline;

    const VkResult descriptorSetResult = createRtDescriptorSet(&rtPipeline, ctx.device);

    if (descriptorSetResult != VK_SUCCESS) {
        std::cerr << "Failed to create Vulkan ray tracing descriptor set: "
                  << formatVkResult(descriptorSetResult) << ".\n";
        return 1;
    }

    std::cout << "Created Vulkan ray tracing descriptor set.\n";

    const VkResult rtPipelineResult = createRtPipeline(
        &rtPipeline,
        ctx.device,
        rayTracingFunctions);

    if (rtPipelineResult != VK_SUCCESS) {
        std::cerr << "Failed to create Vulkan ray tracing pipeline: "
                  << formatVkResult(rtPipelineResult) << ".\n";
        return 1;
    }

    std::cout << "Created Vulkan ray tracing pipeline.\n";

    const VkResult sbtResult = buildShaderBindingTable(
        &rtPipeline,
        physicalDevice,
        ctx.device,
        ctx.allocator,
        rayTracingFunctions,
        sceneData);

    if (sbtResult != VK_SUCCESS) {
        std::cerr << "Failed to build Vulkan shader binding table: "
                  << formatVkResult(sbtResult) << ".\n";
        return 1;
    }

    std::cout << "Built Vulkan shader binding table.\n";

    writeSceneDescriptorSet(ctx.device, rtPipeline.descriptorSet, gpuScene);
    writeLightingDescriptorSet(ctx.device, rtPipeline.descriptorSet, gpuLighting);
    std::cout << "Wrote Vulkan scene descriptor bindings.\n";

    TonemapPipeline tonemapPipeline;
    const VkResult tonemapPipelineResult = createTonemapPipeline(
        &tonemapPipeline,
        ctx.device);
    if (tonemapPipelineResult != VK_SUCCESS) {
        std::cerr << "Failed to create Vulkan tonemap pipeline: "
                  << formatVkResult(tonemapPipelineResult) << ".\n";
        return 1;
    }
    std::cout << "Created Vulkan HDR tonemap pipeline.\n";

    // The renderer's non-owning view over everything the frame path uses, created
    // last — after every handle it borrows exists. The handle members are copies of
    // program-lifetime objects; swap is a pointer because its members are replaced
    // on every recreate.
    const Renderer renderer{
        .physicalDevice = physicalDevice,
        .device = ctx.device,
        .allocator = ctx.allocator,
        .traceQueue = traceQueue,
        .presentQueue = presentQueue,
        .traceTimestampPeriod = physicalDeviceProperties.limits.timestampPeriod,
        .traceTimestampValidBits = queueFamilies.traceTimestampValidBits,
        .frames = ctx.frames.data(),
        .gpuLighting = &gpuLighting,
        .accel = &accelerationStructure,
        .scene = &sceneData,
        .functions = &rayTracingFunctions,
        .rtPipeline = &rtPipeline,
        .tonemapPipeline = &tonemapPipeline,
        .swap = &swap,
    };

    if (!prepareRtForSwapchain(renderer)) {
        std::cerr << "Swapchain extent exceeds the device's trace/tonemap dispatch limits.\n";
        return 1;
    }

    std::cout << "Wrote Vulkan render descriptors (TLAS + HDR/LDR images).\n";

    Camera playerCamera{
        .position = loadedGallery.spawn.position,
        .yaw = loadedGallery.spawn.yaw,
        .pitch = loadedGallery.spawn.pitch,
    };
    const Camera captureCamera{
        .position = loadedGallery.spawn.position,
        .yaw = loadedGallery.spawn.yaw,
        .pitch = loadedGallery.spawn.pitch,
    };
    Camera freeCamera = playerCamera;
    CameraControls cameraControls;
    CameraMode cameraMode = CameraMode::Player;
    double lastTime = glfwGetTime();
    uint32_t currentFrame = 0;
    uint32_t frameCounter = 0;
    if (referenceMode) {
        if (!setPhysicsCharacterEnabled(&physicsWorld, false)) {
            std::cerr << "Failed to disable the player character for reference mode.\n";
            return 1;
        }

        const VkExtent2D referenceExtent = swap.extent;
        std::cout << "Reference start: extent="
                  << referenceExtent.width << 'x' << referenceExtent.height
                  << " requestedSamples=" << commandLine.referenceSampleCount
                  << " time=" << timeOfDayHours
                  << " estimator=" << estimatorModeName(commandLine.estimator)
                  << '\n';
        ReferenceAccumulator accumulator;
        std::array<double, CaptureTraceTimingCapacity> traceTimings{};
        std::size_t traceTimingCount = 0;

        for (std::uint32_t sampleIndex = 0;
             sampleIndex < commandLine.referenceSampleCount;
             ++sampleIndex) {
            glfwPollEvents();
            if (glfwWindowShouldClose(ctx.window)) {
                std::cerr << "Reference incomplete: window closed after "
                          << sampleIndex << " samples.\n";
                return 1;
            }
            int framebufferWidth = 0;
            int framebufferHeight = 0;
            glfwGetFramebufferSize(
                ctx.window,
                &framebufferWidth,
                &framebufferHeight);
            if (framebufferResized
                || framebufferWidth != static_cast<int>(referenceExtent.width)
                || framebufferHeight != static_cast<int>(referenceExtent.height)) {
                std::cerr << "Reference failed: fixed framebuffer extent changed.\n";
                return 1;
            }

            const float aspect = static_cast<float>(referenceExtent.width)
                / static_cast<float>(referenceExtent.height);
            const std::uint32_t submittedSlot = currentFrame;
            const VkResult frameResult = drawFrame(
                renderer,
                submittedSlot,
                makeRaygenPushConstants(
                    makeCameraPushConstants(captureCamera, aspect),
                    frameCounter),
                frameLighting);
            if (frameResult != VK_SUCCESS) {
                std::cerr << "Failed to draw Vulkan reference sample: "
                          << formatVkResult(frameResult) << ".\n";
                return 1;
            }

            if (traceTimingCount < traceTimings.size()) {
                double traceMilliseconds = 0.0;
                const VkResult timingResult = readTraceTimestampMilliseconds(
                    renderer,
                    submittedSlot,
                    &traceMilliseconds);
                if (timingResult != VK_SUCCESS) {
                    std::cerr << "Failed to read reference trace timestamps: "
                              << formatVkResult(timingResult) << ".\n";
                    return 1;
                }
                traceTimings[traceTimingCount++] = traceMilliseconds;
            }

            HdrImageReadback readback;
            const VkResult readbackResult = readbackHdrImage(
                renderer,
                submittedSlot,
                &readback);
            if (readbackResult != VK_SUCCESS) {
                std::cerr << "Failed to read back Vulkan HDR reference image: "
                          << formatVkResult(readbackResult) << ".\n";
                return 1;
            }
            if (!accumulateReferenceImage(
                    readback.width,
                    readback.height,
                    readback.rgba16,
                    &accumulator)) {
                std::cerr << "Failed to accumulate HDR reference sample.\n";
                return 1;
            }
            ++frameCounter;
            currentFrame = (currentFrame + 1) % MaxFramesInFlight;
        }

        CaptureTraceTimingSummary timingSummary;
        std::array<ReferenceRegionSummary, ReferenceRegionCount> summaries{};
        if (!summarizeCaptureTraceTimings(
                std::span(traceTimings.data(), traceTimingCount),
                &timingSummary)
            || !summarizeReferenceRegions(accumulator, &summaries)) {
            std::cerr << "Failed to summarize reference results.\n";
            return 1;
        }
        for (std::size_t region = 0; region < summaries.size(); ++region) {
            const ReferenceRegionSummary& summary = summaries[region];
            std::cout << "Reference region=" << referenceRegionName(region)
                      << " mean=" << summary.mean[0] << ',' << summary.mean[1]
                      << ',' << summary.mean[2]
                      << " standardError=" << summary.standardError[0] << ','
                      << summary.standardError[1] << ','
                      << summary.standardError[2] << '\n';
        }
        std::cout << "Reference complete: extent="
                  << referenceExtent.width << 'x' << referenceExtent.height
                  << " samples=" << accumulator.sampleCount
                  << " time=" << timeOfDayHours
                  << " estimator=" << estimatorModeName(commandLine.estimator)
                  << " traceMedianMs=" << std::fixed << std::setprecision(3)
                  << timingSummary.medianMilliseconds
                  << " traceSamples=" << timingSummary.sampleCount
                  << " traceTiming="
                  << (timingSummary.comparable ? "comparable" : "diagnostic")
                  << std::defaultfloat << std::setprecision(6) << '\n';
        return 0;
    }
    if (captureMode) {
        if (!setPhysicsCharacterEnabled(&physicsWorld, false)) {
            std::cerr << "Failed to disable the player character for capture.\n";
            return 1;
        }

        const VkExtent2D captureExtent = swap.extent;
        std::cout << "Capture start: extent="
                  << captureExtent.width << 'x' << captureExtent.height
                  << " requestedFrames=" << commandLine.captureFrameCount
                  << " time=" << timeOfDayHours
                  << '\n';

        std::uint32_t successfulFrameCount = 0;
        std::uint32_t lastSubmittedSlot = 0;
        std::uint32_t lastRenderedFrameIndex = 0;
        std::array<double, CaptureTraceTimingCapacity> traceTimings{};
        std::size_t traceTimingCount = 0;

        while (successfulFrameCount < commandLine.captureFrameCount) {
            glfwPollEvents();

            if (glfwWindowShouldClose(ctx.window)) {
                std::cerr << "Capture incomplete: window closed after "
                          << successfulFrameCount << " of "
                          << commandLine.captureFrameCount
                          << " successful frames.\n";
                return 1;
            }

            int framebufferWidth = 0;
            int framebufferHeight = 0;
            glfwGetFramebufferSize(
                ctx.window,
                &framebufferWidth,
                &framebufferHeight);
            if (framebufferResized
                || swap.extent.width != captureExtent.width
                || swap.extent.height != captureExtent.height
                || framebufferWidth <= 0
                || framebufferHeight <= 0
                || static_cast<std::uint32_t>(framebufferWidth)
                    != captureExtent.width
                || static_cast<std::uint32_t>(framebufferHeight)
                    != captureExtent.height) {
                std::cerr << "Capture failed: framebuffer extent changed from "
                          << captureExtent.width << 'x' << captureExtent.height
                          << "; deterministic capture does not recreate the "
                             "swapchain.\n";
                return 1;
            }

            if (!stepPhysics(&physicsWorld, PhysicsFixedDt)) {
                std::cerr << "Failed to advance physics world for capture.\n";
                return 1;
            }

            const float aspect = static_cast<float>(captureExtent.width)
                / static_cast<float>(captureExtent.height);
            const std::uint32_t submittedSlot = currentFrame;
            const std::uint32_t submittedFrameIndex = frameCounter;
            const VkResult frameResult = drawFrame(
                renderer,
                submittedSlot,
                makeRaygenPushConstants(
                    makeCameraPushConstants(captureCamera, aspect),
                    submittedFrameIndex),
                frameLighting);

            if (frameResult == VK_ERROR_OUT_OF_DATE_KHR
                || frameResult == VK_SUBOPTIMAL_KHR) {
                std::cerr << "Capture failed: fixed swapchain became "
                          << (frameResult == VK_ERROR_OUT_OF_DATE_KHR
                                  ? "out of date"
                                  : "suboptimal")
                          << " after " << successfulFrameCount
                          << " successful frames.\n";
                return 1;
            }
            if (frameResult != VK_SUCCESS) {
                std::cerr << "Failed to draw Vulkan capture frame: "
                          << formatVkResult(frameResult) << ".\n";
                return 1;
            }

            if (traceTimingCount < traceTimings.size()) {
                double traceMilliseconds = 0.0;
                const VkResult timingResult = readTraceTimestampMilliseconds(
                    renderer,
                    submittedSlot,
                    &traceMilliseconds);
                if (timingResult != VK_SUCCESS) {
                    std::cerr << "Failed to read Vulkan trace timestamps: "
                              << formatVkResult(timingResult) << ".\n";
                    return 1;
                }
                traceTimings[traceTimingCount] = traceMilliseconds;
                ++traceTimingCount;
            }

            lastSubmittedSlot = submittedSlot;
            lastRenderedFrameIndex = submittedFrameIndex;
            ++successfulFrameCount;
            ++frameCounter;
            currentFrame = (currentFrame + 1) % MaxFramesInFlight;
        }

        CaptureTraceTimingSummary timingSummary;
        if (!summarizeCaptureTraceTimings(
                std::span(traceTimings.data(), traceTimingCount),
                &timingSummary)) {
            std::cerr << "Failed to summarize Vulkan trace timestamps.\n";
            return 1;
        }

        StorageImageReadback readback;
        const VkResult readbackResult = readbackStorageImage(
            renderer,
            lastSubmittedSlot,
            &readback);
        if (readbackResult != VK_SUCCESS) {
            std::cerr << "Failed to read back Vulkan LDR output image: "
                      << formatVkResult(readbackResult) << ".\n";
            return 1;
        }

        std::uint64_t hash = 0;
        if (!hashCaptureImage(
                readback.width,
                readback.height,
                readback.rgba8,
                &hash)) {
            std::cerr << "Failed to hash captured LDR bytes.\n";
            return 1;
        }

        std::string publicationError;
        if (!writeCapturePpm(
                commandLine.captureOutputPath,
                readback.width,
                readback.height,
                readback.rgba8,
                &publicationError)) {
            std::cerr << "Failed to publish capture PPM '"
                      << commandLine.captureOutputPath << "': "
                      << publicationError << '\n';
            return 1;
        }

        std::cout << "Capture complete: extent="
                  << readback.width << 'x' << readback.height
                  << " successfulFrames=" << successfulFrameCount
                  << " frameIndex=" << lastRenderedFrameIndex
                  << " hash=0x"
                  << std::hex << std::nouppercase
                  << std::setw(16) << std::setfill('0') << hash
                  << std::dec << std::setfill(' ')
                  << " traceMedianMs=" << std::fixed << std::setprecision(3)
                  << timingSummary.medianMilliseconds
                  << " traceSamples=" << timingSummary.sampleCount
                  << " traceTiming="
                  << (timingSummary.comparable ? "comparable" : "diagnostic")
                  << std::defaultfloat << std::setprecision(6) << '\n';
        return 0;
    }

    std::cout << "Player: WASD run, Left Shift sprint, Left Ctrl crouch, Space jump.\n"
                 "Free camera: WASD move, Left Shift boost, Space/Ctrl up/down.\n"
                 "Shared: F1 switch view, Escape release mouse, left click recapture.\n";
    std::cout << "Entering GLFW event loop in player mode.\n";

    glfwSetInputMode(ctx.window, GLFW_CURSOR, GLFW_CURSOR_DISABLED);
    if (glfwRawMouseMotionSupported() == GLFW_TRUE) {
        glfwSetInputMode(ctx.window, GLFW_RAW_MOUSE_MOTION, GLFW_TRUE);
    }

    while (!glfwWindowShouldClose(ctx.window)) {
        glfwPollEvents();

        const double now = glfwGetTime();
        const float dt = static_cast<float>(std::min(
            now - lastTime,
            static_cast<double>(PhysicsMaxFrameDt)));
        lastTime = now;

        Camera* controlledCamera = cameraMode == CameraMode::Player
            ? &playerCamera
            : &freeCamera;
        CameraUpdate cameraUpdate = updateCamera(
            controlledCamera,
            &cameraControls,
            ctx.window,
            dt,
            cameraMode);
        if (cameraUpdate.toggleMode) {
            cameraMode = toggledCameraMode(cameraMode);
            if (cameraMode == CameraMode::Free) {
                placeFreeCameraAtPlayerView(playerCamera, &freeCamera);
            }
            Camera& nextCamera = cameraMode == CameraMode::Player
                ? playerCamera
                : freeCamera;
            nextCamera.cursorAnchorValid = false;
            cameraUpdate.playerVelocity = {};
            cameraUpdate.jumpRequested = false;
            cameraUpdate.crouched = false;
            std::cout << (cameraMode == CameraMode::Player
                    ? "Switched to player camera.\n"
                    : "Switched to collision-free camera.\n");
        }

        const std::array<float, 2> characterVelocity =
            cameraMode == CameraMode::Player
            ? std::array<float, 2>{
                cameraUpdate.playerVelocity.x,
                cameraUpdate.playerVelocity.z,
            }
            : std::array<float, 2>{0.0f, 0.0f};
        if (!setPhysicsCharacterEnabled(
                &physicsWorld,
                cameraMode == CameraMode::Player)
            || !setPhysicsCharacterInput(
                &physicsWorld,
                characterVelocity,
                cameraMode == CameraMode::Player
                    && cameraUpdate.jumpRequested,
                cameraMode == CameraMode::Player
                    && cameraUpdate.crouched)) {
            std::cerr << "Failed to update player-character input.\n";
            return 1;
        }

        if (!stepPhysics(&physicsWorld, dt)) {
            std::cerr << "Failed to advance physics world.\n";
            return 1;
        }

        std::array<float, 3> characterPosition{};
        bool characterCrouched = false;
        if (!queryPhysicsCharacterPosition(
                &physicsWorld,
                &characterPosition)
            || !queryPhysicsCharacterCrouched(
                &physicsWorld,
                &characterCrouched)) {
            std::cerr << "Failed to query player-character state.\n";
            return 1;
        }
        playerCamera.position = {
            characterPosition[0],
            characterPosition[1] + (characterCrouched
                ? PlayerCrouchEyeHeight
                : PlayerEyeHeight),
            characterPosition[2],
        };

        timeOfDayHours = std::fmod(
            timeOfDayHours + dt * InteractiveDayCycleHoursPerSecond,
            24.0f);
        if (!updateSceneLightingTimeOfDay(timeOfDayHours, &sceneLighting)
            || !packPublishedFrameLighting(
                sceneLighting,
                static_cast<std::uint32_t>(sceneData.instances.size()),
                false,
                EstimatorMode::Mis,
                &frameLighting)) {
            std::cerr << "Failed to update time-varying scene lighting.\n";
            return 1;
        }

        const float aspect = static_cast<float>(swap.extent.width)
            / static_cast<float>(swap.extent.height);

        VkResult frameResult = VK_ERROR_OUT_OF_DATE_KHR;
        if (!framebufferResized) {
            const Camera& renderCamera = cameraMode == CameraMode::Player
                ? playerCamera
                : freeCamera;
            frameResult = drawFrame(
                renderer,
                currentFrame,
                makeRaygenPushConstants(
                    makeCameraPushConstants(renderCamera, aspect),
                    frameCounter),
                frameLighting);
            currentFrame = (currentFrame + 1) % MaxFramesInFlight;
            ++frameCounter;
        }

        // The surface no longer matches the swapchain (typically a resize): rebuild it
        // and skip presenting this frame.
        if (frameResult == VK_ERROR_OUT_OF_DATE_KHR
            || frameResult == VK_SUBOPTIMAL_KHR) {
            const VkResult recreateResult = recreateSwapchain(
                &swap,
                physicalDevice,
                ctx.device,
                ctx.allocator,
                ctx.surface,
                ctx.window,
                queueFamilies);

            if (recreateResult != VK_SUCCESS) {
                std::cerr << "Failed to recreate Vulkan swapchain: "
                          << formatVkResult(recreateResult) << ".\n";
                return 1;
            }

            // The recreate rebuilt both render targets, so both descriptor sets and
            // dispatch-limit checks must run again before the next frame.
            if (!prepareRtForSwapchain(renderer)) {
                std::cerr << "Swapchain extent exceeds the device's trace/tonemap dispatch limits.\n";
                return 1;
            }

            framebufferResized = false;
            continue;
        }

        if (frameResult != VK_SUCCESS) {
            std::cerr << "Failed to draw Vulkan frame: "
                      << formatVkResult(frameResult) << ".\n";
            return 1;
        }

    }

    std::cout << "Exited GLFW event loop.\n";

    return 0;
}
