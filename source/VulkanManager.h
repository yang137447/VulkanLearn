#pragma once

#include <cstdint>
#include <vulkan/vulkan.hpp>
#include <SDL3/SDL.h>
#include <vector>
#include <optional>

#include <Eigen/Dense>

class DrawableObject;
class RenderPipline;

class VulkanManager
{
public:
    static VulkanManager& GetInstance()
    {
        static VulkanManager inst;
        return inst;
    }

    void Init(std::vector<const char *> &extensions, SDL_Window *window);
    ~VulkanManager();

    void ReCreateSwapChain(int newWidth, int newHeight);

    inline vk::Device& GetDevice() { return device; }
    inline vk::PhysicalDeviceMemoryProperties& GetGpuMemoryProperties() { return gpuMemoryProperties; }
    inline vk::CommandPool& GetCommandPool() { return commandPool; }
    inline std::vector<vk::CommandBuffer>& GetCommandBuffers() { return commandBuffers; }
    inline vk::Queue& GetGraphicQueue() { return graphicQueue; }
    inline vk::RenderPass& GetRenderPass() { return renderPass; }
    inline vk::SampleCountFlagBits GetSampleCount() { return sampleCount; }
    inline std::vector<vk::Fence>& GetTaskFinishedFences() { return taskFinishedFences; }
    inline vk::SwapchainKHR& GetSwapChain() { return swapChain; }
    inline std::vector<vk::Semaphore>& GetImageAcquiredSemaphores() { return imageAcquiredSemaphores; }
    inline std::vector<vk::Semaphore>& GetRenderFinishedSemaphores() { return renderFinishedSemaphores; }
    inline vk::RenderPassBeginInfo& GetRenderPassBeginInfo() { return renderPassBeginInfo; }
    inline std::vector<vk::Framebuffer>& GetFrameBuffers() { return framebuffers; }
    inline vk::CommandBufferBeginInfo& GetCommandBufferBeginInfo() { return commandBufferBeginInfo; }
    inline uint32_t GetSwapChainImageCount() { return swapChainImageCount; }
private:
    VulkanManager();

    void CreateVkInstance();
    void DestroyVkInstance();

    void EnumeratePhysicalDevices();

    void CreateVkSurface();
    void DestroyVkSurface();

    void CreateVkDevice();
    void DestroyVkDevice();

    void CreateVkSwapChain();
    void DestroyVkSwapChain();

    void CreateVkCommandBuffer();
    void DestroyVkCommandBuffer();

    void CreateColorResource();
    void DestroyColorResource();

    void CreateVkDepthBuffer();
    void DestroyVkDepthBuffer();

    void CreateVkRenderPass();
    void DestroyVkRenderPass();

    void CreateVkFrameBuffers();
    void DestroyVkFrameBuffers();

    void CreateSyncObjects();
    void DestroySyncObjects();

    void InitInstance();

private:
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
    std::optional<uint32_t> graphicQueueFamilyIndex;
    vk::Queue graphicQueue;
    std::optional<uint32_t> presentQueueFamilyIndex;
    vk::Queue presentQueue;

    vk::SampleCountFlagBits sampleCount;

    vk::Device device;

    vk::CommandPool commandPool;
    vk::CommandBufferBeginInfo commandBufferBeginInfo;
    std::vector<vk::CommandBuffer> commandBuffers;
    std::array<vk::SubmitInfo, 1> submitInfo;

    VkSurfaceKHR surface;
    std::vector<vk::SurfaceFormatKHR> surfaceFormats;
    vk::SurfaceFormatKHR surfaceFormat;
    vk::SurfaceCapabilitiesKHR surfaceCapabilities;
    std::vector<vk::PresentModeKHR> presentModes;
    vk::Extent2D swapChainExtent;
    uint32_t swapChainImageCount = 0;
    vk::SwapchainKHR swapChain;
    std::vector<vk::Image> swapChainImages;
    std::vector<vk::ImageView> swapChainImageViews;

    vk::Format depthFormat;
    vk::FormatProperties depthFormatProperties;
    vk::Image depthImage;
    vk::PhysicalDeviceMemoryProperties depthImageMemoryProperties;
    vk::DeviceMemory depthImageMemory;
    vk::ImageView depthImageView;

    vk::Image colorImage;
    vk::DeviceMemory colorImageMemory;
    vk::ImageView colorImageView;

    std::vector<vk::Semaphore> imageAcquiredSemaphores;
    std::vector<vk::Semaphore> renderFinishedSemaphores;
    uint32_t swapchainImageIndex = 0;
    vk::RenderPass renderPass;
    std::vector<vk::ClearValue> clearValues;
    vk::RenderPassBeginInfo renderPassBeginInfo;

    std::vector<vk::Framebuffer> framebuffers;

    std::vector<vk::Fence> taskFinishedFences;
};
