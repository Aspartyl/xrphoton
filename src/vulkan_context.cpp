#include "vulkan_context.hpp"

#include "acceleration_structure.hpp"
#include "swapchain.hpp"
#include "vk_mem_alloc.h"

#include <cstring>
#include <iostream>
#include <iterator>
#include <limits>
#include <string>
#include <vector>

namespace xrphoton
{
namespace
{
// Device extensions every selected GPU must support and that the logical device
// enables. The first three form the hardware ray tracing stack: acceleration
// structures, the RT pipeline, and deferred host operations (which
// VK_KHR_acceleration_structure requires enabled even though nothing here defers);
// the last is presentation. Deliberately absent: buffer device address is core in
// the 1.3 baseline (the feature is enabled via the core
// VkPhysicalDeviceBufferDeviceAddressFeatures struct, and a 1.3 driver need not
// still advertise the promoted KHR extension string), and pipeline libraries are
// only an optional interaction of the RT pipeline extension, never used here.
constexpr const char* RequiredDeviceExtensions[] = {
    VK_KHR_ACCELERATION_STRUCTURE_EXTENSION_NAME,
    VK_KHR_RAY_TRACING_PIPELINE_EXTENSION_NAME,
    VK_KHR_DEFERRED_HOST_OPERATIONS_EXTENSION_NAME,
    VK_KHR_SWAPCHAIN_EXTENSION_NAME,
};

const char* getVkResultName(VkResult result)
{
    switch (result) {
    case VK_SUCCESS: return "VK_SUCCESS";
    case VK_NOT_READY: return "VK_NOT_READY";
    case VK_TIMEOUT: return "VK_TIMEOUT";
    case VK_EVENT_SET: return "VK_EVENT_SET";
    case VK_EVENT_RESET: return "VK_EVENT_RESET";
    case VK_INCOMPLETE: return "VK_INCOMPLETE";
    case VK_PIPELINE_COMPILE_REQUIRED: return "VK_PIPELINE_COMPILE_REQUIRED";
    case VK_SUBOPTIMAL_KHR: return "VK_SUBOPTIMAL_KHR";
    case VK_ERROR_OUT_OF_HOST_MEMORY: return "VK_ERROR_OUT_OF_HOST_MEMORY";
    case VK_ERROR_OUT_OF_DEVICE_MEMORY: return "VK_ERROR_OUT_OF_DEVICE_MEMORY";
    case VK_ERROR_INITIALIZATION_FAILED: return "VK_ERROR_INITIALIZATION_FAILED";
    case VK_ERROR_DEVICE_LOST: return "VK_ERROR_DEVICE_LOST";
    case VK_ERROR_MEMORY_MAP_FAILED: return "VK_ERROR_MEMORY_MAP_FAILED";
    case VK_ERROR_LAYER_NOT_PRESENT: return "VK_ERROR_LAYER_NOT_PRESENT";
    case VK_ERROR_EXTENSION_NOT_PRESENT: return "VK_ERROR_EXTENSION_NOT_PRESENT";
    case VK_ERROR_FEATURE_NOT_PRESENT: return "VK_ERROR_FEATURE_NOT_PRESENT";
    case VK_ERROR_INCOMPATIBLE_DRIVER: return "VK_ERROR_INCOMPATIBLE_DRIVER";
    case VK_ERROR_TOO_MANY_OBJECTS: return "VK_ERROR_TOO_MANY_OBJECTS";
    case VK_ERROR_FORMAT_NOT_SUPPORTED: return "VK_ERROR_FORMAT_NOT_SUPPORTED";
    case VK_ERROR_FRAGMENTED_POOL: return "VK_ERROR_FRAGMENTED_POOL";
    case VK_ERROR_UNKNOWN: return "VK_ERROR_UNKNOWN";
    case VK_ERROR_OUT_OF_POOL_MEMORY: return "VK_ERROR_OUT_OF_POOL_MEMORY";
    case VK_ERROR_INVALID_EXTERNAL_HANDLE: return "VK_ERROR_INVALID_EXTERNAL_HANDLE";
    case VK_ERROR_FRAGMENTATION: return "VK_ERROR_FRAGMENTATION";
    case VK_ERROR_INVALID_OPAQUE_CAPTURE_ADDRESS: return "VK_ERROR_INVALID_OPAQUE_CAPTURE_ADDRESS";
    case VK_ERROR_SURFACE_LOST_KHR: return "VK_ERROR_SURFACE_LOST_KHR";
    case VK_ERROR_NATIVE_WINDOW_IN_USE_KHR: return "VK_ERROR_NATIVE_WINDOW_IN_USE_KHR";
    case VK_ERROR_OUT_OF_DATE_KHR: return "VK_ERROR_OUT_OF_DATE_KHR";
    case VK_ERROR_INCOMPATIBLE_DISPLAY_KHR: return "VK_ERROR_INCOMPATIBLE_DISPLAY_KHR";
    case VK_ERROR_VALIDATION_FAILED_EXT: return "VK_ERROR_VALIDATION_FAILED_EXT";
    default: return nullptr;
    }
}

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

// Result of enumerating the required extension set. Keeping both the query result and
// every missing name lets device selection explain an enumeration failure without
// misreporting it as unsupported hardware, and report all missing extensions at once.
struct RequiredDeviceExtensionSupport
{
    VkResult queryResult = VK_SUCCESS;
    std::vector<const char*> missingExtensions;

