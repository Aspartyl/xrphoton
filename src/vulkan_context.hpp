#pragma once

#include <cstdint>
#include <vector>

#include <vulkan/vulkan.h>
#include <GLFW/glfw3.h>

namespace xrphoton
{
// The single Khronos validation layer we request, and the minimum Vulkan API
// version the program targets (1.3 baseline).
constexpr const char* ValidationLayerName = "VK_LAYER_KHRONOS_validation";
constexpr uint32_t RequiredApiVersion = VK_API_VERSION_1_3;

// Owns every program-lifetime Vulkan/GLFW handle. Its destructor tears them down
// in reverse creation order (with null guards), so each failure path in main() is a
// bare return. The swapchain and its per-image sync live in Swapchain, not here.
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
    VkSemaphore imageAvailableSemaphore = VK_NULL_HANDLE;
    VkFence inFlightFence = VK_NULL_HANDLE;

    VulkanContext() = default;
    VulkanContext(const VulkanContext&) = delete;
    VulkanContext& operator=(const VulkanContext&) = delete;
    ~VulkanContext();
};

// Indices of the queue families the program needs: one compute-capable family to
// record/trace work ("trace") and one capable of presenting to the surface. The two
// may resolve to the same family. The booleans distinguish "found family 0" from
// "no family found", since 0 is a valid index.
struct QueueFamilyIndices
{
    uint32_t traceFamily = 0;
    bool hasTraceFamily = false;
    uint32_t presentFamily = 0;
    bool hasPresentFamily = false;

    // True once both required families have been located.
    bool isComplete() const
    {
        return hasTraceFamily && hasPresentFamily;
    }
};

// The ray tracing / acceleration structure entry points, which are extension
// functions that must be resolved at runtime via vkGetDeviceProcAddr rather than
// linked directly. Populated by loadRayTracingFunctions. Loaded but not yet used:
// they exist so the eventual renderer can build acceleration structures and trace.
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

    // True once every entry point above resolved to a non-null pointer.
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

// Print a packed VkVersion as "major.minor.patch" to std::cout (no trailing newline).
void printVulkanVersion(uint32_t version);

// True if the named instance layer / extension is present in this Vulkan runtime.
// Both enumerate the available set and report errors to std::cerr.
bool isValidationLayerAvailable(const char* requestedLayerName);
bool isInstanceExtensionAvailable(const char* requestedExtensionName);

// Build the debug-messenger create info (severity/type filters + callback). Returned
// by value so it can be reused both as the messenger's own config and, via pNext on
// the instance create info, to capture diagnostics during instance creation/teardown.
VkDebugUtilsMessengerCreateInfoEXT makeDebugMessengerCreateInfo();

// Create / destroy the debug-utils messenger. Because the messenger entry points are
// themselves extension functions, both resolve them through vkGetInstanceProcAddr;
// createDebugUtilsMessenger returns VK_ERROR_EXTENSION_NOT_PRESENT if unavailable, and
// destroyDebugUtilsMessenger is a no-op if the destroy pointer cannot be resolved.
VkResult createDebugUtilsMessenger(
    VkInstance instance,
    const VkDebugUtilsMessengerCreateInfoEXT* createInfo,
    VkDebugUtilsMessengerEXT* debugMessenger);
void destroyDebugUtilsMessenger(VkInstance instance, VkDebugUtilsMessengerEXT debugMessenger);

// Scan the device's queue families for a compute-capable (trace) family and a
// present-capable family, taking the first match of each. Call isComplete() on the
// result to check both were found.
QueueFamilyIndices findQueueFamilies(VkPhysicalDevice physicalDevice, VkSurfaceKHR surface);

// Pick the first physical device meeting every requirement (queue families, API
// version, device extensions, swapchain support, ray tracing features). Returns
// VK_NULL_HANDLE and logs to std::cerr if none qualifies.
VkPhysicalDevice pickPhysicalDevice(VkInstance instance, VkSurfaceKHR surface);

// Create the logical device with one queue per unique {trace, present} family and the
// ray tracing feature chain enabled.
VkResult createLogicalDevice(
    VkPhysicalDevice physicalDevice,
    const QueueFamilyIndices& queueFamilies,
    VkDevice* device);

// Create the command pool on the trace family (with per-buffer reset enabled) and
// allocate a single primary command buffer from it.
VkResult createCommandPool(
    VkDevice device,
    const QueueFamilyIndices& queueFamilies,
    VkCommandPool* commandPool);
VkResult allocateCommandBuffer(
    VkDevice device,
    VkCommandPool commandPool,
    VkCommandBuffer* commandBuffer);

// Create the per-frame sync objects for the single in-flight frame: an unsignaled
// image-available semaphore and an in-flight fence created already signaled (so the
// first drawFrame's wait returns immediately). On failure no handle is leaked.
VkResult createFrameSyncObjects(
    VkDevice device,
    VkSemaphore* imageAvailableSemaphore,
    VkFence* inFlightFence);

// Resolve every ray tracing entry point into *functions. Returns false (leaving any
// partially resolved pointers in place) if any one of them is unavailable.
bool loadRayTracingFunctions(VkDevice device, RayTracingFunctions* functions);
}
