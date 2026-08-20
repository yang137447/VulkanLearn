#pragma once

#include <cstdint>
#include <vulkan/vulkan.hpp>
#include <vector>
#include <optional>

struct SDL_Window;

namespace VL
{
class RHIDeviceVulkan;
}

class VulkanManager
{
public:
    static VulkanManager& GetInstance()
    {
        static VulkanManager inst;
        return inst;
    }

private:
    friend class VL::RHIDeviceVulkan;

    VulkanManager() = default;
    ~VulkanManager();

    void Init(std::vector<const char *> &extensions, SDL_Window *window);

    // Swapchain recreate entry owned by RendererBackendVulkan through the
    // Vulkan device boundary. The engine-level resize path passes the target drawable size in, so
    // callers should not infer it from stale SDL or swapchain extents here.
    void ReCreateSwapChain(int newWidth, int newHeight);

    inline vk::Device& GetDevice() { return device; }
    inline vk::PhysicalDevice& GetPhysicalDevice() { return physicalDevices[kGpuIndex]; }
    inline vk::PhysicalDeviceMemoryProperties& GetGpuMemoryProperties() { return gpuMemoryProperties; }
    inline vk::CommandPool& GetCommandPool() { return commandPool; }
    inline std::vector<vk::CommandBuffer>& GetCommandBuffers() { return commandBuffers; }
    inline vk::Queue& GetGraphicsQueue() { return graphicsQueue; }
    inline std::vector<vk::Fence>& GetTaskFinishedFences() { return taskFinishedFences; }
    inline vk::SwapchainKHR& GetSwapChain() { return swapChain; }
    inline std::vector<vk::Semaphore>& GetImageAcquiredSemaphores() { return imageAcquiredSemaphores; }
    inline std::vector<vk::Semaphore>& GetRenderFinishedSemaphores() { return renderFinishedSemaphores; }
    inline std::vector<vk::Fence>& GetImagesInFlightFences() { return imagesInFlightFences; }
    inline uint32_t GetSwapChainImageCount() { return swapChainImageCount; }
    inline vk::SurfaceFormatKHR& GetSurfaceFormat() { return surfaceFormat; }
    inline std::vector<vk::ImageView>& GetSwapChainImageViews() { return swapChainImageViews; }
    inline vk::Extent2D GetSwapChainExtent() const { return swapChainExtent; }
    inline uint32_t GetGraphicsQueueTimestampValidBits() const
    {
        return graphicsQueueTimestampValidBits;
    }
    inline bool IsDualSourceBlendEnabled() const
    {
        return dualSourceBlendEnabled;
    }

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

private:
    vk::Instance instance;
    std::vector<const char *> instanceLayers = {
#if defined(VULKANLEARN_ENABLE_VULKAN_VALIDATION)
            "VK_LAYER_KHRONOS_validation"
#endif
    };
    std::vector<const char *> instanceExtensions = {};
    std::vector<const char *> deviceExtensions = {
            VK_KHR_SWAPCHAIN_EXTENSION_NAME};
    
    static constexpr uint32_t kGpuIndex = 0;
    SDL_Window *sdlWindow = nullptr;
    
    std::vector<vk::PhysicalDevice> physicalDevices;
    vk::PhysicalDeviceMemoryProperties gpuMemoryProperties;
    std::optional<uint32_t> graphicsQueueFamilyIndex;
    uint32_t graphicsQueueTimestampValidBits = 0;
    // 记录逻辑设备实际启用的能力，而不是仅记录物理设备是否宣称支持。
    bool dualSourceBlendEnabled = false;
    vk::Queue graphicsQueue;
    std::optional<uint32_t> presentQueueFamilyIndex;

    vk::Device device;

    vk::CommandPool commandPool;
    std::vector<vk::CommandBuffer> commandBuffers;

    VkSurfaceKHR surface;
    vk::SurfaceFormatKHR surfaceFormat;
    vk::Extent2D swapChainExtent;
    std::optional<vk::Extent2D> requestedSwapChainExtent;
    uint32_t swapChainImageCount = 0;
    vk::SwapchainKHR swapChain;
    std::vector<vk::ImageView> swapChainImageViews;

    std::vector<vk::Semaphore> imageAcquiredSemaphores;
    std::vector<vk::Semaphore> renderFinishedSemaphores;

    std::vector<vk::Fence> taskFinishedFences;
    std::vector<vk::Fence> imagesInFlightFences;
};