    bool isComplete() const
    {
        return queryResult == VK_SUCCESS && missingExtensions.empty();
    }

    bool hasRequiredExtension(const char* extensionName) const
    {
        if (queryResult != VK_SUCCESS) {
            return false;
        }

        for (const char* missingExtension : missingExtensions) {
            if (std::strcmp(missingExtension, extensionName) == 0) {
                return false;
            }
        }

        return true;
    }
};

RequiredDeviceExtensionSupport queryRequiredDeviceExtensionSupport(
    VkPhysicalDevice physicalDevice)
{
    RequiredDeviceExtensionSupport support{};
    uint32_t extensionCount = 0;
    support.queryResult = vkEnumerateDeviceExtensionProperties(
        physicalDevice,
        nullptr,
        &extensionCount,
        nullptr);

    if (support.queryResult != VK_SUCCESS) {
        return support;
    }

    std::vector<VkExtensionProperties> availableExtensions(extensionCount);

    if (extensionCount > 0) {
        support.queryResult = vkEnumerateDeviceExtensionProperties(
            physicalDevice,
            nullptr,
            &extensionCount,
            availableExtensions.data());

        if (support.queryResult != VK_SUCCESS) {
            return support;
        }
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
            support.missingExtensions.push_back(requiredExtension);
        }
    }

    return support;
}

// The feature structs for acceleration structures and ray tracing pipelines are only
// valid in a query when the candidate advertises their defining extensions. Callers
// therefore gate this function on the API version and those extension names.
struct RayTracingFeatureSupport
{
    bool wasQueried = false;
    bool hasBufferDeviceAddress = false;
    bool hasAccelerationStructure = false;
    bool hasRayTracingPipeline = false;

