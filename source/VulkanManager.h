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

    // 呈现目标的身份。用**引用**而不是索引：调用点需要的是"哪个目标"，而不是"容器的第几项"，
    // 索引会把容器形状泄漏到每个使用处。容器仍由本类独占拥有，且只在启动时增长，
    // 因此这里的引用在目标生命周期内稳定。
    VL::PresentTarget& GetPresentTarget(size_t index) { return presentTargets.at(index); }
    const VL::PresentTarget& GetPresentTarget(size_t index) const { return presentTargets.at(index); }
    size_t PresentTargetCount() const { return presentTargets.size(); }

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

    // 呈现状态住在 PresentTarget 里，且**没有隐式默认目标**：本类不再提供
    // "取那个 swapchain"之类的无参访问器，因为那正是多窗口要消除的东西
    // （一个全局单例装作是唯一目标）。需要呈现状态的调用点必须经 GetPresentTarget(身份) 取。
    // 设备级状态仍直接提供，因为它们本来就跨目标共享。
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
    // 目标 0 是主窗口。它的存在是"多窗口尚未接线"这一事实，所以只在这里出现一次，
    // 而不是像以前那样藏在每个访问器里。多窗口接线后，这一处改为按窗口解析。
    VL::PresentTarget& defaultTarget() { return presentTargets.front(); }

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
