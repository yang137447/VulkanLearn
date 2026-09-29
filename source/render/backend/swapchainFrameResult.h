#pragma once

#include <stdexcept>

#include <vulkan/vulkan.hpp>

namespace VL
{

enum class SwapchainFrameResult
{
    Ready,
    Suboptimal,
    OutOfDate
};

inline SwapchainFrameResult ClassifySwapchainFrameResult(vk::Result result)
{
    switch (result)
    {
    case vk::Result::eSuccess:
        return SwapchainFrameResult::Ready;
    case vk::Result::eSuboptimalKHR:
        return SwapchainFrameResult::Suboptimal;
    case vk::Result::eErrorOutOfDateKHR:
        return SwapchainFrameResult::OutOfDate;
    default:
        throw std::runtime_error(
            "Swapchain operation failed: " + vk::to_string(result));
    }
}

inline bool RequiresSwapchainRecreate(SwapchainFrameResult result)
{
    return result != SwapchainFrameResult::Ready;
}

inline bool HasAcquiredSwapchainImage(SwapchainFrameResult result)
{
    return result != SwapchainFrameResult::OutOfDate;
}

} // namespace VL
