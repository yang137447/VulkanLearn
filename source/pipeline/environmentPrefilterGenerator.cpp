#include "environmentPrefilterGenerator.h"

#include "../commonFunction.h"
#include "../render/backend/rendererBackendVulkan.h"
#include "../texture.h"
#include "computePipeline.h"
#include "pipelineFactory.h"

#include <algorithm>
#include <array>
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

    vk::Sampler CreatePrefilterCubeSampler(VL::RendererBackendVulkan& rendererBackend, float maxLod)
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
        return rendererBackend.CreateSampler(samplerInfo, "EnvironmentPrefilterSampler");
    }

    vk::ImageView CreateCubeMipStorageView(
        VL::RendererBackendVulkan& rendererBackend,
        vk::Image image,
        vk::Format format,
        uint32_t mipLevel)
    {
        return rendererBackend.CreateImageView(
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

std::shared_ptr<Texture> EnvironmentPrefilterGenerator::Generate(
    const Texture& environmentCube,
    uint32_t cubeSize,
    PipelineFactory& pipelineFactory,
    VL::RendererBackendVulkan& rendererBackend)
{
    if (cubeSize == 0)
    {
        return nullptr;
    }

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

    vk::Image prefilterImage;
    vk::DeviceMemory prefilterImageMemory;
    std::tie(prefilterImage, prefilterImageMemory) = rendererBackend.CreateImage(
        imageInfo,
        prefilterMemoryFlags,
        "EnvironmentPrefilterImage");

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
    vk::DescriptorPool descriptorPool = rendererBackend.CreateDescriptorPool(
        poolInfo,
        "DescriptorPool: EnvironmentPrefilter");

    std::vector<vk::DescriptorSetLayout> setLayouts(mipLevels, computePipeline->GetDescriptorSetLayouts()[0]);
    vk::DescriptorSetAllocateInfo descriptorSetAllocateInfo;
    descriptorSetAllocateInfo
        .setDescriptorPool(descriptorPool)
        .setSetLayouts(setLayouts);
    std::vector<vk::DescriptorSet> descriptorSets(mipLevels);
    rendererBackend.AllocateDescriptorSets(descriptorSetAllocateInfo, descriptorSets);

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

        auto [paramBuffer, paramBufferMemory] = rendererBackend.CreateBuffer(
            paramBufferSize,
            paramBufferUsage,
            paramMemoryFlags,
            "EnvironmentPrefilterParams_Mip" + std::to_string(mipLevel));
        paramBuffers[mipLevel] = BufferResource{ paramBuffer, paramBufferMemory };

        void* mapped = rendererBackend.MapMemory(paramBufferMemory, sizeof(PrefilterParams));
        std::memcpy(mapped, &params, sizeof(PrefilterParams));
        rendererBackend.UnmapMemory(paramBufferMemory);

        storageViews[mipLevel] = CreateCubeMipStorageView(
            rendererBackend,
            prefilterImage,
            prefilterFormat,
            mipLevel);

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
        rendererBackend.UpdateDescriptorSets(std::vector<vk::WriteDescriptorSet>(
            writes.begin(),
            writes.end()));
    }

    vk::CommandBuffer commandBuffer = rendererBackend.BeginSingleTimeCommands();

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

    rendererBackend.EndSingleTimeCommands(commandBuffer);

    rendererBackend.DestroyDescriptorPool(descriptorPool);
    for (uint32_t mipLevel = 0; mipLevel < mipLevels; ++mipLevel)
    {
        rendererBackend.DestroyImageView(storageViews[mipLevel]);
        rendererBackend.DestroyBuffer(paramBuffers[mipLevel].buffer, paramBuffers[mipLevel].memory);
    }

    vk::ImageView cubeSampleView = rendererBackend.CreateCubeImageView(
        prefilterImage,
        mipLevels,
        prefilterFormat,
        "EnvironmentPrefilterCubeView");
    vk::Sampler cubeSampler = CreatePrefilterCubeSampler(
        rendererBackend,
        static_cast<float>(mipLevels - 1));
    return std::make_shared<Texture>(
        rendererBackend,
        prefilterImage,
        prefilterImageMemory,
        cubeSampleView,
        cubeSampler,
        mipLevels,
        prefilterFormat);
}
