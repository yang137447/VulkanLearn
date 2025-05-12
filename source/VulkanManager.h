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

    void createInstance();
    void destroyInstance();
private:
    
private:
    vk::Instance instance;
        // data
    std::vector<const char *> instanceLayers = {
            "VK_LAYER_KHRONOS_validation"};
    
    std::vector<const char *> instanceExtensions = {};
    
    std::vector<const char *> deviceExtensions = {
            VK_KHR_SWAPCHAIN_EXTENSION_NAME};
    
    const int GPUIndex = 0;
    
    SDL_Window *sdlWindow = nullptr;
};
