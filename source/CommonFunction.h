#pragma once

#include "vulkan/vulkan.hpp"
#include <cstdint>
#include <vulkan/vulkan_enums.hpp>
#include <vulkan/vulkan_handles.hpp>
#include <Eigen/Dense>
#include <array>
#include <vulkan/vulkan_structs.hpp>
#include "settings.h"

struct UniformBufferObject
{
    Eigen::Matrix4f model;
    Eigen::Matrix4f view;
    Eigen::Matrix4f projection;
};

namespace CommonFunction
{
    inline std::string Path(const std::string& path)
    {
        return filePath + "/"+ path;
    }

    inline uint32_t FindMemoryType(vk::PhysicalDeviceMemoryProperties& physicalDeviceMemoryProperties, uint32_t typeFilter, vk::MemoryPropertyFlags& memoryPropertyFlags)
    {
        for (uint32_t i = 0; i < physicalDeviceMemoryProperties.memoryTypeCount; i++)
        {
            if ((typeFilter & (1 << i)) && (physicalDeviceMemoryProperties.memoryTypes[i].propertyFlags & memoryPropertyFlags) == memoryPropertyFlags)
            {
                return i;
            }
        }
        throw std::runtime_error("Failed to find suitable memory type!");
    }

    inline vk::Format FindSupportedFormat(vk::PhysicalDevice& physicalDevice, const std::vector<vk::Format>& candidates, vk::ImageTiling tiling, vk::FormatFeatureFlags features)
    {
        for (const auto& format : candidates) {
            vk::FormatProperties props = physicalDevice.getFormatProperties(format);
            if (tiling == vk::ImageTiling::eLinear && (props.linearTilingFeatures & features) == features) 
            {
                return format;
            }
            else if (tiling == vk::ImageTiling::eOptimal && (props.optimalTilingFeatures & features) == features) 
            {
                return format;
            }
        }

        return vk::Format::eUndefined;
    }

    inline vk::Format FindDepthFormat(vk::PhysicalDevice& physicalDevice)
    {
        return FindSupportedFormat(physicalDevice,
            { vk::Format::eD32Sfloat, vk::Format::eD32SfloatS8Uint, vk::Format::eD24UnormS8Uint },
            vk::ImageTiling::eOptimal,
            vk::FormatFeatureFlagBits::eDepthStencilAttachment);
    }

    inline bool HasStencilComponent(vk::Format format) {
        return format == vk::Format::eD32SfloatS8Uint || format == vk::Format::eD24UnormS8Uint;
    }

    inline vk::CommandBuffer BeginSingleTimeCommands(vk::Device& device, vk::CommandPool& commandPool)
    {
        vk::CommandBufferAllocateInfo allocInfo;
        allocInfo
            .setCommandPool(commandPool)
            .setLevel(vk::CommandBufferLevel::ePrimary)
            .setCommandBufferCount(1);

        vk::CommandBuffer commandBuffer = device.allocateCommandBuffers(allocInfo)[0];

        vk::CommandBufferBeginInfo beginInfo;
        beginInfo
            .setFlags(vk::CommandBufferUsageFlagBits::eOneTimeSubmit);

        commandBuffer.begin(beginInfo);
        return commandBuffer;
    }

    inline void EndSingleTimeCommands(vk::Device& device, vk::CommandBuffer& commandBuffer, vk::Queue& graphicsQueue, vk::CommandPool& commandPool)
    {
        commandBuffer.end();

        vk::SubmitInfo submitInfo;
        submitInfo
            .setCommandBuffers(commandBuffer);
        
        graphicsQueue.submit(submitInfo);
        graphicsQueue.waitIdle();

        device.freeCommandBuffers(commandPool, commandBuffer);
    }

    inline std::pair<vk::Buffer, vk::DeviceMemory> CreateBuffer(vk::Device& device, vk::DeviceSize& size, vk::BufferUsageFlags& usage, vk::PhysicalDeviceMemoryProperties& physicalDeviceMemoryProperties, vk::MemoryPropertyFlags& memoryPropertyFlags)
    {
        vk::BufferCreateInfo bufferInfo;
        bufferInfo
            .setSize(size)
            .setUsage(usage)
            .setSharingMode(vk::SharingMode::eExclusive);

        vk::Buffer buffer = device.createBuffer(bufferInfo);

        vk::MemoryRequirements memRequirements = device.getBufferMemoryRequirements(buffer);

        vk::MemoryAllocateInfo allocInfo;
        allocInfo
            .setAllocationSize(memRequirements.size)
            .setMemoryTypeIndex(FindMemoryType(physicalDeviceMemoryProperties, memRequirements.memoryTypeBits, memoryPropertyFlags));

        vk::DeviceMemory bufferMemory = device.allocateMemory(allocInfo);
        device.bindBufferMemory(buffer, bufferMemory, 0);

        return { buffer, bufferMemory };
    }

