#pragma once

#include <memory>
#include <vector>
#include "vulkan/vulkan.hpp"
#include "SDL3/SDL.h"
#include "BaseStructs.h"
#include "swapchain.h"

class RenderCore
{
public:
    RenderCore(std::vector<const char *> &extensions, SDL_Window *window);
    ~RenderCore();
    static void Init(std::vector<const char *> &extensions, SDL_Window *window);

    void CreateVkInstace();
    void DestroyVkInstance();
    void PickupPhysicalDevice();
    void CreateVkSurface();
    void CreateVkDevice();
    void DestroyVkDevice();
    void CreateVkSwapchain();
    void DestroyVkSwapchain();

    vk::Instance instance;
    vk::PhysicalDevice physicalDevice;
    vk::SurfaceKHR surface;
    vk::Device device;
    QueueFamilyIndices queueFamilyIndices;
    vk::Queue graphicQueue;
    vk::Queue presentQueue;
    std::unique_ptr<Swapchain> swapchain;

private:
    RenderCore();
    // data
    inline static std::vector<const char *> instanceLayers = {
        "VK_LAYER_KHRONOS_validation"};

    const int GPUIndex = 0;

    inline static std::vector<const char *> sdlExtensoins = {};

    inline static SDL_Window *sdlWindow = nullptr;

    // function
    std::optional<uint32_t> QueryQueueFamilyIndices(vk::QueueFlagBits queryQueueflagbits = vk::QueueFlagBits::eGraphics);
    std::optional<uint32_t> QueryQueueFamilyIndices(bool findPresentQueue = true);
};