    bool isComplete() const
    {
        return wasQueried
            && hasBufferDeviceAddress
            && hasAccelerationStructure
            && hasRayTracingPipeline;
    }
};

// Query the ray tracing feature chain and confirm the device actually enables the
// three capabilities the renderer depends on. The structs are linked through pNext so a
// single vkGetPhysicalDeviceFeatures2 call fills them all; createLogicalDevice later
// re-uses the same chain shape to turn the features on.
RayTracingFeatureSupport queryRequiredRayTracingFeatureSupport(
    VkPhysicalDevice physicalDevice)
{
    RayTracingFeatureSupport support{};

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

    support.wasQueried = true;
    support.hasBufferDeviceAddress = bufferDeviceAddressFeatures.bufferDeviceAddress == VK_TRUE;
    support.hasAccelerationStructure =
        accelerationStructureFeatures.accelerationStructure == VK_TRUE;
    support.hasRayTracingPipeline = rayTracingPipelineFeatures.rayTracingPipeline == VK_TRUE;
    return support;
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

// Complete per-candidate report used both for the selection decision and for rejection
// diagnostics. Independent categories are all evaluated so one run describes every
// actionable problem instead of revealing them one launch at a time.
struct PhysicalDeviceSuitability
{
    VkPhysicalDeviceProperties properties{};
    QueueFamilyIndices queueFamilies{};
    RequiredDeviceExtensionSupport extensions{};
    RayTracingFeatureSupport rayTracingFeatures{};
    bool hasSwapchainSupport = false;
    bool hasAccelerationStructureFormatSupport = false;

    bool hasRequiredApiVersion() const
    {
        return properties.apiVersion >= RequiredApiVersion;
    }

    bool isSuitable() const
    {
        return queueFamilies.isComplete()
            && hasRequiredApiVersion()
            && extensions.isComplete()
            && hasSwapchainSupport
            && hasAccelerationStructureFormatSupport
            && rayTracingFeatures.isComplete();
    }
};

PhysicalDeviceSuitability queryPhysicalDeviceSuitability(
    VkPhysicalDevice physicalDevice,
    VkSurfaceKHR surface)
{
    PhysicalDeviceSuitability suitability{};
    vkGetPhysicalDeviceProperties(physicalDevice, &suitability.properties);
    suitability.queueFamilies = findQueueFamilies(physicalDevice, surface);
    suitability.extensions = queryRequiredDeviceExtensionSupport(physicalDevice);
    suitability.hasSwapchainSupport = hasRequiredSwapchainSupport(physicalDevice, surface);
    suitability.hasAccelerationStructureFormatSupport =
        hasRequiredAccelerationStructureFormatSupport(physicalDevice);

    // Buffer device address is core from VK 1.2 onward, while the other two feature
    // structs remain defined by device extensions. Do not put unsupported structs in
    // pNext, even though querying a 1.2 candidate can still enrich its 1.3 rejection.
    const bool canQueryRayTracingFeatures =
        suitability.properties.apiVersion >= VK_API_VERSION_1_2
        && suitability.extensions.hasRequiredExtension(
            VK_KHR_ACCELERATION_STRUCTURE_EXTENSION_NAME)
        && suitability.extensions.hasRequiredExtension(
            VK_KHR_RAY_TRACING_PIPELINE_EXTENSION_NAME);

    if (canQueryRayTracingFeatures) {
        suitability.rayTracingFeatures =
            queryRequiredRayTracingFeatureSupport(physicalDevice);
    }

    return suitability;
}

void reportPhysicalDeviceRejection(const PhysicalDeviceSuitability& suitability)
{
    const char* deviceName = suitability.properties.deviceName[0] != '\0'
        ? suitability.properties.deviceName
        : "<unnamed>";
    std::cerr << "Rejected Vulkan physical device \"" << deviceName << "\":\n";

    if (!suitability.queueFamilies.hasTraceFamily) {
        std::cerr << "  - no queue family supports both graphics and compute\n";
    }

    if (!suitability.queueFamilies.hasPresentFamily) {
        std::cerr << "  - no usable present queue family was found for this surface\n";
    }

    if (!suitability.hasRequiredApiVersion()) {
        std::cerr << "  - requires Vulkan "
                  << VK_VERSION_MAJOR(RequiredApiVersion) << '.'
                  << VK_VERSION_MINOR(RequiredApiVersion) << '.'
                  << VK_VERSION_PATCH(RequiredApiVersion)
                  << ", but the device exposes "
                  << VK_VERSION_MAJOR(suitability.properties.apiVersion) << '.'
                  << VK_VERSION_MINOR(suitability.properties.apiVersion) << '.'
                  << VK_VERSION_PATCH(suitability.properties.apiVersion) << '\n';
    }

    if (suitability.extensions.queryResult != VK_SUCCESS) {
        std::cerr << "  - could not enumerate device extensions: "
                  << formatVkResult(suitability.extensions.queryResult) << '\n';
    } else {
        for (const char* missingExtension : suitability.extensions.missingExtensions) {
            std::cerr << "  - missing required device extension "
                      << missingExtension << '\n';
        }
    }

    if (!suitability.hasSwapchainSupport) {
        std::cerr << "  - required swapchain support is unavailable (surface queries, "
                     "formats, present modes, image usages, or storage/blit capabilities)\n";
    }

    if (!suitability.hasAccelerationStructureFormatSupport
        && suitability.extensions.hasRequiredExtension(
            VK_KHR_ACCELERATION_STRUCTURE_EXTENSION_NAME)) {
        std::cerr << "  - the BLAS vertex format cannot be used for acceleration-structure builds\n";
    }

    if (suitability.rayTracingFeatures.wasQueried) {
        if (!suitability.rayTracingFeatures.hasBufferDeviceAddress) {
            std::cerr << "  - missing required feature bufferDeviceAddress\n";
        }

        if (!suitability.rayTracingFeatures.hasAccelerationStructure) {
            std::cerr << "  - missing required feature accelerationStructure\n";
        }

        if (!suitability.rayTracingFeatures.hasRayTracingPipeline) {
            std::cerr << "  - missing required feature rayTracingPipeline\n";
        }
    }
}

// The destination allocation is parked directly in its caller-owned output, but the
// staging allocation has no owner beyond one upload. This local owner keeps every
// pre-submit failure path leak-free without adding staging state to a program-lifetime
// resource owner.
struct StagingBuffer
{
    VmaAllocator allocator = nullptr;
    VkBuffer buffer = VK_NULL_HANDLE;
    VmaAllocation allocation = nullptr;

    StagingBuffer() = default;
    StagingBuffer(const StagingBuffer&) = delete;
    StagingBuffer& operator=(const StagingBuffer&) = delete;

    ~StagingBuffer()
    {
        if (buffer != VK_NULL_HANDLE || allocation != nullptr) {
            vmaDestroyBuffer(allocator, buffer, allocation);
        }
    }
};

} // namespace

void printVulkanVersion(uint32_t version)
{
    std::cout << VK_VERSION_MAJOR(version) << '.'
              << VK_VERSION_MINOR(version) << '.'
              << VK_VERSION_PATCH(version);
}

std::string formatVkResult(VkResult result)
{
    const char* name = getVkResultName(result);
    std::string formatted = name != nullptr ? name : "VkResult";
    formatted += " (";
    formatted += std::to_string(static_cast<int32_t>(result));
    formatted += ')';
    return formatted;
}

bool isValidationLayerAvailable(const char* requestedLayerName)
{
    uint32_t layerCount = 0;
    VkResult result = vkEnumerateInstanceLayerProperties(&layerCount, nullptr);

    if (result != VK_SUCCESS) {
        std::cerr << "Failed to enumerate Vulkan instance layers: "
                  << formatVkResult(result) << ".\n";
        return false;
    }

    std::vector<VkLayerProperties> availableLayers(layerCount);
    result = vkEnumerateInstanceLayerProperties(&layerCount, availableLayers.data());

    if (result != VK_SUCCESS) {
        std::cerr << "Failed to read Vulkan instance layer properties: "
                  << formatVkResult(result) << ".\n";
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
        std::cerr << "Failed to enumerate Vulkan instance extensions: "
                  << formatVkResult(result) << ".\n";
        return false;
    }

    std::vector<VkExtensionProperties> availableExtensions(extensionCount);
    result = vkEnumerateInstanceExtensionProperties(nullptr, &extensionCount, availableExtensions.data());

    if (result != VK_SUCCESS) {
        std::cerr << "Failed to read Vulkan instance extension properties: "
                  << formatVkResult(result) << ".\n";
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
        std::cerr << "Failed to enumerate Vulkan physical devices: "
                  << formatVkResult(result) << ".\n";
        return VK_NULL_HANDLE;
    }

    if (physicalDeviceCount == 0) {
        std::cerr << "No Vulkan physical devices were found.\n";
        return VK_NULL_HANDLE;
    }

    std::vector<VkPhysicalDevice> physicalDevices(physicalDeviceCount);
    result = vkEnumeratePhysicalDevices(instance, &physicalDeviceCount, physicalDevices.data());

    if (result != VK_SUCCESS) {
        std::cerr << "Failed to read Vulkan physical devices: "
                  << formatVkResult(result) << ".\n";
        return VK_NULL_HANDLE;
    }

    for (VkPhysicalDevice physicalDevice : physicalDevices) {
        const PhysicalDeviceSuitability suitability =
            queryPhysicalDeviceSuitability(physicalDevice, surface);
        *queueFamilies = suitability.queueFamilies;

        if (suitability.isSuitable()) {
            return physicalDevice;
        }

        reportPhysicalDeviceRejection(suitability);
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

    // Enable the same feature chain that queryRequiredRayTracingFeatureSupport verified,
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

VkResult createAllocator(
    VkInstance instance,
    VkPhysicalDevice physicalDevice,
    VkDevice device,
    VmaAllocator* allocator)
{
    VmaAllocatorCreateInfo createInfo{};
    createInfo.flags = VMA_ALLOCATOR_CREATE_BUFFER_DEVICE_ADDRESS_BIT;
    createInfo.physicalDevice = physicalDevice;
    createInfo.device = device;
    createInfo.instance = instance;
    // VMA otherwise assumes Vulkan 1.0 and silently avoids newer core entry points.
    createInfo.vulkanApiVersion = RequiredApiVersion;

    return vmaCreateAllocator(&createInfo, allocator);
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

VkResult allocateCommandBuffers(
    VkDevice device,
    VkCommandPool commandPool,
    std::array<FrameResources, MaxFramesInFlight>* frames)
{
    VkCommandBufferAllocateInfo commandBufferAllocateInfo{};
    commandBufferAllocateInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    commandBufferAllocateInfo.commandPool = commandPool;
    commandBufferAllocateInfo.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    commandBufferAllocateInfo.commandBufferCount = 1;

    // One allocation call per slot rather than one batched call: the batched form
    // writes a contiguous VkCommandBuffer array, which the commandBuffer members of an
    // array of FrameResources are not. On mid-loop failure the buffers allocated so
    // far stay in *frames for the VulkanContext destructor.
    for (FrameResources& frame : *frames) {
        const VkResult result = vkAllocateCommandBuffers(
            device,
            &commandBufferAllocateInfo,
            &frame.commandBuffer);

        if (result != VK_SUCCESS) {
            return result;
        }
    }

    return VK_SUCCESS;
}

VkResult createFrameSyncObjects(
    VkDevice device,
    std::array<FrameResources, MaxFramesInFlight>* frames)
{
    VkSemaphoreCreateInfo semaphoreCreateInfo{};
    semaphoreCreateInfo.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;

    // Created already signaled so the very first drawFrame can wait on it without
    // deadlocking (there is no prior submission to signal it).
    VkFenceCreateInfo fenceCreateInfo{};
    fenceCreateInfo.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
    fenceCreateInfo.flags = VK_FENCE_CREATE_SIGNALED_BIT;

    for (FrameResources& frame : *frames) {
        VkResult result = vkCreateSemaphore(
            device,
            &semaphoreCreateInfo,
            nullptr,
            &frame.imageAvailableSemaphore);

        if (result != VK_SUCCESS) {
            return result;
        }

        result = vkCreateFence(device, &fenceCreateInfo, nullptr, &frame.inFlightFence);

        if (result != VK_SUCCESS) {
            return result;
        }
    }

    return VK_SUCCESS;
}

VkResult createBuffer(
    VmaAllocator allocator,
    VkDeviceSize size,
    VkBufferUsageFlags usage,
    VmaAllocationCreateFlags allocationFlags,
    VkBuffer* buffer,
    VmaAllocation* allocation)
{
    VkBufferCreateInfo bufferCreateInfo{};
    bufferCreateInfo.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    bufferCreateInfo.size = size;
    bufferCreateInfo.usage = usage;
    bufferCreateInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;

    VmaAllocationCreateInfo allocationCreateInfo{};
    allocationCreateInfo.flags = allocationFlags;
    allocationCreateInfo.usage = VMA_MEMORY_USAGE_AUTO;

    constexpr VmaAllocationCreateFlags HostAccessFlags =
        VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT
        | VMA_ALLOCATION_CREATE_HOST_ACCESS_RANDOM_BIT;
    if ((allocationFlags & HostAccessFlags) != 0) {
        allocationCreateInfo.requiredFlags = VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
    } else {
        allocationCreateInfo.requiredFlags = VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT;
    }

    return vmaCreateBuffer(
        allocator,
        &bufferCreateInfo,
        &allocationCreateInfo,
        buffer,
        allocation,
        nullptr);
}

VkResult uploadDeviceLocalBuffer(
    VmaAllocator allocator,
    VkDevice device,
    VkCommandBuffer commandBuffer,
    VkQueue queue,
    VkFence fence,
    const void* data,
    VkDeviceSize size,
    VkBufferUsageFlags usage,
    VkBuffer* buffer,
    VmaAllocation* allocation)
{
    VkResult result = createBuffer(
        allocator,
        size,
        usage,
        0,
        buffer,
        allocation);

    if (result != VK_SUCCESS) {
        return result;
    }

    StagingBuffer staging{};
    staging.allocator = allocator;
    result = createBuffer(
        allocator,
        size,
        VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
        VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT
            | VMA_ALLOCATION_CREATE_MAPPED_BIT,
        &staging.buffer,
        &staging.allocation);

    if (result != VK_SUCCESS) {
        return result;
    }

    VmaAllocationInfo stagingAllocationInfo{};
    vmaGetAllocationInfo(allocator, staging.allocation, &stagingAllocationInfo);
    if (stagingAllocationInfo.pMappedData == nullptr) {
        return VK_ERROR_MEMORY_MAP_FAILED;
    }

    std::memcpy(stagingAllocationInfo.pMappedData, data, size);

    // The borrowed per-frame command buffer may still be executable from a previous
    // startup upload; the pool's RESET_COMMAND_BUFFER flag makes this reusable path
    // independent of its incoming non-pending state.
    result = vkResetCommandBuffer(commandBuffer, 0);
    if (result != VK_SUCCESS) {
        return result;
    }

    VkCommandBufferBeginInfo beginInfo{};
    beginInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    beginInfo.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;

    result = vkBeginCommandBuffer(commandBuffer, &beginInfo);
    if (result != VK_SUCCESS) {
        return result;
    }

    VkBufferCopy copyRegion{};
    copyRegion.size = size;
    vkCmdCopyBuffer(commandBuffer, staging.buffer, *buffer, 1, &copyRegion);

    // A fence wait only synchronizes the device with the host. This device-side
    // dependency makes the transfer write visible to AS builds and BDA shader reads
    // in later queue submissions, so those consumers need no upload-specific barrier.
    VkMemoryBarrier uploadBarrier{};
    uploadBarrier.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER;
    uploadBarrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    uploadBarrier.dstAccessMask = VK_ACCESS_ACCELERATION_STRUCTURE_READ_BIT_KHR
        | VK_ACCESS_SHADER_READ_BIT;

    vkCmdPipelineBarrier(
        commandBuffer,
        VK_PIPELINE_STAGE_TRANSFER_BIT,
        VK_PIPELINE_STAGE_ACCELERATION_STRUCTURE_BUILD_BIT_KHR
            | VK_PIPELINE_STAGE_RAY_TRACING_SHADER_BIT_KHR,
        0,
        1,
        &uploadBarrier,
        0,
        nullptr,
        0,
        nullptr);

    result = vkEndCommandBuffer(commandBuffer);
    if (result != VK_SUCCESS) {
        return result;
    }

    result = vkResetFences(device, 1, &fence);
    if (result != VK_SUCCESS) {
        return result;
    }

    VkSubmitInfo submitInfo{};
    submitInfo.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    submitInfo.commandBufferCount = 1;
    submitInfo.pCommandBuffers = &commandBuffer;

    result = vkQueueSubmit(queue, 1, &submitInfo, fence);
    if (result != VK_SUCCESS) {
        return result;
    }

    result = vkWaitForFences(
        device,
        1,
        &fence,
        VK_TRUE,
        std::numeric_limits<uint64_t>::max());

    if (result != VK_SUCCESS) {
        // The transient staging buffer cannot escape through the API. Retire any work
        // the failed fence wait may have left pending before its local owner destroys it.
        (void)vkQueueWaitIdle(queue);
        return result;
    }

    return VK_SUCCESS;
}

bool loadRayTracingFunctions(VkDevice device, RayTracingFunctions* functions)
{
    // Resolved by its core name: buffer device address is core in the 1.3 baseline,
    // and the KHR alias is only guaranteed to resolve when the promoted extension is
    // enabled — which it deliberately no longer is (see RequiredDeviceExtensions).
    functions->getBufferDeviceAddress = reinterpret_cast<PFN_vkGetBufferDeviceAddressKHR>(
        vkGetDeviceProcAddr(device, "vkGetBufferDeviceAddress"));
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

    // Each slot's sync objects go together (fence, then semaphore — reverse of their
    // per-slot creation order); there is no cross-slot ordering requirement.
    if (device != VK_NULL_HANDLE) {
        for (const FrameResources& frame : frames) {
            if (frame.inFlightFence != VK_NULL_HANDLE) {
                vkDestroyFence(device, frame.inFlightFence, nullptr);
                std::cout << "Destroyed Vulkan in-flight fence.\n";
            }

            if (frame.imageAvailableSemaphore != VK_NULL_HANDLE) {
                vkDestroySemaphore(device, frame.imageAvailableSemaphore, nullptr);
                std::cout << "Destroyed Vulkan image-available semaphore.\n";
            }
        }
    }

    if (device != VK_NULL_HANDLE && commandPool != VK_NULL_HANDLE) {
        for (const FrameResources& frame : frames) {
            if (frame.commandBuffer != VK_NULL_HANDLE) {
                vkFreeCommandBuffers(device, commandPool, 1, &frame.commandBuffer);
                std::cout << "Freed Vulkan command buffer.\n";
            }
        }
    }

    if (commandPool != VK_NULL_HANDLE && device != VK_NULL_HANDLE) {
        vkDestroyCommandPool(device, commandPool, nullptr);
        std::cout << "Destroyed Vulkan command pool.\n";
    }

    if (allocator != nullptr) {
        vmaDestroyAllocator(allocator);
        std::cout << "Destroyed Vulkan memory allocator.\n";
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
