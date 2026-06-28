#pragma once

#include <vector>

#include <vulkan/vulkan.h>
#include <GLFW/glfw3.h>

namespace xrphoton
{
struct QueueFamilyIndices;

// Owns the resources recreated on resize. Its VkDevice is non-owning (borrowed from
// VulkanContext) and is used only to destroy the children below. Its destructor waits
// for the device to go idle before tearing down, so it is safe to declare a Swapchain
// after the VulkanContext it borrows from (it destructs first, before the device).
struct Swapchain
{
    VkDevice device = VK_NULL_HANDLE;
    VkSwapchainKHR swapchain = VK_NULL_HANDLE;
    std::vector<VkImage> images;
    std::vector<VkImageView> imageViews;
    VkFormat imageFormat = VK_FORMAT_UNDEFINED;
    VkExtent2D extent{};
    std::vector<VkSemaphore> renderFinishedSemaphores;

    Swapchain() = default;
    Swapchain(const Swapchain&) = delete;
    Swapchain& operator=(const Swapchain&) = delete;
    ~Swapchain();
};

bool hasRequiredSwapchainSupport(VkPhysicalDevice physicalDevice, VkSurfaceKHR surface);
VkResult createSwapchainResources(
    Swapchain* swap,
    VkPhysicalDevice physicalDevice,
    VkDevice device,
    VkSurfaceKHR surface,
    GLFWwindow* window,
    const QueueFamilyIndices& queueFamilies);
VkResult recreateSwapchain(
    Swapchain* swap,
    VkPhysicalDevice physicalDevice,
    VkDevice device,
    VkSurfaceKHR surface,
    GLFWwindow* window,
    const QueueFamilyIndices& queueFamilies);
}
