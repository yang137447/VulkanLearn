#pragma once

#include <functional>
#include <vulkan/vulkan.hpp>
#include "render/backend/frameAbandon.h"
#include "render/backend/swapchainFrameResult.h"

namespace VL
{
struct FrameAbandonVulkanSync
{
    vk::Device device;
    vk::CommandBuffer command;
    vk::Image image;
    vk::Semaphore acquired;
    vk::Semaphore finished;
    vk::Fence fence;
    vk::ImageView imageView;
    vk::Format format = vk::Format::eUndefined;
    vk::Extent2D extent;
};

using FrameAbandonSubmit = std::function<void(const vk::SubmitInfo&, vk::Fence)>;
using FrameAbandonPresent = std::function<SwapchainFrameResult(vk::Semaphore)>;

// 共享 backend/harness 的恢复实现；只清安全底色并呈现，不提交已废弃的场景命令。
SwapchainFrameResult RecoverAcquiredFrame(
    const FrameAbandonVulkanSync& sync, AcquiredFrameDisposition& disposition,
    const FrameAbandonSubmit& submit, const FrameAbandonPresent& present);
}
