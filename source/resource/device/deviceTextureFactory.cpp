#include "deviceTextureFactory.h"

#include "../../commonFunction.h"
#include "../image/hostImageVulkanPlan.h"
#include "render/backend/rendererBackendVulkan.h"

#include <cmath>
#include <cstring>
#include <limits>
#include <stdexcept>

DeviceTextureAllocationRequirements DeviceTextureFactory::QueryAllocationRequirements(
    VL::RendererBackendVulkan& rendererBackend, const HostImage& image,
    const DeviceTextureCreateOptions& options)
{
    if (!image.data.empty())
        image.ValidateStorage();
    const auto info = ImageCreateInfo(image, options);
    return {
        rendererBackend.QueryImageMemoryRequirements(info),
        rendererBackend.QueryBufferMemoryRequirements(
            image.GetTotalBytes(), vk::BufferUsageFlagBits::eTransferSrc)};
}

DeviceTextureResource DeviceTextureFactory::CreateResourceFromHostImage(
    VL::RendererBackendVulkan& rendererBackend,
    const HostImage& image,
    const std::string& debugName,
    const DeviceTextureCreateOptions& options)
{
    if (image.width == 0 || image.height == 0 || image.data.empty())
        throw std::runtime_error("Invalid HostImage for DeviceTextureFactory::CreateFromHostImage");
    image.ValidateStorage();
    const auto imageCreateInfo = ImageCreateInfo(image, options);
    const auto format = imageCreateInfo.format;
    const auto mipLevels = imageCreateInfo.mipLevels;
    const bool generateMipmaps = !HostImage::IsBlockCompressed(image.format) &&
        image.mipLevels == 1 && options.generateMipmapsOnDevice;
    const vk::DeviceSize imageSize = static_cast<vk::DeviceSize>(image.data.size());
    const auto stagingAllocation = rendererBackend.CreateBuffer(imageSize,
        vk::BufferUsageFlagBits::eTransferSrc,
        vk::MemoryPropertyFlagBits::eHostVisible | vk::MemoryPropertyFlagBits::eHostCoherent,
        "StagingBuffer_Texture: " + debugName);
    const auto stagingBuffer = stagingAllocation.first;
    const auto stagingMemory = stagingAllocation.second;
    void* mapped = rendererBackend.MapMemory(stagingMemory, imageSize);
    std::memcpy(mapped, image.data.data(), static_cast<size_t>(imageSize));
    rendererBackend.UnmapMemory(stagingMemory);
    auto [deviceImage, deviceImageMemory] = rendererBackend.CreateImage(
        imageCreateInfo,
        vk::MemoryPropertyFlagBits::eDeviceLocal,
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
