#pragma once

#include "../image/hostImage.h"
#include "../image/hostImageVulkanPlan.h"

#include "render/rhi/rhiResourceHandles.h"

#include <string>
#include <tuple>
#include <algorithm>
#include <cmath>
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
    uint32_t arrayLayers = 1;
    vk::Format format = vk::Format::eUndefined;
};

// 查询与实际上传使用同一 image create info；staging 仍按原始 CPU 字节分配。
struct DeviceTextureAllocationRequirements
{
    vk::MemoryRequirements image;
    vk::MemoryRequirements staging;
};

class DeviceTextureFactory
{
public:
    static vk::ImageCreateInfo ImageCreateInfo(
        const HostImage& image, const DeviceTextureCreateOptions& options = {});
    static DeviceTextureAllocationRequirements QueryAllocationRequirements(
        VL::RendererBackendVulkan& rendererBackend,
        const HostImage& image, const DeviceTextureCreateOptions& options = {});
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

inline vk::ImageCreateInfo DeviceTextureFactory::ImageCreateInfo(
    const HostImage& image, const DeviceTextureCreateOptions& options)
{
    if (image.width == 0 || image.height == 0 || image.arrayLayers == 0 || image.mipLevels == 0)
        throw std::invalid_argument("Texture image plan requires nonzero dimensions/layers/mips");
    const bool blockCompressed = HostImage::IsBlockCompressed(image.format);
    if (blockCompressed && options.generateMipmapsOnDevice)
        throw std::invalid_argument("Block-compressed textures require an offline mip chain");
    // 与实际上传共用完整描述；查询不生成像素，也不分配 device memory。
    uint32_t mipLevels = image.mipLevels;
    if (!blockCompressed && mipLevels == 1 && options.generateMipmapsOnDevice)
        mipLevels = static_cast<uint32_t>(std::floor(std::log2(std::max(image.width, image.height)))) + 1;
    vk::ImageCreateInfo info;
    info.setImageType(vk::ImageType::e2D).setExtent({image.width, image.height, 1})
        .setMipLevels(mipLevels).setArrayLayers(image.arrayLayers)
        .setSamples(vk::SampleCountFlagBits::e1).setFormat(HostImageVulkan::ToVulkanFormat(image.format))
        .setTiling(vk::ImageTiling::eOptimal)
        .setUsage(vk::ImageUsageFlagBits::eTransferDst | vk::ImageUsageFlagBits::eTransferSrc |
            vk::ImageUsageFlagBits::eSampled)
        .setSharingMode(vk::SharingMode::eExclusive).setInitialLayout(vk::ImageLayout::eUndefined);
    return info;
}
