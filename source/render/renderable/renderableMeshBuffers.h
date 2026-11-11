#pragma once

#include <cstddef>
#include <string>
#include <utility>

#include <vulkan/vulkan.hpp>

namespace VL
{
class RendererBackendVulkan;

namespace RenderableDetail
{
// Creates a device-local storage buffer and uploads `size` bytes once through a
// host-visible staging buffer. Shared by mesh and TressFX buffer creation; the
// transfer uses a single blocking copy because this runs at object creation.
std::pair<vk::Buffer, vk::DeviceMemory> CreateUploadedDeviceLocalStorageBuffer(
    RendererBackendVulkan& backend,
    const void* data,
    vk::DeviceSize size,
    const std::string& debugName);
} // namespace RenderableDetail
} // namespace VL