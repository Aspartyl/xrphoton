#include <cstring>
#include <iostream>
#include <vector>
#include <vulkan/vulkan.h>

namespace
{
constexpr const char* ValidationLayerName = "VK_LAYER_KHRONOS_validation";
constexpr const char* RequiredDeviceExtensions[] = {
    VK_KHR_ACCELERATION_STRUCTURE_EXTENSION_NAME,
    VK_KHR_RAY_TRACING_PIPELINE_EXTENSION_NAME,
    VK_KHR_BUFFER_DEVICE_ADDRESS_EXTENSION_NAME,
    VK_KHR_DEFERRED_HOST_OPERATIONS_EXTENSION_NAME,
    VK_KHR_PIPELINE_LIBRARY_EXTENSION_NAME,
};

uint32_t requiredDeviceExtensionCount()
{
    return static_cast<uint32_t>(
        sizeof(RequiredDeviceExtensions) / sizeof(RequiredDeviceExtensions[0]));
}

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

struct QueueFamilyIndices
{
    uint32_t graphicsFamily = 0;
    bool hasGraphicsFamily = false;

    bool isComplete() const
    {
        return hasGraphicsFamily;
    }
};

QueueFamilyIndices findQueueFamilies(VkPhysicalDevice physicalDevice)
{
    uint32_t queueFamilyCount = 0;
    vkGetPhysicalDeviceQueueFamilyProperties(physicalDevice, &queueFamilyCount, nullptr);

    std::vector<VkQueueFamilyProperties> queueFamilies(queueFamilyCount);
    vkGetPhysicalDeviceQueueFamilyProperties(physicalDevice, &queueFamilyCount, queueFamilies.data());

    QueueFamilyIndices indices{};

    for (uint32_t index = 0; index < queueFamilyCount; ++index) {
        if ((queueFamilies[index].queueFlags & VK_QUEUE_GRAPHICS_BIT) != 0) {
            indices.graphicsFamily = index;
            indices.hasGraphicsFamily = true;
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

bool isPhysicalDeviceSuitable(VkPhysicalDevice physicalDevice)
{
    const QueueFamilyIndices queueFamilies = findQueueFamilies(physicalDevice);
    return queueFamilies.isComplete()
        && areRequiredDeviceExtensionsAvailable(physicalDevice)
        && areRequiredRayTracingFeaturesAvailable(physicalDevice);
}

VkPhysicalDevice pickPhysicalDevice(VkInstance instance)
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
        if (isPhysicalDeviceSuitable(physicalDevice)) {
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

    VkDeviceQueueCreateInfo queueCreateInfo{};
    queueCreateInfo.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
    queueCreateInfo.queueFamilyIndex = queueFamilies.graphicsFamily;
    queueCreateInfo.queueCount = 1;
    queueCreateInfo.pQueuePriorities = &queuePriority;

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
    deviceCreateInfo.queueCreateInfoCount = 1;
    deviceCreateInfo.pQueueCreateInfos = &queueCreateInfo;
    deviceCreateInfo.enabledExtensionCount = requiredDeviceExtensionCount();
    deviceCreateInfo.ppEnabledExtensionNames = RequiredDeviceExtensions;

    return vkCreateDevice(physicalDevice, &deviceCreateInfo, nullptr, device);
}
}

int main()
{
    std::cout << "xrPhoton booting...\n";

    uint32_t instanceVersion = VK_API_VERSION_1_0;
    const VkResult result = vkEnumerateInstanceVersion(&instanceVersion);

    if (result != VK_SUCCESS) {
        std::cerr << "Failed to enumerate Vulkan instance version.\n";
        return 1;
    }

    std::cout << "Vulkan instance version: ";
    printVulkanVersion(instanceVersion);
    std::cout << '\n';

    if (!isValidationLayerAvailable(ValidationLayerName)) {
        std::cerr << "Required Vulkan validation layer is not available: "
                  << ValidationLayerName << '\n';
        return 1;
    }

    std::cout << "Using Vulkan validation layer: " << ValidationLayerName << '\n';

    if (!isInstanceExtensionAvailable(VK_EXT_DEBUG_UTILS_EXTENSION_NAME)) {
        std::cerr << "Required Vulkan instance extension is not available: "
                  << VK_EXT_DEBUG_UTILS_EXTENSION_NAME << '\n';
        return 1;
    }

    std::cout << "Using Vulkan instance extension: " << VK_EXT_DEBUG_UTILS_EXTENSION_NAME << '\n';

    const char* enabledLayers[] = {
        ValidationLayerName,
    };
    const char* enabledExtensions[] = {
        VK_EXT_DEBUG_UTILS_EXTENSION_NAME,
    };

    VkApplicationInfo applicationInfo{};
    applicationInfo.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
    applicationInfo.pApplicationName = "xrPhoton";
    applicationInfo.applicationVersion = VK_MAKE_VERSION(0, 1, 0);
    applicationInfo.pEngineName = "xrPhoton";
    applicationInfo.engineVersion = VK_MAKE_VERSION(0, 1, 0);
    applicationInfo.apiVersion = instanceVersion;

    VkDebugUtilsMessengerCreateInfoEXT debugMessengerCreateInfo = makeDebugMessengerCreateInfo();

    VkInstanceCreateInfo instanceCreateInfo{};
    instanceCreateInfo.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
    instanceCreateInfo.pNext = &debugMessengerCreateInfo;
    instanceCreateInfo.pApplicationInfo = &applicationInfo;
    instanceCreateInfo.enabledLayerCount = 1;
    instanceCreateInfo.ppEnabledLayerNames = enabledLayers;
    instanceCreateInfo.enabledExtensionCount = 1;
    instanceCreateInfo.ppEnabledExtensionNames = enabledExtensions;

    VkInstance instance = VK_NULL_HANDLE;
    const VkResult createResult = vkCreateInstance(&instanceCreateInfo, nullptr, &instance);

    if (createResult != VK_SUCCESS) {
        std::cerr << "Failed to create Vulkan instance.\n";
        return 1;
    }

    std::cout << "Created Vulkan instance.\n";

    VkDebugUtilsMessengerEXT debugMessenger = VK_NULL_HANDLE;
    const VkResult debugMessengerResult = createDebugUtilsMessenger(instance, &debugMessengerCreateInfo, &debugMessenger);

    if (debugMessengerResult != VK_SUCCESS) {
        std::cerr << "Failed to create Vulkan debug messenger.\n";
        vkDestroyInstance(instance, nullptr);
        return 1;
    }

    std::cout << "Created Vulkan debug messenger.\n";

    VkPhysicalDevice physicalDevice = pickPhysicalDevice(instance);

    if (physicalDevice == VK_NULL_HANDLE) {
        destroyDebugUtilsMessenger(instance, debugMessenger);
        vkDestroyInstance(instance, nullptr);
        return 1;
    }

    VkPhysicalDeviceProperties physicalDeviceProperties{};
    vkGetPhysicalDeviceProperties(physicalDevice, &physicalDeviceProperties);

    const QueueFamilyIndices queueFamilies = findQueueFamilies(physicalDevice);
    std::cout << "Selected Vulkan physical device: "
              << physicalDeviceProperties.deviceName << '\n';
    std::cout << "Using graphics queue family: "
              << queueFamilies.graphicsFamily << '\n';

    VkDevice device = VK_NULL_HANDLE;
    const VkResult deviceResult = createLogicalDevice(physicalDevice, queueFamilies, &device);

    if (deviceResult != VK_SUCCESS) {
        std::cerr << "Failed to create Vulkan logical device.\n";
        destroyDebugUtilsMessenger(instance, debugMessenger);
        vkDestroyInstance(instance, nullptr);
        return 1;
    }

    std::cout << "Created Vulkan logical device with hardware ray tracing prerequisites.\n";

    VkQueue graphicsQueue = VK_NULL_HANDLE;
    vkGetDeviceQueue(device, queueFamilies.graphicsFamily, 0, &graphicsQueue);
    std::cout << "Retrieved Vulkan graphics queue.\n";

    vkDestroyDevice(device, nullptr);
    std::cout << "Destroyed Vulkan logical device.\n";

    destroyDebugUtilsMessenger(instance, debugMessenger);
    std::cout << "Destroyed Vulkan debug messenger.\n";

    vkDestroyInstance(instance, nullptr);
    std::cout << "Destroyed Vulkan instance.\n";

    return 0;
}
