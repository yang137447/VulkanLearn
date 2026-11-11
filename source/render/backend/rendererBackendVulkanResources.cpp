#include "render/backend/rendererBackendVulkan.h"

#include <algorithm>
#include <array>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <shared_mutex>
#include <sstream>
#include <stdexcept>
#include <string>
#include <type_traits>
#include <tuple>

#include "pipeline/pipelineFactory.h"
#include "pipeline/graphicsPipelineBuilder.h"
#include "profiler.h"
#include "render/backend/frameEpochRing.h"
#include "render/rhi/vulkan/rhiDeviceVulkan.h"
#include "render/resource/resourceRetireQueue.h"
#include "render/resource/rendererThumbnailImages.h"
#include "vulkanDebug.h"

#include "render/backend/rendererBackendVulkanInternal.h"

namespace VL
{
RHIBufferHandle RendererBackendVulkan::RequireBufferHandle(vk::Buffer buffer) const
{
    std::shared_lock<std::shared_mutex> lock(resourceMapMutex);
    auto handleIt = bufferHandlesByBuffer.find(static_cast<VkBuffer>(buffer));
    if (!buffer || handleIt == bufferHandlesByBuffer.end())
    {
        throw std::runtime_error(BuildUnknownRawHandleMessage("buffer", static_cast<VkBuffer>(buffer)));
    }

    return handleIt->second;
}

RHIBufferHandle RendererBackendVulkan::RequireBufferMemoryHandle(vk::DeviceMemory memory) const
{
    std::shared_lock<std::shared_mutex> lock(resourceMapMutex);
    auto handleIt = bufferHandlesByMemory.find(static_cast<VkDeviceMemory>(memory));
    if (!memory || handleIt == bufferHandlesByMemory.end())
    {
        throw std::runtime_error(BuildUnknownRawHandleMessage(
            "buffer memory",
            static_cast<VkDeviceMemory>(memory)));
    }

    return handleIt->second;
}

void RendererBackendVulkan::BindBufferHandle(RHIBufferHandle bufferHandle)
{
    const VulkanBufferResource bufferResource = rhiDevice->GetVulkanBufferResource(bufferHandle);
    std::unique_lock<std::shared_mutex> lock(resourceMapMutex);
    bufferHandlesByBuffer[static_cast<VkBuffer>(bufferResource.buffer)] = bufferHandle;
    bufferHandlesByMemory[static_cast<VkDeviceMemory>(bufferResource.memory)] = bufferHandle;
}

void RendererBackendVulkan::UnbindBufferHandle(RHIBufferHandle bufferHandle)
{
    const VulkanBufferResource bufferResource = rhiDevice->GetVulkanBufferResource(bufferHandle);
    std::unique_lock<std::shared_mutex> lock(resourceMapMutex);
    bufferHandlesByBuffer.erase(static_cast<VkBuffer>(bufferResource.buffer));
    bufferHandlesByMemory.erase(static_cast<VkDeviceMemory>(bufferResource.memory));
}

std::pair<vk::Buffer, vk::DeviceMemory> RendererBackendVulkan::CreateBuffer(
    vk::DeviceSize size,
    vk::BufferUsageFlags usage,
    vk::MemoryPropertyFlags memoryPropertyFlags,
    const std::string& debugName)
{
    RHIBufferHandle bufferHandle = rhiDevice->CreateBuffer(size, usage, memoryPropertyFlags, debugName);
    BindBufferHandle(bufferHandle);
    const VulkanBufferResource bufferResource = rhiDevice->GetVulkanBufferResource(bufferHandle);
    return { bufferResource.buffer, bufferResource.memory };
}

void* RendererBackendVulkan::MapMemory(vk::DeviceMemory memory, vk::DeviceSize size)
{
    return rhiDevice->MapBufferMemory(RequireBufferMemoryHandle(memory), size);
}

void RendererBackendVulkan::UnmapMemory(vk::DeviceMemory memory)
{
    rhiDevice->UnmapBufferMemory(RequireBufferMemoryHandle(memory));
}

void RendererBackendVulkan::DestroyBuffer(vk::Buffer buffer, vk::DeviceMemory memory)
{
    if (!buffer && !memory)
    {
        return;
    }

    RHIBufferHandle bufferHandle = RequireBufferHandle(buffer);
    UnbindBufferHandle(bufferHandle);
    rhiDevice->DestroyBuffer(bufferHandle);
}

void RendererBackendVulkan::CopyBufferToBuffer(
    vk::Buffer source,
    vk::Buffer destination,
    vk::DeviceSize size)
{
    rhiDevice->CopyBufferToBuffer(
        RequireBufferHandle(source),
        RequireBufferHandle(destination),
        size);
}

void RendererBackendVulkan::CreatePerSwapchainBufferSet(
    Buffer& bufferSet,
    vk::DeviceSize bufferSize,
    vk::BufferUsageFlags usage,
    vk::MemoryPropertyFlags memoryPropertyFlags,
    const std::string& debugNamePrefix,
    bool mapMemory)
{
    if (bufferSize == 0)
    {
        throw std::invalid_argument(
            "RendererBackendVulkan cannot create a zero-sized buffer set: " +
            debugNamePrefix);
    }

    if (!bufferSet.buffers.empty())
    {
        DestroyBufferSet(bufferSet);
    }

    const uint32_t swapchainImageCount = GetSwapchainImageCount();
    bufferSet.bufferHandles.resize(swapchainImageCount);
    bufferSet.buffers.resize(swapchainImageCount);
    bufferSet.bufferMemories.resize(swapchainImageCount);
    bufferSet.buffersMapped.resize(swapchainImageCount);
    bufferSet.bufferSize = static_cast<uint32_t>(bufferSize);

    for (uint32_t i = 0; i < swapchainImageCount; i++)
    {
        RHIBufferHandle bufferHandle = rhiDevice->CreateBuffer(
            bufferSize,
            usage,
            memoryPropertyFlags,
            debugNamePrefix + " (SwapchainIndex " + std::to_string(i) + ")");
        BindBufferHandle(bufferHandle);

        const VulkanBufferResource bufferResource = rhiDevice->GetVulkanBufferResource(bufferHandle);
        bufferSet.bufferHandles[i] = bufferHandle;
        bufferSet.buffers[i] = bufferResource.buffer;
        bufferSet.bufferMemories[i] = bufferResource.memory;
        // device-local 内存不可 map；调用方按用途选择是否保留主机映射。
        bufferSet.buffersMapped[i] = mapMemory
            ? rhiDevice->MapBufferMemory(bufferHandle, bufferSize)
            : nullptr;
    }

    SetupDescriptorBufferInfos(bufferSet);
}

void RendererBackendVulkan::CreateBufferSet(
    Buffer& bufferSet,
    vk::DeviceSize bufferSize,
    vk::BufferUsageFlags usage,
    vk::MemoryPropertyFlags memoryPropertyFlags,
    const std::string& debugName,
    bool mapMemory)
{
    if (bufferSize == 0)
    {
        throw std::invalid_argument(
            "RendererBackendVulkan cannot create a zero-sized buffer set: " +
            debugName);
    }

    if (!bufferSet.buffers.empty())
    {
        DestroyBufferSet(bufferSet);
    }

    RHIBufferHandle bufferHandle = rhiDevice->CreateBuffer(
        bufferSize, usage, memoryPropertyFlags, debugName);
    BindBufferHandle(bufferHandle);
    const VulkanBufferResource bufferResource = rhiDevice->GetVulkanBufferResource(bufferHandle);

    bufferSet.bufferHandles = { bufferHandle };
    bufferSet.buffers = { bufferResource.buffer };
    bufferSet.bufferMemories = { bufferResource.memory };
    bufferSet.buffersMapped = {
        mapMemory ? rhiDevice->MapBufferMemory(bufferHandle, bufferSize) : nullptr };
    bufferSet.bufferSize = static_cast<uint32_t>(bufferSize);
    SetupDescriptorBufferInfos(bufferSet);
}

void RendererBackendVulkan::DestroyBufferSet(Buffer& bufferSet)
{
    const size_t bufferCount = bufferSet.bufferHandles.size() > bufferSet.buffers.size()
        ? bufferSet.bufferHandles.size()
        : bufferSet.buffers.size();
    for (size_t i = 0; i < bufferCount; i++)
    {
        RHIBufferHandle bufferHandle;
        if (i < bufferSet.bufferHandles.size())
        {
            bufferHandle = bufferSet.bufferHandles[i];
        }
        if (!bufferHandle.IsValid() && i < bufferSet.buffers.size() && bufferSet.buffers[i])
        {
            bufferHandle = RequireBufferHandle(bufferSet.buffers[i]);
        }
        if (!bufferHandle.IsValid())
        {
            continue;
        }

        if (i < bufferSet.buffersMapped.size() && bufferSet.buffersMapped[i] != nullptr)
        {
            rhiDevice->UnmapBufferMemory(bufferHandle);
            bufferSet.buffersMapped[i] = nullptr;
        }
        UnbindBufferHandle(bufferHandle);
        rhiDevice->DestroyBuffer(bufferHandle);
    }

    bufferSet.bufferHandles.clear();
    bufferSet.buffers.clear();
    bufferSet.bufferMemories.clear();
    bufferSet.buffersMapped.clear();
    bufferSet.bufferInfos.clear();
    bufferSet.bufferSize = 0;
}

void RendererBackendVulkan::SetupDescriptorBufferInfos(Buffer& bufferSet)
{
    bufferSet.bufferInfos.resize(bufferSet.buffers.size());
    for (size_t i = 0; i < bufferSet.buffers.size(); i++)
    {
        bufferSet.bufferInfos[i]
            .setBuffer(bufferSet.buffers[i])
            .setOffset(0)
            .setRange(bufferSet.bufferSize);
    }
}

RHIImageHandle RendererBackendVulkan::RequireImageHandle(vk::Image image) const
{
    std::shared_lock<std::shared_mutex> lock(resourceMapMutex);
    auto handleIt = imageHandlesByImage.find(static_cast<VkImage>(image));
    if (!image || handleIt == imageHandlesByImage.end())
    {
        throw std::runtime_error(BuildUnknownRawHandleMessage("image", static_cast<VkImage>(image)));
    }

    return handleIt->second;
}

RHIImageHandle RendererBackendVulkan::RequireImageMemoryHandle(vk::DeviceMemory memory) const
{
    std::shared_lock<std::shared_mutex> lock(resourceMapMutex);
    auto handleIt = imageHandlesByMemory.find(static_cast<VkDeviceMemory>(memory));
    if (!memory || handleIt == imageHandlesByMemory.end())
    {
        throw std::runtime_error(BuildUnknownRawHandleMessage(
            "image memory",
            static_cast<VkDeviceMemory>(memory)));
    }

    return handleIt->second;
}

void RendererBackendVulkan::BindImageHandle(RHIImageHandle imageHandle)
{
    const VulkanImageResource imageResource = rhiDevice->GetVulkanImageResource(imageHandle);
    std::unique_lock<std::shared_mutex> lock(resourceMapMutex);
    imageHandlesByImage[static_cast<VkImage>(imageResource.image)] = imageHandle;
    imageHandlesByMemory[static_cast<VkDeviceMemory>(imageResource.memory)] = imageHandle;
}

void RendererBackendVulkan::UnbindImageHandle(RHIImageHandle imageHandle)
{
    const VulkanImageResource imageResource = rhiDevice->GetVulkanImageResource(imageHandle);
    std::unique_lock<std::shared_mutex> lock(resourceMapMutex);
    imageHandlesByImage.erase(static_cast<VkImage>(imageResource.image));
    imageHandlesByMemory.erase(static_cast<VkDeviceMemory>(imageResource.memory));
}

RHIImageViewHandle RendererBackendVulkan::RequireImageViewHandle(vk::ImageView imageView) const
{
    std::shared_lock<std::shared_mutex> lock(resourceMapMutex);
    auto handleIt = imageViewHandlesByImageView.find(static_cast<VkImageView>(imageView));
    if (!imageView || handleIt == imageViewHandlesByImageView.end())
    {
        throw std::runtime_error(BuildUnknownRawHandleMessage(
            "image view",
            static_cast<VkImageView>(imageView)));
    }

    return handleIt->second;
}

void RendererBackendVulkan::BindImageViewHandle(RHIImageViewHandle imageViewHandle)
{
    const VulkanImageViewResource imageViewResource =
        rhiDevice->GetVulkanImageViewResource(imageViewHandle);
    std::unique_lock<std::shared_mutex> lock(resourceMapMutex);
    imageViewHandlesByImageView[static_cast<VkImageView>(imageViewResource.imageView)] = imageViewHandle;
}

void RendererBackendVulkan::UnbindImageViewHandle(RHIImageViewHandle imageViewHandle)
{
    const VulkanImageViewResource imageViewResource =
        rhiDevice->GetVulkanImageViewResource(imageViewHandle);
    std::unique_lock<std::shared_mutex> lock(resourceMapMutex);
    imageViewHandlesByImageView.erase(static_cast<VkImageView>(imageViewResource.imageView));
}

RHISamplerHandle RendererBackendVulkan::RequireSamplerHandle(vk::Sampler sampler) const
{
    std::shared_lock<std::shared_mutex> lock(resourceMapMutex);
    auto handleIt = samplerHandlesBySampler.find(static_cast<VkSampler>(sampler));
    if (!sampler || handleIt == samplerHandlesBySampler.end())
    {
        throw std::runtime_error(BuildUnknownRawHandleMessage("sampler", static_cast<VkSampler>(sampler)));
    }

    return handleIt->second;
}

void RendererBackendVulkan::BindSamplerHandle(RHISamplerHandle samplerHandle)
{
    const VulkanSamplerResource samplerResource = rhiDevice->GetVulkanSamplerResource(samplerHandle);
    std::unique_lock<std::shared_mutex> lock(resourceMapMutex);
    samplerHandlesBySampler[static_cast<VkSampler>(samplerResource.sampler)] = samplerHandle;
}

void RendererBackendVulkan::UnbindSamplerHandle(RHISamplerHandle samplerHandle)
{
    const VulkanSamplerResource samplerResource = rhiDevice->GetVulkanSamplerResource(samplerHandle);
    std::unique_lock<std::shared_mutex> lock(resourceMapMutex);
    samplerHandlesBySampler.erase(static_cast<VkSampler>(samplerResource.sampler));
}

std::pair<vk::Image, vk::DeviceMemory> RendererBackendVulkan::CreateImage(
    uint32_t width,
    uint32_t height,
    uint32_t mipLevels,
    vk::SampleCountFlagBits samples,
    vk::Format format,
    vk::ImageTiling tiling,
    vk::ImageUsageFlags usage,
    vk::MemoryPropertyFlags memoryPropertyFlags,
    const std::string& debugName)
{
    RHIImageHandle imageHandle = rhiDevice->CreateImage(
        width,
        height,
        mipLevels,
        samples,
        format,
        tiling,
        usage,
        memoryPropertyFlags,
        debugName);
    BindImageHandle(imageHandle);
    const VulkanImageResource imageResource = rhiDevice->GetVulkanImageResource(imageHandle);
    {
        std::unique_lock<std::shared_mutex> lock(resourceMapMutex);
        imageDebugNamesByImage[static_cast<VkImage>(imageResource.image)] =
            debugName;
        imageExtentsByImage[static_cast<VkImage>(imageResource.image)] =
            vk::Extent2D{ width, height };
    }
    return { imageResource.image, imageResource.memory };
}

std::pair<vk::Image, vk::DeviceMemory> RendererBackendVulkan::CreateImage(
    const vk::ImageCreateInfo& createInfo,
    vk::MemoryPropertyFlags memoryPropertyFlags,
    const std::string& debugName)
{
    RHIImageHandle imageHandle = rhiDevice->CreateImage(createInfo, memoryPropertyFlags, debugName);
    BindImageHandle(imageHandle);
    const VulkanImageResource imageResource = rhiDevice->GetVulkanImageResource(imageHandle);
    {
        std::unique_lock<std::shared_mutex> lock(resourceMapMutex);
        imageDebugNamesByImage[static_cast<VkImage>(imageResource.image)] =
            debugName;
        imageExtentsByImage[static_cast<VkImage>(imageResource.image)] =
            vk::Extent2D{
                createInfo.extent.width,
                createInfo.extent.height };
    }
    return { imageResource.image, imageResource.memory };
}

vk::Extent2D RendererBackendVulkan::GetImageExtent(vk::Image image) const
{
    std::shared_lock<std::shared_mutex> lock(resourceMapMutex);
    const auto found =
        imageExtentsByImage.find(static_cast<VkImage>(image));
    return found != imageExtentsByImage.end()
        ? found->second
        : vk::Extent2D{0, 0};
}

void RendererBackendVulkan::TransitionImageLayout(
    vk::Image image,
    uint32_t mipLevels,
    vk::Format format,
    vk::ImageLayout oldLayout,
    vk::ImageLayout newLayout,
    uint32_t baseArrayLayer,
    uint32_t layerCount)
{
    rhiDevice->TransitionImageLayout(
        RequireImageHandle(image),
        mipLevels,
        format,
        oldLayout,
        newLayout,
        baseArrayLayer,
        layerCount);
}

vk::ImageView RendererBackendVulkan::Create2DImageView(
    vk::Image image,
    uint32_t mipLevels,
    vk::Format format,
    vk::ImageAspectFlagBits aspectMask,
    const std::string& debugName)
{
    RHIImageViewHandle imageViewHandle =
        rhiDevice->Create2DImageView(RequireImageHandle(image), mipLevels, format, aspectMask, debugName);
    BindImageViewHandle(imageViewHandle);
    const vk::ImageView imageView =
        rhiDevice->GetVulkanImageViewResource(imageViewHandle).imageView;
    {
        std::unique_lock<std::shared_mutex> lock(resourceMapMutex);
        imageViewDebugNamesByImageView[static_cast<VkImageView>(imageView)] =
            debugName;
    }
    return imageView;
}

vk::ImageView RendererBackendVulkan::CreateImageView(
    vk::Image image,
    vk::ImageViewType viewType,
    vk::Format format,
    vk::ImageAspectFlagBits aspectMask,
    uint32_t baseMipLevel,
    uint32_t mipLevelCount,
    uint32_t baseArrayLayer,
    uint32_t layerCount,
    const std::string& debugName)
{
    RHIImageViewHandle imageViewHandle = rhiDevice->CreateImageView(
        RequireImageHandle(image),
        viewType,
        format,
        aspectMask,
        baseMipLevel,
        mipLevelCount,
        baseArrayLayer,
        layerCount,
        debugName);
    BindImageViewHandle(imageViewHandle);
    const vk::ImageView imageView =
        rhiDevice->GetVulkanImageViewResource(imageViewHandle).imageView;
    {
        std::unique_lock<std::shared_mutex> lock(resourceMapMutex);
        imageViewDebugNamesByImageView[static_cast<VkImageView>(imageView)] =
            debugName;
    }
    return imageView;
}

vk::ImageView RendererBackendVulkan::CreateCubeImageView(
    vk::Image image,
    uint32_t mipLevels,
    vk::Format format,
    const std::string& debugName)
{
    RHIImageViewHandle imageViewHandle =
        rhiDevice->CreateCubeImageView(RequireImageHandle(image), mipLevels, format, debugName);
    BindImageViewHandle(imageViewHandle);
    const vk::ImageView imageView =
        rhiDevice->GetVulkanImageViewResource(imageViewHandle).imageView;
    {
        std::unique_lock<std::shared_mutex> lock(resourceMapMutex);
        imageViewDebugNamesByImageView[static_cast<VkImageView>(imageView)] =
            debugName;
    }
    return imageView;
}

vk::ImageView RendererBackendVulkan::CreateCubeStorageImageView(
    vk::Image image,
    vk::Format format,
    const std::string& debugName)
{
    RHIImageViewHandle imageViewHandle =
        rhiDevice->CreateCubeStorageImageView(RequireImageHandle(image), format, debugName);
    BindImageViewHandle(imageViewHandle);
    const vk::ImageView imageView =
        rhiDevice->GetVulkanImageViewResource(imageViewHandle).imageView;
    {
        std::unique_lock<std::shared_mutex> lock(resourceMapMutex);
        imageViewDebugNamesByImageView[static_cast<VkImageView>(imageView)] =
            debugName;
    }
    return imageView;
}

void RendererBackendVulkan::DestroyImageView(vk::ImageView& imageView)
{
    if (!imageView)
    {
        return;
    }

    RHIImageViewHandle imageViewHandle = RequireImageViewHandle(imageView);
    {
        std::unique_lock<std::shared_mutex> lock(resourceMapMutex);
        imageViewDebugNamesByImageView.erase(
            static_cast<VkImageView>(imageView));
    }
    UnbindImageViewHandle(imageViewHandle);
    rhiDevice->DestroyImageView(imageViewHandle);
    imageView = nullptr;
}

vk::Sampler RendererBackendVulkan::Create2DSampler(const std::string& debugName)
{
    RHISamplerHandle samplerHandle = rhiDevice->Create2DSampler(debugName);
    BindSamplerHandle(samplerHandle);
    const vk::Sampler sampler =
        rhiDevice->GetVulkanSamplerResource(samplerHandle).sampler;
    {
        std::unique_lock<std::shared_mutex> lock(resourceMapMutex);
        samplerDebugNamesBySampler[static_cast<VkSampler>(sampler)] =
            debugName;
    }
    return sampler;
}

vk::Sampler RendererBackendVulkan::Create2DSampler(
    vk::Filter filter,
    vk::SamplerAddressMode addressMode,
    bool enableMipmaps,
    const std::string& debugName)
{
    RHISamplerHandle samplerHandle =
        rhiDevice->Create2DSampler(filter, addressMode, enableMipmaps, debugName);
    BindSamplerHandle(samplerHandle);
    const vk::Sampler sampler =
        rhiDevice->GetVulkanSamplerResource(samplerHandle).sampler;
    {
        std::unique_lock<std::shared_mutex> lock(resourceMapMutex);
        samplerDebugNamesBySampler[static_cast<VkSampler>(sampler)] =
            debugName;
    }
    return sampler;
}

vk::Sampler RendererBackendVulkan::CreateSampler(
    const vk::SamplerCreateInfo& createInfo,
    const std::string& debugName)
{
    RHISamplerHandle samplerHandle = rhiDevice->CreateSampler(createInfo, debugName);
    BindSamplerHandle(samplerHandle);
    const vk::Sampler sampler =
        rhiDevice->GetVulkanSamplerResource(samplerHandle).sampler;
    {
        std::unique_lock<std::shared_mutex> lock(resourceMapMutex);
        samplerDebugNamesBySampler[static_cast<VkSampler>(sampler)] =
            debugName;
    }
    return sampler;
}

vk::Sampler RendererBackendVulkan::CreateCubeSampler(
    float maxLod,
    const std::string& debugName)
{
    RHISamplerHandle samplerHandle = rhiDevice->CreateCubeSampler(maxLod, debugName);
    BindSamplerHandle(samplerHandle);
    const vk::Sampler sampler =
        rhiDevice->GetVulkanSamplerResource(samplerHandle).sampler;
    {
        std::unique_lock<std::shared_mutex> lock(resourceMapMutex);
        samplerDebugNamesBySampler[static_cast<VkSampler>(sampler)] =
            debugName;
    }
    return sampler;
}

void RendererBackendVulkan::DestroySampler(vk::Sampler& sampler)
{
    if (!sampler)
    {
        return;
    }

    RHISamplerHandle samplerHandle = RequireSamplerHandle(sampler);
    {
        std::unique_lock<std::shared_mutex> lock(resourceMapMutex);
        samplerDebugNamesBySampler.erase(
            static_cast<VkSampler>(sampler));
    }
    UnbindSamplerHandle(samplerHandle);
    rhiDevice->DestroySampler(samplerHandle);
    sampler = nullptr;
}

vk::Sampler RendererBackendVulkan::CreateDepthSampler(const std::string& debugName)
{
    RHISamplerHandle samplerHandle = rhiDevice->CreateDepthSampler(debugName);
    BindSamplerHandle(samplerHandle);
    const vk::Sampler sampler =
        rhiDevice->GetVulkanSamplerResource(samplerHandle).sampler;
    {
        std::unique_lock<std::shared_mutex> lock(resourceMapMutex);
        samplerDebugNamesBySampler[static_cast<VkSampler>(sampler)] =
            debugName;
    }
    return sampler;
}

vk::Sampler RendererBackendVulkan::CreateDepthCompareSampler(const std::string& debugName)
{
    RHISamplerHandle samplerHandle = rhiDevice->CreateDepthCompareSampler(debugName);
    BindSamplerHandle(samplerHandle);
    const vk::Sampler sampler =
        rhiDevice->GetVulkanSamplerResource(samplerHandle).sampler;
    {
        std::unique_lock<std::shared_mutex> lock(resourceMapMutex);
        samplerDebugNamesBySampler[static_cast<VkSampler>(sampler)] =
            debugName;
    }
    return sampler;
}

vk::CommandBuffer RendererBackendVulkan::BeginSingleTimeCommands()
{
    return rhiDevice->BeginSingleTimeCommands();
}

void RendererBackendVulkan::EndSingleTimeCommands(vk::CommandBuffer& commandBuffer)
{
    rhiDevice->EndSingleTimeCommands(commandBuffer);
}

void RendererBackendVulkan::AbortSingleTimeCommands(vk::CommandBuffer& commandBuffer)
{
    rhiDevice->AbortSingleTimeCommands(commandBuffer);
}

void RendererBackendVulkan::CopyBufferToImage(
    vk::Buffer buffer,
    vk::Image image,
    uint32_t width,
    uint32_t height,
    uint32_t baseArrayLayer,
    uint32_t layerCount,
    vk::DeviceSize layerBytes,
    uint32_t bufferRowLength)
{
    rhiDevice->CopyBufferToImage(
        RequireBufferHandle(buffer),
        RequireImageHandle(image),
        width,
        height,
        baseArrayLayer,
        layerCount,
        layerBytes,
        bufferRowLength);
}

void RendererBackendVulkan::CopyBufferToImageRegion(
    vk::Buffer buffer,
    vk::Image image,
    vk::DeviceSize bufferOffset,
    uint32_t bufferRowLength,
    uint32_t imageOffsetX,
    uint32_t imageOffsetY,
    uint32_t width,
    uint32_t height,
    uint32_t mipLevel,
    uint32_t baseArrayLayer,
    uint32_t layerCount,
    vk::DeviceSize layerBytes)
{
    rhiDevice->CopyBufferToImageRegion(
        RequireBufferHandle(buffer),
        RequireImageHandle(image),
        bufferOffset,
        bufferRowLength,
        imageOffsetX,
        imageOffsetY,
        width,
        height,
        mipLevel,
        baseArrayLayer,
        layerCount,
        layerBytes);
}

void RendererBackendVulkan::CopyBufferToImageRegions(
    vk::Buffer buffer,
    vk::Image image,
    const std::vector<vk::BufferImageCopy>& regions)
{
    rhiDevice->CopyBufferToImageRegions(
        RequireBufferHandle(buffer),
        RequireImageHandle(image),
        regions);
}

void RendererBackendVulkan::RecordTransitionImageLayout(
    vk::CommandBuffer commandBuffer,
    vk::Image image,
    uint32_t mipLevels,
    vk::Format format,
    vk::ImageLayout oldLayout,
    vk::ImageLayout newLayout,
    uint32_t baseArrayLayer,
    uint32_t layerCount)
{
    rhiDevice->RecordTransitionImageLayout(
        commandBuffer,
        RequireImageHandle(image),
        mipLevels,
        format,
        oldLayout,
        newLayout,
        baseArrayLayer,
        layerCount);
}

void RendererBackendVulkan::RecordCopyBufferToImageRegions(
    vk::CommandBuffer commandBuffer,
    vk::Buffer buffer,
    vk::Image image,
    const std::vector<vk::BufferImageCopy>& regions)
{
    rhiDevice->RecordCopyBufferToImageRegions(
        commandBuffer,
        RequireBufferHandle(buffer),
        RequireImageHandle(image),
        regions);
}

void RendererBackendVulkan::RecordClearColorImage(
    vk::CommandBuffer commandBuffer,
    vk::Image image,
    vk::ImageLayout layout,
    vk::ClearColorValue clearValue,
    uint32_t baseArrayLayer,
    uint32_t layerCount)
{
    rhiDevice->RecordClearColorImage(
        commandBuffer,
        RequireImageHandle(image),
        layout,
        clearValue,
        baseArrayLayer,
        layerCount);
}

void RendererBackendVulkan::RecordCopyImageRegions(
    vk::CommandBuffer commandBuffer,
    vk::Image source,
    vk::Image destination,
    const std::vector<vk::ImageCopy>& regions)
{
    rhiDevice->RecordCopyImageRegions(
        commandBuffer,
        RequireImageHandle(source),
        RequireImageHandle(destination),
        regions);
}

void RendererBackendVulkan::CopyImageRegions(
    vk::Image source,
    vk::Image destination,
    const std::vector<vk::ImageCopy>& regions)
{
    rhiDevice->CopyImageRegions(
        RequireImageHandle(source),
        RequireImageHandle(destination),
        regions);
}

void RendererBackendVulkan::RecordCopyImageToBuffer(
    vk::CommandBuffer commandBuffer,
    vk::Image image,
    vk::Buffer buffer,
    uint32_t width,
    uint32_t height,
    bool flipY,
    vk::DeviceSize rowBytes)
{
    rhiDevice->RecordCopyImageToBuffer(
        commandBuffer,
        RequireImageHandle(image),
        RequireBufferHandle(buffer),
        width,
        height,
        flipY,
        rowBytes);
}

void RendererBackendVulkan::CopyImageToBuffer(
    vk::Image image,
    vk::Buffer buffer,
    uint32_t width,
    uint32_t height,
    bool flipY,
    vk::DeviceSize rowBytes)
{
    rhiDevice->CopyImageToBuffer(
        RequireImageHandle(image),
        RequireBufferHandle(buffer),
        width,
        height,
        flipY,
        rowBytes);
}

void RendererBackendVulkan::GenerateMipmaps(
    vk::Image image,
    uint32_t width,
    uint32_t height,
    uint32_t mipLevels,
    uint32_t baseArrayLayer,
    uint32_t layerCount)
{
    rhiDevice->GenerateMipmaps(
        RequireImageHandle(image),
        width,
        height,
        mipLevels,
        baseArrayLayer,
        layerCount);
}

RHIImageHandle RendererBackendVulkan::GetImageHandle(vk::Image image) const
{
    return RequireImageHandle(image);
}

RHIImageViewHandle RendererBackendVulkan::GetImageViewHandle(vk::ImageView imageView) const
{
    return RequireImageViewHandle(imageView);
}

RHISamplerHandle RendererBackendVulkan::GetSamplerHandle(vk::Sampler sampler) const
{
    return RequireSamplerHandle(sampler);
}

void RendererBackendVulkan::DestroyImageResource(
    vk::Image& image,
    vk::DeviceMemory& memory,
    vk::ImageView& imageView,
    vk::Sampler& sampler)
{
    if (!image && !memory)
    {
        DestroyImageView(imageView);
        DestroySampler(sampler);
        return;
    }

    RHIImageHandle imageHandle = RequireImageHandle(image);
    RHIImageViewHandle imageViewHandle;
    RHISamplerHandle samplerHandle;
    if (imageView)
    {
        imageViewHandle = RequireImageViewHandle(imageView);
        {
            std::unique_lock<std::shared_mutex> lock(resourceMapMutex);
            imageViewDebugNamesByImageView.erase(
                static_cast<VkImageView>(imageView));
        }
        UnbindImageViewHandle(imageViewHandle);
    }
    if (sampler)
    {
        samplerHandle = RequireSamplerHandle(sampler);
        {
            std::unique_lock<std::shared_mutex> lock(resourceMapMutex);
            samplerDebugNamesBySampler.erase(
                static_cast<VkSampler>(sampler));
        }
        UnbindSamplerHandle(samplerHandle);
    }
    {
        std::unique_lock<std::shared_mutex> lock(resourceMapMutex);
        imageDebugNamesByImage.erase(
            static_cast<VkImage>(image));
        imageExtentsByImage.erase(
            static_cast<VkImage>(image));
    }
    UnbindImageHandle(imageHandle);
    rhiDevice->DestroyImageResource(imageHandle, imageViewHandle, samplerHandle);
    image = nullptr;
    memory = nullptr;
    imageView = nullptr;
    sampler = nullptr;
}

void RendererBackendVulkan::DestroyImageResource(
    RHIImageHandle& imageHandle,
    RHIImageViewHandle& imageViewHandle,
    RHISamplerHandle& samplerHandle)
{
    if (!imageHandle.IsValid())
    {
        if (imageViewHandle.IsValid())
        {
            const vk::ImageView imageView =
                rhiDevice->GetVulkanImageViewResource(
                    imageViewHandle).imageView;
            {
                std::unique_lock<std::shared_mutex> lock(resourceMapMutex);
                imageViewDebugNamesByImageView.erase(
                    static_cast<VkImageView>(imageView));
            }
            UnbindImageViewHandle(imageViewHandle);
            rhiDevice->DestroyImageView(imageViewHandle);
            imageViewHandle = RHIImageViewHandle();
        }
        if (samplerHandle.IsValid())
        {
            const vk::Sampler sampler =
                rhiDevice->GetVulkanSamplerResource(
                    samplerHandle).sampler;
            {
                std::unique_lock<std::shared_mutex> lock(resourceMapMutex);
                samplerDebugNamesBySampler.erase(
                    static_cast<VkSampler>(sampler));
            }
            UnbindSamplerHandle(samplerHandle);
            rhiDevice->DestroySampler(samplerHandle);
            samplerHandle = RHISamplerHandle();
        }
        return;
    }

    if (imageViewHandle.IsValid())
    {
        const vk::ImageView imageView =
            rhiDevice->GetVulkanImageViewResource(
                imageViewHandle).imageView;
        {
            std::unique_lock<std::shared_mutex> lock(resourceMapMutex);
            imageViewDebugNamesByImageView.erase(
                static_cast<VkImageView>(imageView));
        }
        UnbindImageViewHandle(imageViewHandle);
    }
    if (samplerHandle.IsValid())
    {
        const vk::Sampler sampler =
            rhiDevice->GetVulkanSamplerResource(
                samplerHandle).sampler;
        {
            std::unique_lock<std::shared_mutex> lock(resourceMapMutex);
            samplerDebugNamesBySampler.erase(
                static_cast<VkSampler>(sampler));
        }
        UnbindSamplerHandle(samplerHandle);
    }
    const vk::Image image =
        rhiDevice->GetVulkanImageResource(
            imageHandle).image;
    {
        std::unique_lock<std::shared_mutex> lock(resourceMapMutex);
        imageDebugNamesByImage.erase(
            static_cast<VkImage>(image));
        imageExtentsByImage.erase(
            static_cast<VkImage>(image));
    }
    UnbindImageHandle(imageHandle);
    rhiDevice->DestroyImageResource(imageHandle, imageViewHandle, samplerHandle);

    imageHandle = RHIImageHandle();
    imageViewHandle = RHIImageViewHandle();
    samplerHandle = RHISamplerHandle();
}

} // namespace VL
