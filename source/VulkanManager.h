#pragma once

#include <cstdint>
#include <vulkan/vulkan.hpp>
#include <SDL3/SDL.h>
#include <vector>
#include <optional>

#include <Eigen/Dense>

class DrawableObject;
class GraphicsPipeline;

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

    // Legacy swapchain recreate entry kept behind RendererBackendVulkan/RHI.
    // The engine-level resize path now passes the target drawable size in, so
    // callers should not infer it from stale SDL or swapchain extents here.
    void ReCreateSwapChain(int newWidth, int newHeight);

    inline vk::Device& GetDevice() { return device; }
    inline vk::PhysicalDevice& GetPhysicalDevice() { return physicalDevices[GPUIndex]; }
    inline vk::PhysicalDeviceMemoryProperties& GetGpuMemoryProperties() { return gpuMemoryProperties; }
    inline vk::CommandPool& GetCommandPool() { return commandPool; }
    inline std::vector<vk::CommandBuffer>& GetCommandBuffers() { return commandBuffers; }
    inline vk::Queue& GetGraphicQueue() { return graphicQueue; }
    //inline vk::SampleCountFlagBits GetSampleCount() { return sampleCount; }
    inline std::vector<vk::Fence>& GetTaskFinishedFences() { return taskFinishedFences; }
    inline vk::SwapchainKHR& GetSwapChain() { return swapChain; }
    inline std::vector<vk::Semaphore>& GetImageAcquiredSemaphores() { return imageAcquiredSemaphores; }
    inline std::vector<vk::Semaphore>& GetRenderFinishedSemaphores() { return renderFinishedSemaphores; }
    inline std::vector<vk::Fence>& GetImagesInFlightFences() { return imagesInFlightFences; }
    inline vk::CommandBufferBeginInfo& GetCommandBufferBeginInfo() { return commandBufferBeginInfo; }
    inline uint32_t GetSwapChainImageCount() { return swapChainImageCount; }
    inline vk::SurfaceFormatKHR& GetSurfaceFormat() { return surfaceFormat; }
    inline std::vector<vk::ImageView>& GetSwapChainImageViews() { return swapChainImageViews; }
    inline vk::Extent2D GetSwapChainExtent() const { return swapChainExtent; }
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

    void CreateSyncObjects();
    void DestroySyncObjects();

    void InitInstance();

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
    std::optional<vk::Extent2D> requestedSwapChainExtent;
    uint32_t swapChainImageCount = 0;
    vk::SwapchainKHR swapChain;
    std::vector<vk::Image> swapChainImages;
    std::vector<vk::ImageView> swapChainImageViews;

    std::vector<vk::Semaphore> imageAcquiredSemaphores;
    std::vector<vk::Semaphore> renderFinishedSemaphores;

    std::vector<vk::Fence> taskFinishedFences;
    std::vector<vk::Fence> imagesInFlightFences;
};
