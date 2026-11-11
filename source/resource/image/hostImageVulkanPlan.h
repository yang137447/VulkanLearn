#pragma once

#include "hostImage.h"

#include <cstdint>
#include <stdexcept>
#include <vector>
#include <vulkan/vulkan.hpp>

// HostImage 到 Vulkan 的纯数据映射：格式选择与逐 mip/逐层 copy region。
// 不创建任何 GPU 资源，供 DeviceTextureFactory 与模块测试共用同一套块布局规则。
namespace HostImageVulkan
{
    inline vk::Format ToVulkanFormat(HostImage::PixelFormat format)
    {
        switch (format)
        {
        case HostImage::PixelFormat::RGBA8_UNORM:
            return vk::Format::eR8G8B8A8Unorm;
        case HostImage::PixelFormat::RGBA8_SRGB:
            return vk::Format::eR8G8B8A8Srgb;
        case HostImage::PixelFormat::RGBA16_FLOAT:
            return vk::Format::eR16G16B16A16Sfloat;
        case HostImage::PixelFormat::RG16_FLOAT:
            return vk::Format::eR16G16Sfloat;
        case HostImage::PixelFormat::R16_FLOAT:
            return vk::Format::eR16Sfloat;
        case HostImage::PixelFormat::R16_UNORM:
            return vk::Format::eR16Unorm;
        case HostImage::PixelFormat::RGBA32_FLOAT:
            return vk::Format::eR32G32B32A32Sfloat;
        // BC 块格式：sRGB 与 UNORM 复用同一组 128-bit 块，仅采样解码不同。
        case HostImage::PixelFormat::BC7_SRGB:
            return vk::Format::eBc7SrgbBlock;
        case HostImage::PixelFormat::BC7_UNORM:
            return vk::Format::eBc7UnormBlock;
        case HostImage::PixelFormat::BC5_UNORM:
            return vk::Format::eBc5UnormBlock;
        default:
            throw std::runtime_error("Unsupported HostImage::PixelFormat for Vulkan upload");
        }
    }

    // 每个 (mip, array layer) 一个 region。bufferOffset 直接用 HostImage 的
    // layer-major / mip-major 偏移；extent 是被拷贝 mip 的纹素尺寸，
    // 块格式的行跨度由驱动按 extent 推导（bufferRowLength = 0 表示紧凑块行）。
    inline std::vector<vk::BufferImageCopy> BuildUploadRegions(
        const HostImage& image,
        vk::DeviceSize bufferBase = 0)
    {
        image.ValidateStorage();
        std::vector<vk::BufferImageCopy> regions;
        regions.reserve(
            static_cast<size_t>(image.mipLevels) * static_cast<size_t>(image.arrayLayers));

        const bool blockCompressed = HostImage::IsBlockCompressed(image.format);
        for (uint32_t layer = 0; layer < image.arrayLayers; ++layer)
        {
            for (uint32_t mip = 0; mip < image.mipLevels; ++mip)
            {
                const uint32_t mipWidth = image.GetMipWidth(mip);
                const uint32_t mipHeight = image.GetMipHeight(mip);
                const uint32_t rowStride = image.GetMipRowStride(mip);
                const uint32_t bytesPerElement =
                    blockCompressed
                        ? HostImage::TexelBlockBytes
                        : HostImage::BytesPerTexelOrBlock(image.format);
                // 非块格式的填充行需要用 texel 数表达跨度；块格式必须紧凑，传 0。
                const uint32_t tightRowStride =
                    blockCompressed ? rowStride : mipWidth * bytesPerElement;
                const uint32_t bufferRowLength =
                    rowStride == tightRowStride ? 0u : rowStride / bytesPerElement;

                vk::BufferImageCopy region;
                region
                    .setBufferOffset(bufferBase + image.GetMipOffset(mip, layer))
                    .setBufferRowLength(bufferRowLength)
                    .setBufferImageHeight(0)
                    .setImageSubresource(vk::ImageSubresourceLayers()
                        .setAspectMask(vk::ImageAspectFlagBits::eColor)
                        .setMipLevel(mip)
                        .setBaseArrayLayer(layer)
                        .setLayerCount(1))
                    .setImageOffset(vk::Offset3D{ 0, 0, 0 })
                    .setImageExtent(vk::Extent3D{ mipWidth, mipHeight, 1 });
                regions.push_back(region);
            }
        }
        return regions;
    }
}
