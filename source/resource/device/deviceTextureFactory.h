#pragma once

#include "../image/hostImage.h"

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

class DeviceTextureFactory
{
public:
    static std::tuple<vk::Image, vk::DeviceMemory, uint32_t, vk::Format> CreateFromHostImage(
        VL::RendererBackendVulkan& rendererBackend,
        const HostImage& image,
        const std::string& debugName,
        const DeviceTextureCreateOptions& options = {});
};
