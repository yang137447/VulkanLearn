#pragma once

#include <memory>
#include <vulkan/vulkan.hpp>

namespace VL
{
void RegisterRenderGraphDepthOwner(vk::Image image, std::shared_ptr<void> owner);
std::shared_ptr<const void> CaptureRenderGraphDepthOwner(vk::Image image);
std::shared_ptr<void> ReleaseRenderGraphDepthOwner(vk::Image image);
}
