#include "deviceTextureFactory.h"

#include "../../commonFunction.h"
#include "render/backend/rendererBackendVulkan.h"

#include <cmath>
#include <cstring>
#include <stdexcept>

namespace
{
    vk::Format ToVkFormat(HostImage::PixelFormat format)
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
        case HostImage::PixelFormat::RGBA32_FLOAT:
            return vk::Format::eR32G32B32A32Sfloat;
        default:
            throw std::runtime_error("Unsupported HostImage::PixelFormat for Vulkan upload");
        }
    }
}

DeviceTextureResource DeviceTextureFactory::CreateResourceFromHostImage(
    VL::RendererBackendVulkan& rendererBackend,
    const HostImage& image,
    const std::string& debugName,
    const DeviceTextureCreateOptions& options)
{
    if (image.width == 0 || image.height == 0 || image.data.empty())
    {
        throw std::runtime_error("Invalid HostImage for DeviceTextureFactory::CreateFromHostImage");
    }

    vk::Format format = ToVkFormat(image.format);
    const vk::DeviceSize imageSize = static_cast<vk::DeviceSize>(image.data.size());
    vk::DeviceSize stagingSize = imageSize;
    vk::BufferUsageFlags stagingUsage = vk::BufferUsageFlagBits::eTransferSrc;
    vk::MemoryPropertyFlags stagingMemFlags = vk::MemoryPropertyFlagBits::eHostVisible | vk::MemoryPropertyFlagBits::eHostCoherent;
    auto [stagingBuffer, stagingMemory] = rendererBackend.CreateBuffer(
        stagingSize,
        stagingUsage,
        stagingMemFlags,
        "StagingBuffer_Texture: " + debugName);

    void* mapped = rendererBackend.MapMemory(stagingMemory, imageSize);
    std::memcpy(mapped, image.data.data(), static_cast<size_t>(imageSize));
    rendererBackend.UnmapMemory(stagingMemory);

    uint32_t mipLevels = 1;
    if (options.generateMipmapsOnDevice)
    {
        mipLevels = static_cast<uint32_t>(std::floor(std::log2(std::max(image.width, image.height)))) + 1;
    }

    vk::ImageUsageFlags usageFlags = vk::ImageUsageFlagBits::eTransferDst | vk::ImageUsageFlagBits::eSampled;
    if (options.generateMipmapsOnDevice)
    {
        usageFlags |= vk::ImageUsageFlagBits::eTransferSrc;
    }

    vk::ImageTiling tiling = vk::ImageTiling::eOptimal;
    vk::MemoryPropertyFlags imageMemoryFlags = vk::MemoryPropertyFlagBits::eDeviceLocal;
    auto [deviceImage, deviceImageMemory] = rendererBackend.CreateImage(
        image.width,
        image.height,
        mipLevels,
        vk::SampleCountFlagBits::e1,
        format,
        tiling,
        usageFlags,
        imageMemoryFlags,
        "Texture: " + debugName);

    rendererBackend.TransitionImageLayout(
        deviceImage,
        mipLevels,
        format,
        vk::ImageLayout::eUndefined,
        vk::ImageLayout::eTransferDstOptimal);
    rendererBackend.CopyBufferToImage(stagingBuffer, deviceImage, image.width, image.height);
    if (options.generateMipmapsOnDevice)
    {
        rendererBackend.GenerateMipmaps(deviceImage, image.width, image.height, mipLevels);
    }
    else
    {
        rendererBackend.TransitionImageLayout(
            deviceImage,
            mipLevels,
            format,
            vk::ImageLayout::eTransferDstOptimal,
            vk::ImageLayout::eShaderReadOnlyOptimal);
    }

    rendererBackend.DestroyBuffer(stagingBuffer, stagingMemory);

    DeviceTextureResource textureResource;
    textureResource.imageHandle = rendererBackend.GetImageHandle(deviceImage);
    textureResource.image = deviceImage;
    textureResource.imageMemory = deviceImageMemory;
    textureResource.mipLevels = mipLevels;
    textureResource.format = format;
    return textureResource;
}

std::tuple<vk::Image, vk::DeviceMemory, uint32_t, vk::Format> DeviceTextureFactory::CreateFromHostImage(
    VL::RendererBackendVulkan& rendererBackend,
    const HostImage& image,
    const std::string& debugName,
    const DeviceTextureCreateOptions& options)
{
    DeviceTextureResource textureResource =
        CreateResourceFromHostImage(rendererBackend, image, debugName, options);
    return {
        textureResource.image,
        textureResource.imageMemory,
        textureResource.mipLevels,
        textureResource.format
    };
}
