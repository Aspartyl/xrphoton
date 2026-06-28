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

// Part of device suitability: true if the surface exposes at least one format and
// present mode and supports the image usages the render path needs. Kept here (rather
// than in vulkan_context) so physical-device selection and swapchain creation share
// one definition of "adequate swapchain support".
bool hasRequiredSwapchainSupport(VkPhysicalDevice physicalDevice, VkSurfaceKHR surface);

// Populate *swap with a fresh swapchain, its image views, and one render-finished
// semaphore per image. On failure the partially built resources are owned by *swap and
// torn down by ~Swapchain (or the next recreateSwapchain), so the caller need not unwind.
VkResult createSwapchainResources(
    Swapchain* swap,
    VkPhysicalDevice physicalDevice,
    VkDevice device,
    VkSurfaceKHR surface,
    GLFWwindow* window,
    const QueueFamilyIndices& queueFamilies);

// Rebuild the swapchain after a resize / out-of-date surface: wait until the window has
// a drawable (non-zero) framebuffer, idle the device, destroy the old resources, then
// create new ones. Blocking on a minimized window is expected. Returns VK_SUCCESS
// (leaving *swap empty) if the window is closing while waiting.
VkResult recreateSwapchain(
    Swapchain* swap,
    VkPhysicalDevice physicalDevice,
    VkDevice device,
    VkSurfaceKHR surface,
    GLFWwindow* window,
    const QueueFamilyIndices& queueFamilies);
}