    inline std::pair<vk::Image, vk::DeviceMemory> CreateImage(vk::Device& device, uint32_t width, uint32_t height, uint32_t mipLevels, vk::Format& format, vk::ImageTiling& tiling, vk::ImageUsageFlags& usage, vk::PhysicalDeviceMemoryProperties& physicalDeviceMemoryProperties, vk::MemoryPropertyFlags& memoryPropertyFlags)
    {
        vk::ImageCreateInfo imageInfo;
        imageInfo
            .setImageType(vk::ImageType::e2D)
            .setExtent(vk::Extent3D{ width, height, 1 })
            .setMipLevels(mipLevels)
            .setArrayLayers(1)
            .setFormat(format)
            .setTiling(tiling)
            .setInitialLayout(vk::ImageLayout::eUndefined)
            .setUsage(usage)
            .setSharingMode(vk::SharingMode::eExclusive)
            .setSamples(vk::SampleCountFlagBits::e1);

        vk::Image image = device.createImage(imageInfo);

        vk::MemoryRequirements memRequirements = device.getImageMemoryRequirements(image);
        uint32_t memoryTypeIndex = FindMemoryType(physicalDeviceMemoryProperties,memRequirements.memoryTypeBits,memoryPropertyFlags);
        vk::MemoryAllocateInfo allocInfo;
        allocInfo
            .setAllocationSize(memRequirements.size)
            .setMemoryTypeIndex(memoryTypeIndex);
        
        vk::DeviceMemory imageMemory = device.allocateMemory(allocInfo);
        device.bindImageMemory(image, imageMemory, 0);

        return { image, imageMemory };
    }

    inline std::pair<vk::Image, vk::DeviceMemory> CreateDepthImage(vk::Device& device, vk::PhysicalDevice& physicalDevice, uint32_t width, uint32_t height, vk::Format& format, vk::ImageTiling& tiling, vk::ImageUsageFlags& usage, vk::PhysicalDeviceMemoryProperties& physicalDeviceMemoryProperties, vk::MemoryPropertyFlags& memoryPropertyFlags)
    {
        vk::Format depthFormat = FindDepthFormat(physicalDevice);

        return CreateImage(device, width, height, 1, depthFormat, tiling, usage, physicalDeviceMemoryProperties, memoryPropertyFlags);
    }

    inline vk::ImageView CreateImageView(vk::Device& device, vk::Image& image, uint32_t mipLevels, vk::Format& format, vk::ImageAspectFlagBits aspectMask = vk::ImageAspectFlagBits::eColor)
    {
        vk::ImageViewCreateInfo viewInfo;
        viewInfo
            .setImage(image)
            .setViewType(vk::ImageViewType::e2D)
            .setFormat(format)
            .setSubresourceRange(vk::ImageSubresourceRange()
                .setAspectMask(aspectMask)
                .setBaseMipLevel(0)
                .setLevelCount(mipLevels)
                .setBaseArrayLayer(0)
                .setLayerCount(1));

        return device.createImageView(viewInfo);
    }

    inline vk::ImageView CreateDepthImageView(vk::Device& device, vk::PhysicalDevice& physicalDevice, vk::Image& image, vk::Format& format)
    {
        vk::Format depthFormat = FindDepthFormat(physicalDevice);

        return CreateImageView(device, image, 1, depthFormat, vk::ImageAspectFlagBits::eDepth);
    }

    inline vk::Sampler CreateSampler(vk::Device& device, vk::PhysicalDevice& physicalDevice)
    {

        vk::PhysicalDeviceProperties physicalDeviceProperties = physicalDevice.getProperties();
        vk::PhysicalDeviceFeatures physicalDeviceFeatures = physicalDevice.getFeatures();
        
        vk::SamplerCreateInfo samplerInfo;
        samplerInfo
            .setMagFilter(vk::Filter::eLinear)
            .setMinFilter(vk::Filter::eLinear)
            .setAddressModeU(vk::SamplerAddressMode::eRepeat)
            .setAddressModeV(vk::SamplerAddressMode::eRepeat)
            .setAddressModeW(vk::SamplerAddressMode::eRepeat)
            .setAnisotropyEnable(VK_TRUE)
            .setMaxAnisotropy(physicalDeviceProperties.limits.maxSamplerAnisotropy)
            .setBorderColor(vk::BorderColor::eIntOpaqueBlack)
            .setUnnormalizedCoordinates(VK_FALSE)
            .setCompareEnable(VK_FALSE)
            .setCompareOp(vk::CompareOp::eAlways)
            .setMipmapMode(vk::SamplerMipmapMode::eLinear)
            .setMipLodBias(0.0f)
            .setMinLod(0.0f)
            .setMaxLod(VK_LOD_CLAMP_NONE);

        if(physicalDeviceFeatures.samplerAnisotropy == VK_FALSE)
        {
            samplerInfo
                .setAnisotropyEnable(VK_FALSE)
                .setMaxAnisotropy(1.0f);
        }

        return device.createSampler(samplerInfo);
    }

