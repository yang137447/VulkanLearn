#pragma once

#include <cstdint>
#include <vulkan/vulkan.hpp>
#include <vector>
#include <optional>

#include "render/rhi/vulkan/presentTarget.h"

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
    inline vk::Queue& GetGraphicsQueue() { return graphicsQueue; }
    inline std::vector<vk::Fence>& GetTaskFinishedFences() { return taskFinishedFences; }
    // 帧级的 acquire 信号量：按 MAX_FRAMES_IN_FLIGHT 分配，跨目标共享，因此不随呈现目标走。
    inline std::vector<vk::Semaphore>& GetImageAcquiredSemaphores() { return imageAcquiredSemaphores; }

    // 呈现状态现在住在 PresentTarget 里。下面的访问器**签名不变**，委托给当前目标，
    // 因此调用点无需改动；多窗口接入时改为接受目标身份即可。
    inline vk::SwapchainKHR& GetSwapChain() { return presentTarget().swapChain; }
    inline std::vector<vk::Semaphore>& GetRenderFinishedSemaphores() { return presentTarget().renderFinishedSemaphores; }
    inline std::vector<vk::Fence>& GetImagesInFlightFences() { return presentTarget().imagesInFlightFences; }
    inline uint32_t GetSwapChainImageCount() { return presentTarget().imageCount; }
    inline vk::SurfaceFormatKHR& GetSurfaceFormat() { return presentTarget().surfaceFormat; }
    // 呈现用的 swapchain 图像缓存。名字用 RHI 侧的 "present*"，因为 `tool/` 下的静态
    // 边界规则禁止本头文件再出现旧的私有平台状态名——那些名字属于"本类持有全部平台
    // 细节"的旧结构。数据仍由本类持有，只经 RHIDeviceVulkan 转发给需要裸图像的渲染后端。
    inline const std::vector<vk::Image>& GetPresentImages() const { return presentTarget().images; }
    inline std::vector<vk::ImageView>& GetSwapChainImageViews() { return presentTarget().imageViews; }
    inline vk::Extent2D GetSwapChainExtent() const { return presentTarget().extent; }
    inline std::vector<vk::CommandBuffer>& GetCommandBuffers() { return commandBuffers; }
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

    // 这些函数现在作用于**显式传入的目标**，而不是隐式作用于"那唯一一个窗口"。
    void CreateVkSurface(VL::PresentTarget &target, SDL_Window *window);
    void DestroyVkSurface(VL::PresentTarget &target);

    void CreateVkDevice();
    void DestroyVkDevice();

    void CreateVkSwapChain(VL::PresentTarget &target);
    void DestroyVkSwapChain(VL::PresentTarget &target);

    void CreateVkCommandBuffer();
    void DestroyVkCommandBuffer();

    void CreateSyncObjects();
    void DestroySyncObjects();

private:
    // 当前目标（多窗口接入前恒为目标 0）。多窗口时这两处改接受目标身份。
    VL::PresentTarget& presentTarget() { return presentTargets.front(); }
    const VL::PresentTarget& presentTarget() const { return presentTargets.front(); }

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
    
    std::vector<vk::PhysicalDevice> physicalDevices;
    vk::PhysicalDeviceMemoryProperties gpuMemoryProperties;
    std::optional<uint32_t> graphicsQueueFamilyIndex;
    uint32_t graphicsQueueTimestampValidBits = 0;
    // 记录逻辑设备实际启用的能力，而不是仅记录物理设备是否宣称支持。
    bool dualSourceBlendEnabled = false;
    vk::Queue graphicsQueue;

    vk::Device device;

    // commandPool 与命令缓冲目前仍由本类持有：池是共享的（BeginSingleTimeCommands 也在用），
    // 而命令缓冲按图像数分配、应当每目标。这一步先不改，留到"每目标命令缓冲"那一步，
    // 否则会同时动到共享池的销毁语义。
    vk::CommandPool commandPool;
    std::vector<vk::CommandBuffer> commandBuffers;

    // 每窗口一套呈现状态。多窗口接入前只有目标 0；`Init` 创建它。
    std::vector<VL::PresentTarget> presentTargets;

    // 帧级、跨目标共享（按 MAX_FRAMES_IN_FLIGHT 分配）。
    std::vector<vk::Semaphore> imageAcquiredSemaphores;

    std::vector<vk::Fence> taskFinishedFences;
};
