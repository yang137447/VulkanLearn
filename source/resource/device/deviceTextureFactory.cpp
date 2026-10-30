#include "deviceTextureFactory.h"

#include "../../commonFunction.h"
#include "../image/hostImageVulkanPlan.h"
#include "render/backend/rendererBackendVulkan.h"

#include <cmath>
#include <cstring>
#include <limits>
#include <stdexcept>

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
    image.ValidateStorage();

    const bool blockCompressed = HostImage::IsBlockCompressed(image.format);
    if (blockCompressed && options.generateMipmapsOnDevice)
    {
        // 块格式的 mip 必须离线编码；Texture 在调用本函数前会把该标志清掉。
        throw std::invalid_argument(
            "Block-compressed textures cannot generate mips on the device; "
            "provide an offline mip chain in the container");
    }

    vk::Format format = HostImageVulkan::ToVulkanFormat(image.format);
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

    // 容器自带 mip 链时直接使用；只有单 mip 的非块格式才在设备端生成。
    const bool generateMipmaps =
        !blockCompressed && image.mipLevels == 1 && options.generateMipmapsOnDevice;
    uint32_t mipLevels = image.mipLevels;
    if (generateMipmaps)
    {
        mipLevels = static_cast<uint32_t>(std::floor(std::log2(std::max(image.width, image.height)))) + 1;
    }

    // TransferSrc 始终保留：层 atlas 需要在 GPU 内从源层纹理切页，
    // 而源图在创建时无法预知是否会被 L2 消费。该 flag 不额外分配显存。
    vk::ImageUsageFlags usageFlags =
        vk::ImageUsageFlagBits::eTransferDst |
        vk::ImageUsageFlagBits::eTransferSrc |
        vk::ImageUsageFlagBits::eSampled;

    vk::ImageTiling tiling = vk::ImageTiling::eOptimal;
    vk::MemoryPropertyFlags imageMemoryFlags = vk::MemoryPropertyFlagBits::eDeviceLocal;
    vk::ImageCreateInfo imageCreateInfo;
    imageCreateInfo
        .setImageType(vk::ImageType::e2D)
        .setExtent(vk::Extent3D{ image.width, image.height, 1 })
        .setMipLevels(mipLevels)
        .setArrayLayers(image.arrayLayers)
        .setSamples(vk::SampleCountFlagBits::e1)
        .setFormat(format)
        .setTiling(tiling)
        .setUsage(usageFlags)
        .setSharingMode(vk::SharingMode::eExclusive)
        .setInitialLayout(vk::ImageLayout::eUndefined);
    auto [deviceImage, deviceImageMemory] = rendererBackend.CreateImage(
        imageCreateInfo,
        imageMemoryFlags,
        "Texture: " + debugName);

    rendererBackend.TransitionImageLayout(
        deviceImage,
        mipLevels,
        format,
        vk::ImageLayout::eUndefined,
        vk::ImageLayout::eTransferDstOptimal,
        0,
        image.arrayLayers);
    // 逐 mip/逐层 region 共享同一个 staging buffer；块格式的 extent 是纹素尺寸，
    // 行跨度由驱动按 4×4 块推导。
    rendererBackend.CopyBufferToImageRegions(
        stagingBuffer,
        deviceImage,
        HostImageVulkan::BuildUploadRegions(image));
    if (generateMipmaps)
    {
        rendererBackend.GenerateMipmaps(
            deviceImage,
            image.width,
            image.height,
            mipLevels,
            0,
            image.arrayLayers);
    }
    else
    {
        rendererBackend.TransitionImageLayout(
            deviceImage,
            mipLevels,
            format,
            vk::ImageLayout::eTransferDstOptimal,
            vk::ImageLayout::eShaderReadOnlyOptimal,
            0,
            image.arrayLayers);
    }

    rendererBackend.DestroyBuffer(stagingBuffer, stagingMemory);

    DeviceTextureResource textureResource;
    textureResource.imageHandle = rendererBackend.GetImageHandle(deviceImage);
    textureResource.image = deviceImage;
    textureResource.imageMemory = deviceImageMemory;
    textureResource.mipLevels = mipLevels;
    textureResource.arrayLayers = image.arrayLayers;
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
