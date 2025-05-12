#pragma once

#include <vulkan/vulkan.hpp>
#include <SDL3/SDL.h>
#include <vector>

class VulkanManager
{
public:
    VulkanManager();
    VulkanManager(std::vector<const char *> &extensions, SDL_Window *window);

    ~VulkanManager();

    void CreateVkInstance();
    void DestroyVkInstance();

    void EnumeratePhysicalDevices();
    void CreateVkDevice();
    void DestroyVkDevice();

    void CreateVkCommandBuffer();
    void DestroyVkCommandBuffer();
private:
    
private:
    vk::Instance instance;

    std::vector<const char *> instanceLayers = {
            "VK_LAYER_KHRONOS_validation"};
    std::vector<const char *> instanceExtensions = {};
    std::vector<const char *> deviceExtensions = {
            VK_KHR_SWAPCHAIN_EXTENSION_NAME};
    
    const int GPUIndex = 0;
    SDL_Window *sdlWindow = nullptr;
    
    uint32_t gpuCount = 0;
    std::vector<vk::PhysicalDevice> physicalDevices;
    vk::PhysicalDeviceMemoryProperties gpuMemoryProperties;
    uint32_t queueFamilyCount = 0;
    std::vector<vk::QueueFamilyProperties> queueFamilyProperties;
    uint32_t graphicQueueFamilyIndex = 0;
    uint32_t presentQueueFamilyIndex = 0;

    vk::Device device;

    vk::CommandPool commandPool;
    vk::CommandBuffer commandBuffer;
    vk::CommandBufferBeginInfo commandBufferBeginInfo;
    vk::CommandBuffer commandBuffers[1];
    vk::SubmitInfo submitInfo[1];

};
