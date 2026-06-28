#include <cstdint>
#include <cstring>
#include <iostream>
#include <iterator>
#include <vector>

#define GLFW_INCLUDE_VULKAN
#include <GLFW/glfw3.h>
#include <vulkan/vulkan.h>

namespace
{
constexpr const char* ValidationLayerName = "VK_LAYER_KHRONOS_validation";
constexpr uint32_t RequiredApiVersion = VK_API_VERSION_1_3;
constexpr int WindowWidth = 1920;
constexpr int WindowHeight = 1080;
constexpr const char* WindowTitle = "xrPhoton";
constexpr const char* RequiredDeviceExtensions[] = {
    VK_KHR_ACCELERATION_STRUCTURE_EXTENSION_NAME,
    VK_KHR_RAY_TRACING_PIPELINE_EXTENSION_NAME,
    VK_KHR_BUFFER_DEVICE_ADDRESS_EXTENSION_NAME,
    VK_KHR_DEFERRED_HOST_OPERATIONS_EXTENSION_NAME,
    VK_KHR_PIPELINE_LIBRARY_EXTENSION_NAME,
    VK_KHR_SWAPCHAIN_EXTENSION_NAME,
};

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

struct VulkanContext
{
    bool glfwInitialized = false;
    GLFWwindow* window = nullptr;
    VkInstance instance = VK_NULL_HANDLE;
    VkDebugUtilsMessengerEXT debugMessenger = VK_NULL_HANDLE;
    VkSurfaceKHR surface = VK_NULL_HANDLE;
    VkDevice device = VK_NULL_HANDLE;
    VkCommandPool commandPool = VK_NULL_HANDLE;
    VkCommandBuffer commandBuffer = VK_NULL_HANDLE;

    VulkanContext() = default;
    VulkanContext(const VulkanContext&) = delete;
    VulkanContext& operator=(const VulkanContext&) = delete;

    ~VulkanContext()
    {
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
};

struct QueueFamilyIndices
{
    uint32_t traceFamily = 0;
    bool hasTraceFamily = false;
    uint32_t presentFamily = 0;
    bool hasPresentFamily = false;

    bool isComplete() const
    {
        return hasTraceFamily && hasPresentFamily;
    }
};

struct SwapchainSupportDetails
{
    VkSurfaceCapabilitiesKHR capabilities{};
    std::vector<VkSurfaceFormatKHR> formats;
    std::vector<VkPresentModeKHR> presentModes;
    bool valid = false;
};

struct RayTracingFunctions
{
    PFN_vkGetBufferDeviceAddressKHR getBufferDeviceAddress = nullptr;
    PFN_vkCreateAccelerationStructureKHR createAccelerationStructure = nullptr;
    PFN_vkDestroyAccelerationStructureKHR destroyAccelerationStructure = nullptr;
    PFN_vkGetAccelerationStructureBuildSizesKHR getAccelerationStructureBuildSizes = nullptr;
    PFN_vkGetAccelerationStructureDeviceAddressKHR getAccelerationStructureDeviceAddress = nullptr;
    PFN_vkCmdBuildAccelerationStructuresKHR cmdBuildAccelerationStructures = nullptr;
    PFN_vkCreateRayTracingPipelinesKHR createRayTracingPipelines = nullptr;
    PFN_vkGetRayTracingShaderGroupHandlesKHR getRayTracingShaderGroupHandles = nullptr;
    PFN_vkCmdTraceRaysKHR cmdTraceRays = nullptr;

