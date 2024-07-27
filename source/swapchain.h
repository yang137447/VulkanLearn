#pragma once

#include "vulkan/vulkan.hpp"
#include "BaseStructs.h"

class Swapchain
{
public:
    Swapchain(const uint32_t windowWidth, const uint32_t windowHeight,
              const vk::PhysicalDevice &physicalDevice, const vk::Device &device, const vk::SurfaceKHR &surface,
              const QueueFamilyIndices &queueFamilyIndices);
    ~Swapchain();

private:
    Swapchain(/* args */);
    // input data
    vk::Device device;
    // data
    vk::Extent2D imageExtent;
    uint32_t imageCount;
    vk::SurfaceFormatKHR imageFormat;
    vk::SurfaceTransformFlagsKHR surfaceTransform;
    vk::PresentModeKHR presentMode;
    vk::SwapchainKHR swapchain;

    // function
    void QuerySwapchainData(
        const uint32_t windowWidth, const uint32_t windowHeight,
        const vk::PhysicalDevice &physicalDevice, const vk::SurfaceKHR &surface);
};