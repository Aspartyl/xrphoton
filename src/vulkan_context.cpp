#include "vulkan_context.hpp"

#include "swapchain.hpp"

#include <cstring>
#include <iostream>
#include <iterator>
#include <vector>

namespace xrphoton
{
namespace
{
// Device extensions every selected GPU must support and that the logical device
// enables. The first five form the hardware ray tracing stack (acceleration
// structures, the RT pipeline, and their prerequisites: buffer device address,
// deferred host operations, pipeline libraries); the last is presentation.
constexpr const char* RequiredDeviceExtensions[] = {
    VK_KHR_ACCELERATION_STRUCTURE_EXTENSION_NAME,
    VK_KHR_RAY_TRACING_PIPELINE_EXTENSION_NAME,
    VK_KHR_BUFFER_DEVICE_ADDRESS_EXTENSION_NAME,
    VK_KHR_DEFERRED_HOST_OPERATIONS_EXTENSION_NAME,
    VK_KHR_PIPELINE_LIBRARY_EXTENSION_NAME,
    VK_KHR_SWAPCHAIN_EXTENSION_NAME,
};

// Sink for validation-layer messages. Returning VK_FALSE tells Vulkan not to abort the
// triggering call (the convention for non-fatal reporting). Severity/type are unused;
// makeDebugMessengerCreateInfo already filters to warnings and errors.
VKAPI_ATTR VkBool32 VKAPI_CALL debugMessengerCallback(
    VkDebugUtilsMessageSeverityFlagBitsEXT messageSeverity,
    VkDebugUtilsMessageTypeFlagsEXT messageType,
    const VkDebugUtilsMessengerCallbackDataEXT* callbackData,
    void* userData)
{
    (void)messageSeverity;
    (void)messageType;
    (void)userData;

    std::cerr << "Vulkan validation: " << callbackData->pMessage << '\n';
    return VK_FALSE;
}

// True only if every entry in RequiredDeviceExtensions is advertised by the device.
bool areRequiredDeviceExtensionsAvailable(VkPhysicalDevice physicalDevice)
{
    uint32_t extensionCount = 0;
    VkResult result = vkEnumerateDeviceExtensionProperties(physicalDevice, nullptr, &extensionCount, nullptr);

    if (result != VK_SUCCESS) {
        return false;
    }

    std::vector<VkExtensionProperties> availableExtensions(extensionCount);
    result = vkEnumerateDeviceExtensionProperties(
        physicalDevice,
        nullptr,
        &extensionCount,
        availableExtensions.data());

    if (result != VK_SUCCESS) {
        return false;
    }

    for (const char* requiredExtension : RequiredDeviceExtensions) {
        bool found = false;

        for (const VkExtensionProperties& availableExtension : availableExtensions) {
            if (std::strcmp(availableExtension.extensionName, requiredExtension) == 0) {
                found = true;
                break;
            }
        }

        if (!found) {
            return false;
        }
    }

    return true;
}

bool hasRequiredApiVersion(VkPhysicalDevice physicalDevice)
{
    VkPhysicalDeviceProperties properties{};
    vkGetPhysicalDeviceProperties(physicalDevice, &properties);

    return properties.apiVersion >= RequiredApiVersion;
}

// Query the ray tracing feature chain and confirm the device actually enables the
// three capabilities the renderer depends on. The structs are linked through pNext so a
// single vkGetPhysicalDeviceFeatures2 call fills them all; createLogicalDevice later
// re-uses the same chain shape to turn the features on.
bool areRequiredRayTracingFeaturesAvailable(VkPhysicalDevice physicalDevice)
{
    VkPhysicalDeviceBufferDeviceAddressFeatures bufferDeviceAddressFeatures{};
    bufferDeviceAddressFeatures.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_BUFFER_DEVICE_ADDRESS_FEATURES;

    VkPhysicalDeviceRayTracingPipelineFeaturesKHR rayTracingPipelineFeatures{};
    rayTracingPipelineFeatures.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_RAY_TRACING_PIPELINE_FEATURES_KHR;
    rayTracingPipelineFeatures.pNext = &bufferDeviceAddressFeatures;

    VkPhysicalDeviceAccelerationStructureFeaturesKHR accelerationStructureFeatures{};
    accelerationStructureFeatures.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ACCELERATION_STRUCTURE_FEATURES_KHR;
    accelerationStructureFeatures.pNext = &rayTracingPipelineFeatures;

    VkPhysicalDeviceFeatures2 physicalDeviceFeatures{};
    physicalDeviceFeatures.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2;
    physicalDeviceFeatures.pNext = &accelerationStructureFeatures;

    vkGetPhysicalDeviceFeatures2(physicalDevice, &physicalDeviceFeatures);

    return bufferDeviceAddressFeatures.bufferDeviceAddress == VK_TRUE
        && accelerationStructureFeatures.accelerationStructure == VK_TRUE
        && rayTracingPipelineFeatures.rayTracingPipeline == VK_TRUE;
}

// Locate the queue families the renderer needs. The trace family must support both
// compute and graphics because tracing and the present blit are recorded into one
// command buffer. A family that can also present is preferred: when trace and present
// coincide, the swapchain uses EXCLUSIVE sharing (see createSwapchain), so the combined
// family should be the outcome by construction, not by luck of enumeration order. Only
// when no family covers both roles does the scan fall back to independent first
// matches. Call isComplete() on the result to check both roles were found.
QueueFamilyIndices findQueueFamilies(VkPhysicalDevice physicalDevice, VkSurfaceKHR surface)
{
    uint32_t queueFamilyCount = 0;
    vkGetPhysicalDeviceQueueFamilyProperties(physicalDevice, &queueFamilyCount, nullptr);

    std::vector<VkQueueFamilyProperties> queueFamilies(queueFamilyCount);
    vkGetPhysicalDeviceQueueFamilyProperties(physicalDevice, &queueFamilyCount, queueFamilies.data());

    constexpr VkQueueFlags RequiredTraceQueueFlags =
        VK_QUEUE_COMPUTE_BIT
        | VK_QUEUE_GRAPHICS_BIT;

    const auto isTraceCapable = [&queueFamilies](uint32_t index) {
        return (queueFamilies[index].queueFlags & RequiredTraceQueueFlags)
            == RequiredTraceQueueFlags;
    };

    const auto canPresent = [physicalDevice, surface](uint32_t index) {
        VkBool32 presentSupported = VK_FALSE;
        const VkResult result = vkGetPhysicalDeviceSurfaceSupportKHR(
            physicalDevice,
            index,
            surface,
            &presentSupported);

        return result == VK_SUCCESS && presentSupported == VK_TRUE;
    };

    QueueFamilyIndices indices{};

    for (uint32_t index = 0; index < queueFamilyCount; ++index) {
        if (isTraceCapable(index) && canPresent(index)) {
            indices.traceFamily = index;
            indices.hasTraceFamily = true;
            indices.presentFamily = index;
            indices.hasPresentFamily = true;
            return indices;
        }
    }

    // No combined family exists: take the first match for each role independently. The
    // split forces CONCURRENT sharing on the swapchain images.
    for (uint32_t index = 0; index < queueFamilyCount && !indices.isComplete(); ++index) {
        if (!indices.hasTraceFamily && isTraceCapable(index)) {
            indices.traceFamily = index;
            indices.hasTraceFamily = true;
        }

        if (!indices.hasPresentFamily && canPresent(index)) {
            indices.presentFamily = index;
            indices.hasPresentFamily = true;
        }
    }

    return indices;
}

// Aggregate suitability test used by pickPhysicalDevice. *queueFamilies always receives
// the scan result, so the caller of a suitable device gets its indices without
// re-querying. Ordered cheapest-first so the short-circuiting && skips the more
// expensive enumeration/feature queries once a device has already failed an earlier check.
bool isPhysicalDeviceSuitable(
    VkPhysicalDevice physicalDevice,
    VkSurfaceKHR surface,
    QueueFamilyIndices* queueFamilies)
{
    *queueFamilies = findQueueFamilies(physicalDevice, surface);
    return queueFamilies->isComplete()
        && hasRequiredApiVersion(physicalDevice)
        && areRequiredDeviceExtensionsAvailable(physicalDevice)
        && hasRequiredSwapchainSupport(physicalDevice, surface)
        && areRequiredRayTracingFeaturesAvailable(physicalDevice);
}

} // namespace

void printVulkanVersion(uint32_t version)
{
    std::cout << VK_VERSION_MAJOR(version) << '.'
              << VK_VERSION_MINOR(version) << '.'
              << VK_VERSION_PATCH(version);
}

bool isValidationLayerAvailable(const char* requestedLayerName)
{
    uint32_t layerCount = 0;
    VkResult result = vkEnumerateInstanceLayerProperties(&layerCount, nullptr);

    if (result != VK_SUCCESS) {
        std::cerr << "Failed to enumerate Vulkan instance layers.\n";
        return false;
    }

    std::vector<VkLayerProperties> availableLayers(layerCount);
    result = vkEnumerateInstanceLayerProperties(&layerCount, availableLayers.data());

    if (result != VK_SUCCESS) {
        std::cerr << "Failed to read Vulkan instance layer properties.\n";
        return false;
    }

    for (const VkLayerProperties& layer : availableLayers) {
        if (std::strcmp(layer.layerName, requestedLayerName) == 0) {
            return true;
        }
    }

    return false;
}

bool isInstanceExtensionAvailable(const char* requestedExtensionName)
{
    uint32_t extensionCount = 0;
    VkResult result = vkEnumerateInstanceExtensionProperties(nullptr, &extensionCount, nullptr);

    if (result != VK_SUCCESS) {
        std::cerr << "Failed to enumerate Vulkan instance extensions.\n";
        return false;
    }

    std::vector<VkExtensionProperties> availableExtensions(extensionCount);
    result = vkEnumerateInstanceExtensionProperties(nullptr, &extensionCount, availableExtensions.data());

    if (result != VK_SUCCESS) {
        std::cerr << "Failed to read Vulkan instance extension properties.\n";
        return false;
    }

    for (const VkExtensionProperties& extension : availableExtensions) {
        if (std::strcmp(extension.extensionName, requestedExtensionName) == 0) {
            return true;
        }
    }

    return false;
}

VkDebugUtilsMessengerCreateInfoEXT makeDebugMessengerCreateInfo()
{
    VkDebugUtilsMessengerCreateInfoEXT createInfo{};
    createInfo.sType = VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CREATE_INFO_EXT;
    createInfo.messageSeverity = VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT
        | VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT;
    createInfo.messageType = VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT
        | VK_DEBUG_UTILS_MESSAGE_TYPE_PERFORMANCE_BIT_EXT;
    createInfo.pfnUserCallback = debugMessengerCallback;

    return createInfo;
}

VkResult createDebugUtilsMessenger(
    VkInstance instance,
    const VkDebugUtilsMessengerCreateInfoEXT* createInfo,
    VkDebugUtilsMessengerEXT* debugMessenger)
{
    const auto createFunction = reinterpret_cast<PFN_vkCreateDebugUtilsMessengerEXT>(
        vkGetInstanceProcAddr(instance, "vkCreateDebugUtilsMessengerEXT"));

    if (createFunction == nullptr) {
        return VK_ERROR_EXTENSION_NOT_PRESENT;
    }

    return createFunction(instance, createInfo, nullptr, debugMessenger);
}

void destroyDebugUtilsMessenger(VkInstance instance, VkDebugUtilsMessengerEXT debugMessenger)
{
    const auto destroyFunction = reinterpret_cast<PFN_vkDestroyDebugUtilsMessengerEXT>(
        vkGetInstanceProcAddr(instance, "vkDestroyDebugUtilsMessengerEXT"));

    if (destroyFunction != nullptr) {
        destroyFunction(instance, debugMessenger, nullptr);
    }
}

VkPhysicalDevice pickPhysicalDevice(
    VkInstance instance,
    VkSurfaceKHR surface,
    QueueFamilyIndices* queueFamilies)
{
    uint32_t physicalDeviceCount = 0;
    VkResult result = vkEnumeratePhysicalDevices(instance, &physicalDeviceCount, nullptr);

    if (result != VK_SUCCESS) {
        std::cerr << "Failed to enumerate Vulkan physical devices.\n";
        return VK_NULL_HANDLE;
    }

    if (physicalDeviceCount == 0) {
        std::cerr << "No Vulkan physical devices were found.\n";
        return VK_NULL_HANDLE;
    }

    std::vector<VkPhysicalDevice> physicalDevices(physicalDeviceCount);
    result = vkEnumeratePhysicalDevices(instance, &physicalDeviceCount, physicalDevices.data());

    if (result != VK_SUCCESS) {
        std::cerr << "Failed to read Vulkan physical devices.\n";
        return VK_NULL_HANDLE;
    }

    for (VkPhysicalDevice physicalDevice : physicalDevices) {
        if (isPhysicalDeviceSuitable(physicalDevice, surface, queueFamilies)) {
            return physicalDevice;
        }
    }

    std::cerr << "No suitable Vulkan physical device was found.\n";
    return VK_NULL_HANDLE;
}

VkResult createLogicalDevice(
    VkPhysicalDevice physicalDevice,
    const QueueFamilyIndices& queueFamilies,
    VkDevice* device)
{
    const float queuePriority = 1.0f;

    // Build one VkDeviceQueueCreateInfo per *distinct* family: requesting the same
    // family twice is invalid, and trace/present often resolve to the same index.
    std::vector<uint32_t> uniqueQueueFamilies;
    const auto addUniqueQueueFamily = [&uniqueQueueFamilies](uint32_t queueFamily) {
        for (uint32_t existingQueueFamily : uniqueQueueFamilies) {
            if (existingQueueFamily == queueFamily) {
                return;
            }
        }

        uniqueQueueFamilies.push_back(queueFamily);
    };

    addUniqueQueueFamily(queueFamilies.traceFamily);
    addUniqueQueueFamily(queueFamilies.presentFamily);

    std::vector<VkDeviceQueueCreateInfo> queueCreateInfos;
    queueCreateInfos.reserve(uniqueQueueFamilies.size());

    for (uint32_t queueFamily : uniqueQueueFamilies) {
        VkDeviceQueueCreateInfo queueCreateInfo{};
        queueCreateInfo.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
        queueCreateInfo.queueFamilyIndex = queueFamily;
        queueCreateInfo.queueCount = 1;
        queueCreateInfo.pQueuePriorities = &queuePriority;
        queueCreateInfos.push_back(queueCreateInfo);
    }

    // Enable the same feature chain that areRequiredRayTracingFeaturesAvailable verified,
    // this time with the flags set to VK_TRUE so the device exposes them. The chain is
    // passed through VkPhysicalDeviceFeatures2 on the create info's pNext.
    VkPhysicalDeviceBufferDeviceAddressFeatures bufferDeviceAddressFeatures{};
    bufferDeviceAddressFeatures.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_BUFFER_DEVICE_ADDRESS_FEATURES;
    bufferDeviceAddressFeatures.bufferDeviceAddress = VK_TRUE;

    VkPhysicalDeviceRayTracingPipelineFeaturesKHR rayTracingPipelineFeatures{};
    rayTracingPipelineFeatures.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_RAY_TRACING_PIPELINE_FEATURES_KHR;
    rayTracingPipelineFeatures.pNext = &bufferDeviceAddressFeatures;
    rayTracingPipelineFeatures.rayTracingPipeline = VK_TRUE;

    VkPhysicalDeviceAccelerationStructureFeaturesKHR accelerationStructureFeatures{};
    accelerationStructureFeatures.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ACCELERATION_STRUCTURE_FEATURES_KHR;
    accelerationStructureFeatures.pNext = &rayTracingPipelineFeatures;
    accelerationStructureFeatures.accelerationStructure = VK_TRUE;

    VkPhysicalDeviceFeatures2 deviceFeatures{};
    deviceFeatures.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2;
    deviceFeatures.pNext = &accelerationStructureFeatures;

    VkDeviceCreateInfo deviceCreateInfo{};
    deviceCreateInfo.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
    deviceCreateInfo.pNext = &deviceFeatures;
    deviceCreateInfo.queueCreateInfoCount = static_cast<uint32_t>(queueCreateInfos.size());
    deviceCreateInfo.pQueueCreateInfos = queueCreateInfos.data();
    deviceCreateInfo.enabledExtensionCount = static_cast<uint32_t>(std::size(RequiredDeviceExtensions));
    deviceCreateInfo.ppEnabledExtensionNames = RequiredDeviceExtensions;

    return vkCreateDevice(physicalDevice, &deviceCreateInfo, nullptr, device);
}

VkResult createCommandPool(
    VkDevice device,
    const QueueFamilyIndices& queueFamilies,
    VkCommandPool* commandPool)
{
    VkCommandPoolCreateInfo commandPoolCreateInfo{};
    commandPoolCreateInfo.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
    commandPoolCreateInfo.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    commandPoolCreateInfo.queueFamilyIndex = queueFamilies.traceFamily;

    return vkCreateCommandPool(device, &commandPoolCreateInfo, nullptr, commandPool);
}

VkResult allocateCommandBuffer(
    VkDevice device,
    VkCommandPool commandPool,
    VkCommandBuffer* commandBuffer)
{
    VkCommandBufferAllocateInfo commandBufferAllocateInfo{};
    commandBufferAllocateInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    commandBufferAllocateInfo.commandPool = commandPool;
    commandBufferAllocateInfo.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    commandBufferAllocateInfo.commandBufferCount = 1;

    return vkAllocateCommandBuffers(device, &commandBufferAllocateInfo, commandBuffer);
}

VkResult createFrameSyncObjects(
    VkDevice device,
    VkSemaphore* imageAvailableSemaphore,
    VkFence* inFlightFence)
{
    VkSemaphoreCreateInfo semaphoreCreateInfo{};
    semaphoreCreateInfo.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;

    VkResult result = vkCreateSemaphore(
        device,
        &semaphoreCreateInfo,
        nullptr,
        imageAvailableSemaphore);

    if (result != VK_SUCCESS) {
        return result;
    }

    // Created already signaled so the very first drawFrame can wait on it without
    // deadlocking (there is no prior submission to signal it).
    VkFenceCreateInfo fenceCreateInfo{};
    fenceCreateInfo.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
    fenceCreateInfo.flags = VK_FENCE_CREATE_SIGNALED_BIT;

    result = vkCreateFence(device, &fenceCreateInfo, nullptr, inFlightFence);

    if (result != VK_SUCCESS) {
        // Roll back the semaphore so a failed call leaves no half-built sync state.
        vkDestroySemaphore(device, *imageAvailableSemaphore, nullptr);
        *imageAvailableSemaphore = VK_NULL_HANDLE;
        return result;
    }

    return VK_SUCCESS;
}

bool findMemoryType(
    VkPhysicalDevice physicalDevice,
    uint32_t typeBits,
    VkMemoryPropertyFlags properties,
    uint32_t* memoryTypeIndex)
{
    VkPhysicalDeviceMemoryProperties memoryProperties{};
    vkGetPhysicalDeviceMemoryProperties(physicalDevice, &memoryProperties);

    for (uint32_t index = 0; index < memoryProperties.memoryTypeCount; ++index) {
        const bool typeAllowed = (typeBits & (1u << index)) != 0;
        const bool propertiesMatch =
            (memoryProperties.memoryTypes[index].propertyFlags & properties) == properties;

        if (typeAllowed && propertiesMatch) {
            *memoryTypeIndex = index;
            return true;
        }
    }

    return false;
}

VkResult createBuffer(
    VkPhysicalDevice physicalDevice,
    VkDevice device,
    VkDeviceSize size,
    VkBufferUsageFlags usage,
    VkMemoryPropertyFlags memoryProperties,
    VkBuffer* buffer,
    VkDeviceMemory* memory)
{
    VkBufferCreateInfo bufferCreateInfo{};
    bufferCreateInfo.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    bufferCreateInfo.size = size;
    bufferCreateInfo.usage = usage;
    bufferCreateInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;

    VkResult result = vkCreateBuffer(device, &bufferCreateInfo, nullptr, buffer);

    if (result != VK_SUCCESS) {
        return result;
    }

    VkMemoryRequirements memoryRequirements{};
    vkGetBufferMemoryRequirements(device, *buffer, &memoryRequirements);

    uint32_t memoryTypeIndex = 0;
    if (!findMemoryType(
            physicalDevice,
            memoryRequirements.memoryTypeBits,
            memoryProperties,
            &memoryTypeIndex)) {
        return VK_ERROR_FEATURE_NOT_PRESENT;
    }

    // Taking a buffer's device address requires the DEVICE_ADDRESS flag on the backing
    // allocation, not just the SHADER_DEVICE_ADDRESS usage on the buffer — deriving it
    // from the usage keeps the two in lockstep so callers cannot request one without
    // the other.
    VkMemoryAllocateFlagsInfo allocateFlagsInfo{};
    allocateFlagsInfo.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_FLAGS_INFO;
    allocateFlagsInfo.flags = VK_MEMORY_ALLOCATE_DEVICE_ADDRESS_BIT;

    const bool needsDeviceAddress =
        (usage & VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT) != 0;

    VkMemoryAllocateInfo allocateInfo{};
    allocateInfo.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    allocateInfo.pNext = needsDeviceAddress ? &allocateFlagsInfo : nullptr;
    allocateInfo.allocationSize = memoryRequirements.size;
    allocateInfo.memoryTypeIndex = memoryTypeIndex;

    result = vkAllocateMemory(device, &allocateInfo, nullptr, memory);

    if (result != VK_SUCCESS) {
        return result;
    }

    return vkBindBufferMemory(device, *buffer, *memory, 0);
}

bool loadRayTracingFunctions(VkDevice device, RayTracingFunctions* functions)
{
    functions->getBufferDeviceAddress = reinterpret_cast<PFN_vkGetBufferDeviceAddressKHR>(
        vkGetDeviceProcAddr(device, "vkGetBufferDeviceAddressKHR"));
    functions->createAccelerationStructure = reinterpret_cast<PFN_vkCreateAccelerationStructureKHR>(
        vkGetDeviceProcAddr(device, "vkCreateAccelerationStructureKHR"));
    functions->destroyAccelerationStructure = reinterpret_cast<PFN_vkDestroyAccelerationStructureKHR>(
        vkGetDeviceProcAddr(device, "vkDestroyAccelerationStructureKHR"));
    functions->getAccelerationStructureBuildSizes = reinterpret_cast<PFN_vkGetAccelerationStructureBuildSizesKHR>(
        vkGetDeviceProcAddr(device, "vkGetAccelerationStructureBuildSizesKHR"));
    functions->getAccelerationStructureDeviceAddress = reinterpret_cast<PFN_vkGetAccelerationStructureDeviceAddressKHR>(
        vkGetDeviceProcAddr(device, "vkGetAccelerationStructureDeviceAddressKHR"));
    functions->cmdBuildAccelerationStructures = reinterpret_cast<PFN_vkCmdBuildAccelerationStructuresKHR>(
        vkGetDeviceProcAddr(device, "vkCmdBuildAccelerationStructuresKHR"));
    functions->createRayTracingPipelines = reinterpret_cast<PFN_vkCreateRayTracingPipelinesKHR>(
        vkGetDeviceProcAddr(device, "vkCreateRayTracingPipelinesKHR"));
    functions->getRayTracingShaderGroupHandles = reinterpret_cast<PFN_vkGetRayTracingShaderGroupHandlesKHR>(
        vkGetDeviceProcAddr(device, "vkGetRayTracingShaderGroupHandlesKHR"));
    functions->cmdTraceRays = reinterpret_cast<PFN_vkCmdTraceRaysKHR>(
        vkGetDeviceProcAddr(device, "vkCmdTraceRaysKHR"));

    return functions->isComplete();
}

VulkanContext::~VulkanContext()
{
    // Tear down in reverse creation order. Each handle is null-guarded so this runs
    // correctly no matter how far bring-up got before a failure path returned. Wait for
    // the device to finish any in-flight work before destroying anything it owns.
    if (device != VK_NULL_HANDLE) {
        (void)vkDeviceWaitIdle(device);
    }

    if (inFlightFence != VK_NULL_HANDLE && device != VK_NULL_HANDLE) {
        vkDestroyFence(device, inFlightFence, nullptr);
        std::cout << "Destroyed Vulkan in-flight fence.\n";
    }

    if (imageAvailableSemaphore != VK_NULL_HANDLE && device != VK_NULL_HANDLE) {
        vkDestroySemaphore(device, imageAvailableSemaphore, nullptr);
        std::cout << "Destroyed Vulkan image-available semaphore.\n";
    }

    if (commandBuffer != VK_NULL_HANDLE
        && device != VK_NULL_HANDLE
        && commandPool != VK_NULL_HANDLE) {
        vkFreeCommandBuffers(device, commandPool, 1, &commandBuffer);
        std::cout << "Freed Vulkan command buffer.\n";
    }

    if (commandPool != VK_NULL_HANDLE && device != VK_NULL_HANDLE) {
        vkDestroyCommandPool(device, commandPool, nullptr);
        std::cout << "Destroyed Vulkan command pool.\n";
    }

    if (device != VK_NULL_HANDLE) {
        vkDestroyDevice(device, nullptr);
        std::cout << "Destroyed Vulkan logical device.\n";
    }

    if (surface != VK_NULL_HANDLE && instance != VK_NULL_HANDLE) {
        vkDestroySurfaceKHR(instance, surface, nullptr);
        std::cout << "Destroyed Vulkan surface.\n";
    }

    if (debugMessenger != VK_NULL_HANDLE && instance != VK_NULL_HANDLE) {
        destroyDebugUtilsMessenger(instance, debugMessenger);
        std::cout << "Destroyed Vulkan debug messenger.\n";
    }

    if (instance != VK_NULL_HANDLE) {
        vkDestroyInstance(instance, nullptr);
        std::cout << "Destroyed Vulkan instance.\n";
    }

    if (window != nullptr) {
        glfwDestroyWindow(window);
        std::cout << "Destroyed GLFW window.\n";
    }

    if (glfwInitialized) {
        glfwTerminate();
        std::cout << "Terminated GLFW.\n";
    }
}
}
