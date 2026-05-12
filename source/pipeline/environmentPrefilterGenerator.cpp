#include "environmentPrefilterGenerator.h"

#include "../commonFunction.h"
#include "../texture.h"
#include "../vulkanManager.h"
#include "computePipeline.h"
#include "pipelineFactory.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <tuple>
#include <vector>

namespace
{
    constexpr uint32_t kPrefilterSampleCount = 128;

    struct alignas(16) PrefilterParams
    {
        float roughness = 0.0f;
        float sampleCount = 0.0f;
        float pad0 = 0.0f;
        float pad1 = 0.0f;
    };

    vk::Sampler CreatePrefilterCubeSampler(vk::Device& device, float maxLod)
    {
        vk::SamplerCreateInfo samplerInfo;
        samplerInfo
            .setMagFilter(vk::Filter::eLinear)
            .setMinFilter(vk::Filter::eLinear)
            .setAddressModeU(vk::SamplerAddressMode::eClampToEdge)
            .setAddressModeV(vk::SamplerAddressMode::eClampToEdge)
            .setAddressModeW(vk::SamplerAddressMode::eClampToEdge)
            .setAnisotropyEnable(VK_FALSE)
            .setMaxAnisotropy(1.0f)
            .setBorderColor(vk::BorderColor::eFloatOpaqueBlack)
            .setUnnormalizedCoordinates(VK_FALSE)
            .setCompareEnable(VK_FALSE)
            .setCompareOp(vk::CompareOp::eAlways)
            .setMipmapMode(vk::SamplerMipmapMode::eLinear)
            .setMipLodBias(0.0f)
            .setMinLod(0.0f)
            .setMaxLod(maxLod);
        return CommonFunction::CreateSamplerBase(device, samplerInfo, "EnvironmentPrefilterSampler");
    }

    vk::ImageView CreateCubeMipStorageView(vk::Device& device, vk::Image image, vk::Format format, uint32_t mipLevel)
    {
        return CommonFunction::CreateImageViewBase(
            device,
            image,
            vk::ImageViewType::e2DArray,
            format,
            vk::ImageAspectFlagBits::eColor,
            mipLevel,
            1,
            0,
            6,
            "EnvironmentPrefilterStorageView_Mip" + std::to_string(mipLevel));
    }
}