    bool isComplete() const
    {
        return getBufferDeviceAddress != nullptr
            && createAccelerationStructure != nullptr
            && destroyAccelerationStructure != nullptr
            && getAccelerationStructureBuildSizes != nullptr
            && getAccelerationStructureDeviceAddress != nullptr
            && cmdBuildAccelerationStructures != nullptr
            && createRayTracingPipelines != nullptr
            && getRayTracingShaderGroupHandles != nullptr
            && cmdTraceRays != nullptr;
    }
};

QueueFamilyIndices findQueueFamilies(VkPhysicalDevice physicalDevice, VkSurfaceKHR surface)
{
    uint32_t queueFamilyCount = 0;
    vkGetPhysicalDeviceQueueFamilyProperties(physicalDevice, &queueFamilyCount, nullptr);

    std::vector<VkQueueFamilyProperties> queueFamilies(queueFamilyCount);
    vkGetPhysicalDeviceQueueFamilyProperties(physicalDevice, &queueFamilyCount, queueFamilies.data());

    QueueFamilyIndices indices{};

    for (uint32_t index = 0; index < queueFamilyCount; ++index) {
        if (!indices.hasTraceFamily && (queueFamilies[index].queueFlags & VK_QUEUE_COMPUTE_BIT) != 0) {
            indices.traceFamily = index;
            indices.hasTraceFamily = true;
        }

        VkBool32 presentSupported = VK_FALSE;
        const VkResult presentSupportResult = vkGetPhysicalDeviceSurfaceSupportKHR(
            physicalDevice,
            index,
            surface,
            &presentSupported);

        if (!indices.hasPresentFamily
            && presentSupportResult == VK_SUCCESS
            && presentSupported == VK_TRUE) {
            indices.presentFamily = index;
            indices.hasPresentFamily = true;
        }

        if (indices.isComplete()) {
            break;
        }
    }

    return indices;
}

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

SwapchainSupportDetails querySwapchainSupport(VkPhysicalDevice physicalDevice, VkSurfaceKHR surface)
{
    SwapchainSupportDetails support{};

    VkResult result = vkGetPhysicalDeviceSurfaceCapabilitiesKHR(
        physicalDevice,
        surface,
        &support.capabilities);

    if (result != VK_SUCCESS) {
        return support;
    }

    uint32_t formatCount = 0;
    result = vkGetPhysicalDeviceSurfaceFormatsKHR(
        physicalDevice,
        surface,
        &formatCount,
        nullptr);

    if (result != VK_SUCCESS) {
        return support;
    }

    support.formats.resize(formatCount);

    if (formatCount > 0) {
        result = vkGetPhysicalDeviceSurfaceFormatsKHR(
            physicalDevice,
            surface,
            &formatCount,
            support.formats.data());

        if (result != VK_SUCCESS) {
            return support;
        }
    }

    uint32_t presentModeCount = 0;
    result = vkGetPhysicalDeviceSurfacePresentModesKHR(
        physicalDevice,
        surface,
        &presentModeCount,
        nullptr);

    if (result != VK_SUCCESS) {
        return support;
    }

    support.presentModes.resize(presentModeCount);

    if (presentModeCount > 0) {
        result = vkGetPhysicalDeviceSurfacePresentModesKHR(
            physicalDevice,
            surface,
            &presentModeCount,
            support.presentModes.data());

        if (result != VK_SUCCESS) {
            return support;
        }
    }

    support.valid = true;
    return support;
}

bool hasRequiredSwapchainSupport(VkPhysicalDevice physicalDevice, VkSurfaceKHR surface)
{
    const SwapchainSupportDetails support = querySwapchainSupport(physicalDevice, surface);

    return support.valid
        && !support.formats.empty()
        && !support.presentModes.empty()
        && (support.capabilities.supportedUsageFlags & VK_IMAGE_USAGE_TRANSFER_DST_BIT) != 0;
}

bool hasRequiredApiVersion(VkPhysicalDevice physicalDevice)
{
    VkPhysicalDeviceProperties properties{};
    vkGetPhysicalDeviceProperties(physicalDevice, &properties);

    return properties.apiVersion >= RequiredApiVersion;
}

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

bool isPhysicalDeviceSuitable(VkPhysicalDevice physicalDevice, VkSurfaceKHR surface)
{
    const QueueFamilyIndices queueFamilies = findQueueFamilies(physicalDevice, surface);
    return queueFamilies.isComplete()
        && hasRequiredApiVersion(physicalDevice)
        && areRequiredDeviceExtensionsAvailable(physicalDevice)
        && hasRequiredSwapchainSupport(physicalDevice, surface)
        && areRequiredRayTracingFeaturesAvailable(physicalDevice);
}

VkPhysicalDevice pickPhysicalDevice(VkInstance instance, VkSurfaceKHR surface)
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
        if (isPhysicalDeviceSuitable(physicalDevice, surface)) {
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

VkResult recordEmptyCommandBuffer(VkCommandBuffer commandBuffer)
{
    VkCommandBufferBeginInfo beginInfo{};
    beginInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    beginInfo.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;

    VkResult result = vkBeginCommandBuffer(commandBuffer, &beginInfo);

    if (result != VK_SUCCESS) {
        return result;
    }

    return vkEndCommandBuffer(commandBuffer);
}

VkResult submitCommandBufferAndWait(VkQueue queue, VkCommandBuffer commandBuffer)
{
    VkSubmitInfo submitInfo{};
    submitInfo.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    submitInfo.commandBufferCount = 1;
    submitInfo.pCommandBuffers = &commandBuffer;

    VkResult result = vkQueueSubmit(queue, 1, &submitInfo, VK_NULL_HANDLE);

    if (result != VK_SUCCESS) {
        return result;
    }

    return vkQueueWaitIdle(queue);
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
}

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
    (void)presentQueue;

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

    const VkResult recordCommandBufferResult = recordEmptyCommandBuffer(ctx.commandBuffer);

    if (recordCommandBufferResult != VK_SUCCESS) {
        std::cerr << "Failed to record Vulkan command buffer.\n";
        return 1;
    }

    std::cout << "Recorded empty Vulkan command buffer.\n";

    const VkResult submitCommandBufferResult = submitCommandBufferAndWait(traceQueue, ctx.commandBuffer);

    if (submitCommandBufferResult != VK_SUCCESS) {
        std::cerr << "Failed to submit Vulkan command buffer.\n";
        return 1;
    }

    std::cout << "Submitted Vulkan command buffer and waited for completion.\n";

    std::cout << "Entering GLFW event loop.\n";
    while (!glfwWindowShouldClose(ctx.window)) {
        glfwPollEvents();
    }
    std::cout << "Exited GLFW event loop.\n";

    return 0;
}