    inline void  TransitionImageLayout(vk::Image& image, uint32_t mipLevels, vk::Format& format, vk::Device& device, vk::CommandPool& commandPool, vk::Queue& GraphicsQueue, vk::ImageLayout oldLayout, vk::ImageLayout newLayout)
    {
        vk::CommandBuffer commandBuffer = BeginSingleTimeCommands(device, commandPool);

        vk::ImageMemoryBarrier barrier;
        barrier
            .setOldLayout(oldLayout)
            .setNewLayout(newLayout)
            .setSrcQueueFamilyIndex(VK_QUEUE_FAMILY_IGNORED)
            .setDstQueueFamilyIndex(VK_QUEUE_FAMILY_IGNORED)
            .setImage(image)
            .setSubresourceRange(vk::ImageSubresourceRange()
                .setAspectMask(vk::ImageAspectFlagBits::eColor)
                .setBaseMipLevel(0)
                .setLevelCount(mipLevels)
                .setBaseArrayLayer(0)
                .setLayerCount(1));

        vk::PipelineStageFlags sourceStage;
        vk::PipelineStageFlags destinationStage;

        if(oldLayout == vk::ImageLayout::eUndefined && newLayout == vk::ImageLayout::eTransferDstOptimal)
        {
            barrier
                .setSrcAccessMask(vk::AccessFlagBits::eNone)
                .setDstAccessMask(vk::AccessFlagBits::eTransferWrite);
            sourceStage = vk::PipelineStageFlagBits::eTopOfPipe;
            destinationStage = vk::PipelineStageFlagBits::eTransfer;
        }
        else if(oldLayout == vk::ImageLayout::eTransferDstOptimal && newLayout == vk::ImageLayout::eShaderReadOnlyOptimal)
        {
            barrier
                .setSrcAccessMask(vk::AccessFlagBits::eTransferWrite)
                .setDstAccessMask(vk::AccessFlagBits::eShaderRead);
            sourceStage = vk::PipelineStageFlagBits::eTransfer;
            destinationStage = vk::PipelineStageFlagBits::eFragmentShader;
        }
        else if(oldLayout == vk::ImageLayout::eUndefined && newLayout == vk::ImageLayout::eDepthStencilAttachmentOptimal)
        {
            barrier
                .subresourceRange.setAspectMask(vk::ImageAspectFlagBits::eDepth);
            if (HasStencilComponent(format))
            {
                barrier.subresourceRange.aspectMask |= vk::ImageAspectFlagBits::eStencil;
            }

            barrier
                .setSrcAccessMask(vk::AccessFlagBits::eNone)
                .setDstAccessMask(vk::AccessFlagBits::eDepthStencilAttachmentRead | vk::AccessFlagBits::eDepthStencilAttachmentWrite);
            sourceStage = vk::PipelineStageFlagBits::eTopOfPipe;
            destinationStage = vk::PipelineStageFlagBits::eEarlyFragmentTests;
        }
        else
        {
            throw std::invalid_argument("unsupported layout transition!");
        }   
    
        commandBuffer.pipelineBarrier(
            sourceStage, 
            destinationStage,
            vk::DependencyFlags(),
            nullptr, nullptr, barrier);

        EndSingleTimeCommands(device, commandBuffer, GraphicsQueue, commandPool);
    }

    inline void CopyBufferToBuffer(vk::Device& device, vk::Queue& GraphicsQueue, vk::CommandPool& commandPool, vk::Buffer& srcBuffer, vk::Buffer& dstBuffer, vk::DeviceSize& size)
    {
        vk::CommandBuffer commandBuffer = BeginSingleTimeCommands(device, commandPool);

        vk::BufferCopy copyRegion;
        copyRegion
            .setSrcOffset(0)
            .setDstOffset(0)
            .setSize(size);
        commandBuffer.copyBuffer(srcBuffer, dstBuffer, 1, &copyRegion);

        EndSingleTimeCommands(device, commandBuffer, GraphicsQueue, commandPool);
    }

