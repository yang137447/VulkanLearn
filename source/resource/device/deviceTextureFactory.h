#pragma once

#include "../image/hostImage.h"

#include "render/rhi/rhiResourceHandles.h"

#include <string>
#include <tuple>
#include <vulkan/vulkan.hpp>

namespace VL
{
class RendererBackendVulkan;
}

struct DeviceTextureCreateOptions
{
    HostImage::TextureSemantic semantic = HostImage::TextureSemantic::Color;
    bool generateMipmapsOnDevice = true;
    bool createSampler = true;
};

struct DeviceTextureResource
{
    VL::RHIImageHandle imageHandle;
    vk::Image image;
    vk::DeviceMemory imageMemory;
    uint32_t mipLevels = 1;
    vk::Format format = vk::Format::eUndefined;
};

class DeviceTextureFactory
{
public:
    static DeviceTextureResource CreateResourceFromHostImage(
        VL::RendererBackendVulkan& rendererBackend,
        const HostImage& image,
        const std::string& debugName,
        const DeviceTextureCreateOptions& options = {});

    static std::tuple<vk::Image, vk::DeviceMemory, uint32_t, vk::Format> CreateFromHostImage(
        VL::RendererBackendVulkan& rendererBackend,
        const HostImage& image,
        const std::string& debugName,
        const DeviceTextureCreateOptions& options = {});
};
