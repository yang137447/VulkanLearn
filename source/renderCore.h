#pragma once

#include <memory>
#include <vector>
#include "vulkan/vulkan.hpp"
#include "SDL3/SDL.h"
#include "BaseStructs.h"

class Swapchain;
class ShaderModule;
class RenderProcess;
class Render;

class RenderCore
{
public:
    RenderCore(std::vector<const char *> &extensions, SDL_Window *window);
    ~RenderCore();
    static void Init(std::vector<const char *> &extensions, SDL_Window *window);

    void draw();

    void CreateVkInstace();
    void DestroyVkInstance();
    void PickupPhysicalDevice();
    void CreateVkSurface();
    void DestroyVkSurface();
    void CreateVkDevice();
    void DestroyVkDevice();
    void CreateVkSwapchain();
    void DestroyVkSwapchain();
    void CreateVkRenderPass();
    void DestroyVkRenderPass();
    void CreateVkFramebuffers();
    void DestroyVkFramebuffers();
    void CreateVkShaderModule();
    void DestroyVkShaderModule();
    void CreateVkRenderProcess();
    void DestroyVkRenderProcess();
    void CreateVkRender();
    void DestroyVkRender();

    vk::Instance instance;
    vk::PhysicalDevice physicalDevice;
    vk::SurfaceKHR surface;
    vk::Device device;
    QueueFamilyIndices queueFamilyIndices;
    vk::Queue graphicQueue;
    vk::Queue presentQueue;
    vk::RenderPass renderPass;
    Swapchain *swapchain;
    std::vector<vk::Framebuffer> framebuffers;
    ShaderModule *shaderModule;
    RenderProcess *renderProcess;
    Render *render;

private:
    RenderCore();
    // data
    inline static std::vector<const char *> instanceLayers = {
        "VK_LAYER_KHRONOS_validation"};

    inline static std::vector<const char *> instanceExtensions = {};

    inline static std::vector<const char *> deviceExtensions = {
        VK_KHR_SWAPCHAIN_EXTENSION_NAME};

    const int GPUIndex = 0;

    inline static SDL_Window *sdlWindow = nullptr;

    // function
    std::optional<uint32_t> QueryQueueFamilyIndices(vk::QueueFlagBits queryQueueflagbits = vk::QueueFlagBits::eGraphics);
    std::optional<uint32_t> QueryQueueFamilyIndices(bool findPresentQueue = true);
};