    inline void CopyBufferToImage(vk::Device& device, vk::Queue& GraphicsQueue, vk::CommandPool& commandPool, vk::Buffer& buffer, vk::Image& image, uint32_t width, uint32_t height)
    {
        vk::CommandBuffer commandBuffer = BeginSingleTimeCommands(device, commandPool);

        vk::BufferImageCopy region;
        region
            .setBufferOffset(0)
            .setBufferRowLength(0)
            .setBufferImageHeight(0)
            .setImageSubresource(vk::ImageSubresourceLayers()
                .setAspectMask(vk::ImageAspectFlagBits::eColor)
                .setMipLevel(0)
                .setBaseArrayLayer(0)
                .setLayerCount(1))
            .setImageOffset(vk::Offset3D{ 0, 0, 0 })
            .setImageExtent(vk::Extent3D{ width, height, 1 });

        commandBuffer.copyBufferToImage(buffer, image, vk::ImageLayout::eTransferDstOptimal, 1, &region);

        EndSingleTimeCommands(device, commandBuffer, GraphicsQueue, commandPool);
    }

    inline void GenerateMipmaps(vk::Device& device, vk::Queue& GraphicsQueue, vk::CommandPool& commandPool, vk::Image& image, uint32_t width, uint32_t height, uint32_t mipLevels)
    {
        vk::CommandBuffer commandBuffer = BeginSingleTimeCommands(device, commandPool);

        vk::ImageMemoryBarrier barrier;
        barrier
            .setImage(image)
            .setSrcQueueFamilyIndex(VK_QUEUE_FAMILY_IGNORED)
            .setDstQueueFamilyIndex(VK_QUEUE_FAMILY_IGNORED)
            .setSubresourceRange(vk::ImageSubresourceRange()
                .setAspectMask(vk::ImageAspectFlagBits::eColor)
                .setBaseMipLevel(0)
                .setLevelCount(1)
                .setBaseArrayLayer(0)
                .setLayerCount(1));

        int32_t mipWidth = width;
        int32_t mipHeight = height;
        for (uint32_t i = 1; i < mipLevels; i++)
        {
            barrier
                .setSubresourceRange(vk::ImageSubresourceRange().setBaseMipLevel(i - 1))
                .setOldLayout(vk::ImageLayout::eTransferDstOptimal)
                .setNewLayout(vk::ImageLayout::eTransferDstOptimal)
                .setSrcAccessMask(vk::AccessFlagBits::eTransferWrite)
                .setDstAccessMask(vk::AccessFlagBits::eTransferRead);

            commandBuffer.pipelineBarrier(
                vk::PipelineStageFlagBits::eTransfer,
                vk::PipelineStageFlagBits::eTransfer,
                vk::DependencyFlags(),
                nullptr,nullptr,
                barrier);

            vk::ImageBlit blit;
            blit
                .setSrcOffsets
                ({
                    vk::Offset3D{ 0, 0, 0 },
                    vk::Offset3D{ mipWidth, mipHeight, 1 }
                })
                .setSrcSubresource(vk::ImageSubresourceLayers()
                    .setAspectMask(vk::ImageAspectFlagBits::eColor)
                    .setMipLevel(i - 1)
                    .setBaseArrayLayer(0)
                    .setLayerCount(1))
                .setDstOffsets
                ({
                    vk::Offset3D{ 0, 0, 0 },
                    vk::Offset3D{ mipWidth > 1 ? mipWidth / 2 : 1, mipHeight > 1 ? mipHeight / 2 : 1, 1 }
                })
                .setDstSubresource(vk::ImageSubresourceLayers()
                    .setAspectMask(vk::ImageAspectFlagBits::eColor)
                    .setMipLevel(i)
                    .setBaseArrayLayer(0)
                    .setLayerCount(1));

            commandBuffer.blitImage(
                image, vk::ImageLayout::eTransferSrcOptimal,
                image, vk::ImageLayout::eTransferDstOptimal,
                1, &blit,
                vk::Filter::eLinear);

            if (mipWidth > 1 && mipHeight > 1)
            {
                mipWidth /= 2;
                mipHeight /= 2;
            }
        }

        barrier
            .setSubresourceRange(vk::ImageSubresourceRange().setBaseMipLevel(mipLevels - 1))
            .setOldLayout(vk::ImageLayout::eTransferDstOptimal)
            .setNewLayout(vk::ImageLayout::eShaderReadOnlyOptimal)
            .setSrcAccessMask(vk::AccessFlagBits::eTransferWrite)
            .setDstAccessMask(vk::AccessFlagBits::eShaderRead);

        commandBuffer.pipelineBarrier(
            vk::PipelineStageFlagBits::eTransfer,
            vk::PipelineStageFlagBits::eFragmentShader,
            vk::DependencyFlags(),
            nullptr, nullptr,
            barrier);

        EndSingleTimeCommands(device, commandBuffer, GraphicsQueue, commandPool);
    }
}