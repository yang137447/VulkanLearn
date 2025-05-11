#pragma once

#include <memory>
#include "vulkan/vulkan.hpp"
#include "BaseStructs.h"

class Swapchain
{
public:
    Swapchain(const uint32_t windowWidth, const uint32_t windowHeight,
              const vk::PhysicalDevice &physicalDevice,
              const vk::Device &device,
              const vk::SurfaceKHR &surface,
              const vk::RenderPass &renderPass,
              const QueueFamilyIndices &queueFamilyIndices);
    ~Swapchain();

    inline vk::Extent2D GetExtent() const { return imageExtent; }
    inline vk::SwapchainKHR &GetSwapchain() { return swapchain; }
    inline vk::Format GetSwapchainFormat() const { return imageFormat.format; }
    inline uint32_t GetImageCount() const { return imageCount; }
    inline std::vector<vk::ImageView> &GetImageViews() { return imageViews; }

private:
    Swapchain(/* args */);
    // input data
    vk::Device device;
    vk::RenderPass renderPass;
    // data
    vk::Extent2D imageExtent;
    uint32_t imageCount;
    vk::SurfaceFormatKHR imageFormat;
    vk::SurfaceTransformFlagsKHR surfaceTransform;
    vk::PresentModeKHR presentMode;
    vk::SwapchainKHR swapchain;
    std::vector<vk::Image> images;
    std::vector<vk::ImageView> imageViews;

    // function
    void QuerySwapchainData(
        const uint32_t windowWidth, const uint32_t windowHeight,
        const vk::PhysicalDevice &physicalDevice, const vk::SurfaceKHR &surface);
    void CreateVkSwapChain(const vk::Device &device, const vk::SurfaceKHR &surface, const QueueFamilyIndices &queueFamilyIndices);
    void DestroyVkSwapChain();
    void GetVkImages();
    void CreateVkImageViews();
    void DestroyVkImageViews();
};