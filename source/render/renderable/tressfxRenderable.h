#pragma once

#include <vulkan/vulkan.hpp>

namespace VL
{
// One TressFX buffer plus its mapped pointer and descriptor range. The owning
// RenderableObject keeps the authoritative arrays; this type only describes one
// slot so renderableObject.h and the TressFX slices agree on layout.
struct TressfxBufferTarget
{
    vk::Buffer buffer = nullptr;
    vk::DeviceMemory memory = nullptr;
    void* mapped = nullptr;
    vk::DescriptorBufferInfo info;
};
} // namespace VL