std::shared_ptr<Texture> EnvironmentPrefilterGenerator::Generate(const Texture& environmentCube, uint32_t cubeSize, PipelineFactory& pipelineFactory)
{
    if (cubeSize == 0)
    {
        return nullptr;
    }

    auto& vulkanManager = VulkanManager::GetInstance();
    auto& device = vulkanManager.GetDevice();
    auto& gpuMemoryProperties = vulkanManager.GetGpuMemoryProperties();
    auto& commandPool = vulkanManager.GetCommandPool();
    auto& graphicsQueue = vulkanManager.GetGraphicQueue();

    const uint32_t mipLevels = static_cast<uint32_t>(std::floor(std::log2(static_cast<float>(cubeSize)))) + 1;
    vk::Format prefilterFormat = vk::Format::eR16G16B16A16Sfloat;
    vk::ImageUsageFlags prefilterUsage =
        vk::ImageUsageFlagBits::eStorage |
        vk::ImageUsageFlagBits::eSampled;
    vk::ImageTiling prefilterTiling = vk::ImageTiling::eOptimal;
    vk::MemoryPropertyFlags prefilterMemoryFlags = vk::MemoryPropertyFlagBits::eDeviceLocal;

    vk::ImageCreateInfo imageInfo;
    imageInfo
        .setFlags(vk::ImageCreateFlagBits::eCubeCompatible)
        .setImageType(vk::ImageType::e2D)
        .setExtent(vk::Extent3D{ cubeSize, cubeSize, 1 })
        .setMipLevels(mipLevels)
        .setArrayLayers(6)
        .setFormat(prefilterFormat)
        .setTiling(prefilterTiling)
        .setInitialLayout(vk::ImageLayout::eUndefined)
        .setUsage(prefilterUsage)
        .setSharingMode(vk::SharingMode::eExclusive)
        .setSamples(vk::SampleCountFlagBits::e1);

    vk::Image prefilterImage = device.createImage(imageInfo);
    vk::MemoryRequirements memoryRequirements = device.getImageMemoryRequirements(prefilterImage);
    vk::MemoryAllocateInfo allocInfo;
    allocInfo
        .setAllocationSize(memoryRequirements.size)
        .setMemoryTypeIndex(CommonFunction::FindMemoryType(
            gpuMemoryProperties,
            memoryRequirements.memoryTypeBits,
            prefilterMemoryFlags));

    vk::DeviceMemory prefilterImageMemory = device.allocateMemory(allocInfo);
    device.bindImageMemory(prefilterImage, prefilterImageMemory, 0);

    auto computePipeline = pipelineFactory.CreateComputePipeline("generator/prefilterEnvMap");

    std::array<vk::DescriptorPoolSize, 3> poolSizes = {
        vk::DescriptorPoolSize(vk::DescriptorType::eCombinedImageSampler, mipLevels),
        vk::DescriptorPoolSize(vk::DescriptorType::eStorageImage, mipLevels),
        vk::DescriptorPoolSize(vk::DescriptorType::eUniformBuffer, mipLevels)
    };

    vk::DescriptorPoolCreateInfo poolInfo;
    poolInfo
        .setPoolSizes(poolSizes)
        .setMaxSets(mipLevels);
    vk::DescriptorPool descriptorPool = device.createDescriptorPool(poolInfo);

    std::vector<vk::DescriptorSetLayout> setLayouts(mipLevels, computePipeline->GetDescriptorSetLayouts()[0]);
    vk::DescriptorSetAllocateInfo descriptorSetAllocateInfo;
    descriptorSetAllocateInfo
        .setDescriptorPool(descriptorPool)
        .setSetLayouts(setLayouts);
    std::vector<vk::DescriptorSet> descriptorSets = device.allocateDescriptorSets(descriptorSetAllocateInfo);

    struct BufferResource
    {
        vk::Buffer buffer;
        vk::DeviceMemory memory;
    };

    std::vector<BufferResource> paramBuffers(mipLevels);
    std::vector<vk::ImageView> storageViews(mipLevels);

    vk::DescriptorImageInfo environmentCubeInfo;
    environmentCubeInfo
        .setImageLayout(vk::ImageLayout::eShaderReadOnlyOptimal)
        .setImageView(environmentCube.getImageView())
        .setSampler(environmentCube.getSampler());

    for (uint32_t mipLevel = 0; mipLevel < mipLevels; ++mipLevel)
    {
        PrefilterParams params;
        params.roughness = mipLevels > 1 ? static_cast<float>(mipLevel) / static_cast<float>(mipLevels - 1) : 0.0f;
        params.sampleCount = static_cast<float>(kPrefilterSampleCount);

        vk::DeviceSize paramBufferSize = sizeof(PrefilterParams);
        vk::BufferUsageFlags paramBufferUsage = vk::BufferUsageFlagBits::eUniformBuffer;
        vk::MemoryPropertyFlags paramMemoryFlags =
            vk::MemoryPropertyFlagBits::eHostVisible | vk::MemoryPropertyFlagBits::eHostCoherent;

        auto [paramBuffer, paramBufferMemory] = CommonFunction::CreateBuffer(
            device,
            paramBufferSize,
            paramBufferUsage,
            gpuMemoryProperties,
            paramMemoryFlags,
            "EnvironmentPrefilterParams_Mip" + std::to_string(mipLevel));
        paramBuffers[mipLevel] = BufferResource{ paramBuffer, paramBufferMemory };

        void* mapped = device.mapMemory(paramBufferMemory, 0, sizeof(PrefilterParams));
        std::memcpy(mapped, &params, sizeof(PrefilterParams));
        device.unmapMemory(paramBufferMemory);

        storageViews[mipLevel] = CreateCubeMipStorageView(device, prefilterImage, prefilterFormat, mipLevel);

        vk::DescriptorImageInfo storageImageInfo;
        storageImageInfo
            .setImageLayout(vk::ImageLayout::eGeneral)
            .setImageView(storageViews[mipLevel]);

        vk::DescriptorBufferInfo paramBufferInfo;
        paramBufferInfo
            .setBuffer(paramBuffer)
            .setOffset(0)
            .setRange(sizeof(PrefilterParams));

        std::array<vk::WriteDescriptorSet, 3> writes;
        writes[0]
            .setDstSet(descriptorSets[mipLevel])
            .setDstBinding(0)
            .setDescriptorCount(1)
            .setDescriptorType(vk::DescriptorType::eCombinedImageSampler)
            .setImageInfo(environmentCubeInfo);
        writes[1]
            .setDstSet(descriptorSets[mipLevel])
            .setDstBinding(1)
            .setDescriptorCount(1)
            .setDescriptorType(vk::DescriptorType::eStorageImage)
            .setImageInfo(storageImageInfo);
        writes[2]
            .setDstSet(descriptorSets[mipLevel])
            .setDstBinding(2)
            .setDescriptorCount(1)
            .setDescriptorType(vk::DescriptorType::eUniformBuffer)
            .setBufferInfo(paramBufferInfo);
        device.updateDescriptorSets(writes, nullptr);
    }

    vk::CommandBuffer commandBuffer = CommonFunction::BeginSingleTimeCommands(device, commandPool);

    vk::ImageMemoryBarrier toGeneralBarrier;
    toGeneralBarrier
        .setOldLayout(vk::ImageLayout::eUndefined)
        .setNewLayout(vk::ImageLayout::eGeneral)
        .setSrcQueueFamilyIndex(VK_QUEUE_FAMILY_IGNORED)
        .setDstQueueFamilyIndex(VK_QUEUE_FAMILY_IGNORED)
        .setImage(prefilterImage)
        .setSubresourceRange(vk::ImageSubresourceRange()
            .setAspectMask(vk::ImageAspectFlagBits::eColor)
            .setBaseMipLevel(0)
            .setLevelCount(mipLevels)
            .setBaseArrayLayer(0)
            .setLayerCount(6))
        .setSrcAccessMask(vk::AccessFlagBits::eNone)
        .setDstAccessMask(vk::AccessFlagBits::eShaderWrite);
    commandBuffer.pipelineBarrier(
        vk::PipelineStageFlagBits::eTopOfPipe,
        vk::PipelineStageFlagBits::eComputeShader,
        vk::DependencyFlags(),
        nullptr,
        nullptr,
        toGeneralBarrier);

    computePipeline->Bind(commandBuffer);
    for (uint32_t mipLevel = 0; mipLevel < mipLevels; ++mipLevel)
    {
        const uint32_t mipSize = std::max(1u, cubeSize >> mipLevel);
        commandBuffer.bindDescriptorSets(
            vk::PipelineBindPoint::eCompute,
            computePipeline->GetPipelineLayout(),
            0,
            descriptorSets[mipLevel],
            nullptr);
        computePipeline->Dispatch(commandBuffer, (mipSize + 7) / 8, (mipSize + 7) / 8, 6);
    }

    vk::ImageMemoryBarrier toSampleBarrier;
    toSampleBarrier
        .setOldLayout(vk::ImageLayout::eGeneral)
        .setNewLayout(vk::ImageLayout::eShaderReadOnlyOptimal)
        .setSrcQueueFamilyIndex(VK_QUEUE_FAMILY_IGNORED)
        .setDstQueueFamilyIndex(VK_QUEUE_FAMILY_IGNORED)
        .setImage(prefilterImage)
        .setSubresourceRange(vk::ImageSubresourceRange()
            .setAspectMask(vk::ImageAspectFlagBits::eColor)
            .setBaseMipLevel(0)
            .setLevelCount(mipLevels)
            .setBaseArrayLayer(0)
            .setLayerCount(6))
        .setSrcAccessMask(vk::AccessFlagBits::eShaderWrite)
        .setDstAccessMask(vk::AccessFlagBits::eShaderRead);
    commandBuffer.pipelineBarrier(
        vk::PipelineStageFlagBits::eComputeShader,
        vk::PipelineStageFlagBits::eFragmentShader,
        vk::DependencyFlags(),
        nullptr,
        nullptr,
        toSampleBarrier);

    CommonFunction::EndSingleTimeCommands(device, commandBuffer, graphicsQueue, commandPool);

    device.destroyDescriptorPool(descriptorPool);
    for (uint32_t mipLevel = 0; mipLevel < mipLevels; ++mipLevel)
    {
        device.destroyImageView(storageViews[mipLevel]);
        device.destroyBuffer(paramBuffers[mipLevel].buffer);
        device.freeMemory(paramBuffers[mipLevel].memory);
    }

    vk::ImageView cubeSampleView = CommonFunction::CreateCubeImageView(device, prefilterImage, mipLevels, prefilterFormat, "EnvironmentPrefilterCubeView");
    vk::Sampler cubeSampler = CreatePrefilterCubeSampler(device, static_cast<float>(mipLevels - 1));
    return std::make_shared<Texture>(prefilterImage, prefilterImageMemory, cubeSampleView, cubeSampler, mipLevels, prefilterFormat);
}
