#pragma once

#include <cstdint>
#include <optional>
#include <vector>

#include <vulkan/vulkan.hpp>

struct SDL_Window;

namespace VL
{

// 一个"呈现目标"= 一个窗口那套与呈现相关的 Vulkan 状态。
//
// 存在的理由：这些状态原本全部直接挂在 `VulkanManager` 上，于是 `VulkanManager` 同时是
// "设备所有者"和"唯一那个窗口的呈现状态所有者"，两者被绑死。多窗口需要的是
// **共享设备 + 每窗口一套 surface/swapchain**（Blender 的形态：每窗口一个 context，
// 共享同一份设备与资源），因此把呈现状态独立出来，让"哪个窗口"成为一个可传入的身份。
//
// 归属划分（依据 `VulkanManager::CreateSyncObjects` 的分配方式）：
//   - `renderFinishedSemaphores` / `imagesInFlightFences` / 命令缓冲按 **swapchain 图像数**分配，
//     属于每目标；
//   - 而 `imageAcquiredSemaphores` / `taskFinishedFences` 按 `MAX_FRAMES_IN_FLIGHT` 分配，
//     是帧级、跨目标可共享，因此留在 `VulkanManager`。
//
// 本类型只描述状态，不持有设备：创建/销毁需要 `vk::Device` 与物理设备，由 `VulkanManager`
// 传入，避免这里再出现第二个设备来源。
struct PresentTarget
{
    SDL_Window* window = nullptr;

    // surface 用裸句柄：SDL_Vulkan_CreateSurface 直接写它，且它是 C API 的产物。
    VkSurfaceKHR surface = VK_NULL_HANDLE;
    vk::SurfaceFormatKHR surfaceFormat;
    vk::Extent2D extent;
    // 由 ReCreateSwapChain 写入：surface 的 currentExtent 未定义（宽度 0xFFFFFFFF）时，
    // 用它替代窗口尺寸作为请求尺寸。
    std::optional<vk::Extent2D> requestedExtent;
    uint32_t imageCount = 0;
    vk::SwapchainKHR swapChain;
    std::vector<vk::Image> images;
    std::vector<vk::ImageView> imageViews;

    // 按图像数分配，因此每目标一份。
    // 注意这里**没有** commandBuffers：命令缓冲目前仍由 `VulkanManager` 持有并按图像数分配，
    // 而它与命令池的销毁语义绑在一起（`DestroyVkCommandBuffer` 会销毁共享池，
    // `BeginSingleTimeCommands` 也在用那个池），所以不能单独搬过来。
    // 曾经在本结构里留过一个空的 commandBuffers 字段，无人写入 —— 已删除，避免出现第二个"真相"。
    std::vector<vk::Semaphore> renderFinishedSemaphores;
    std::vector<vk::Fence> imagesInFlightFences;

    // 呈现队列族可能与其他目标不同：每个 surface 各自解析（当前只对目标 0 解析过）。
    std::optional<uint32_t> presentQueueFamilyIndex;
};

} // namespace VL
