#include "deviceTextureFactory.h"

#include "../../commonFunction.h"
#include "../../vulkanManager.h"

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

std::tuple<vk::Image, vk::DeviceMemory, uint32_t, vk::Format> DeviceTextureFactory::CreateFromHostImage(
    const HostImage& image,
    const std::string& debugName,
    const DeviceTextureCreateOptions& options)
{
    if (image.width == 0 || image.height == 0 || image.data.empty())
    {
        throw std::runtime_error("Invalid HostImage for DeviceTextureFactory::CreateFromHostImage");
    }

    auto& vulkanManager = VulkanManager::GetInstance();
    auto& device = vulkanManager.GetDevice();
    auto& memoryProperties = vulkanManager.GetGpuMemoryProperties();
    auto& commandPool = vulkanManager.GetCommandPool();
    auto& graphicsQueue = vulkanManager.GetGraphicQueue();

    vk::Format format = ToVkFormat(image.format);
    const vk::DeviceSize imageSize = static_cast<vk::DeviceSize>(image.data.size());
    vk::DeviceSize stagingSize = imageSize;
    vk::BufferUsageFlags stagingUsage = vk::BufferUsageFlagBits::eTransferSrc;
    vk::MemoryPropertyFlags stagingMemFlags = vk::MemoryPropertyFlagBits::eHostVisible | vk::MemoryPropertyFlagBits::eHostCoherent;
    auto [stagingBuffer, stagingMemory] = CommonFunction::CreateBuffer(
        device,
        stagingSize,
        stagingUsage,
        memoryProperties,
        stagingMemFlags,
        "StagingBuffer_Texture: " + debugName);

    void* mapped = device.mapMemory(stagingMemory, 0, imageSize);
    std::memcpy(mapped, image.data.data(), static_cast<size_t>(imageSize));
    device.unmapMemory(stagingMemory);

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
    auto [deviceImage, deviceImageMemory] = CommonFunction::CreateImage(
        device,
        image.width,
        image.height,
        mipLevels,
        vk::SampleCountFlagBits::e1,
        format,
        tiling,
        usageFlags,
        memoryProperties,
        imageMemoryFlags,
        "Texture: " + debugName);

    CommonFunction::TransitionImageLayout(
        deviceImage,
        mipLevels,
        format,
        device,
        commandPool,
        graphicsQueue,
        vk::ImageLayout::eUndefined,
        vk::ImageLayout::eTransferDstOptimal);
    CommonFunction::CopyBufferToImage(device, graphicsQueue, commandPool, stagingBuffer, deviceImage, image.width, image.height);
    if (options.generateMipmapsOnDevice)
    {
        CommonFunction::GenerateMipmaps(device, graphicsQueue, commandPool, deviceImage, image.width, image.height, mipLevels);
    }
    else
    {
        CommonFunction::TransitionImageLayout(
            deviceImage,
            mipLevels,
            format,
            device,
            commandPool,
            graphicsQueue,
            vk::ImageLayout::eTransferDstOptimal,
            vk::ImageLayout::eShaderReadOnlyOptimal);
    }

    device.destroyBuffer(stagingBuffer);
    device.freeMemory(stagingMemory);

    return { deviceImage, deviceImageMemory, mipLevels, format };
}
