#include "environmentIblBaker.h"

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstring>
#include <cmath>
#include <stdexcept>
#include <tuple>

#include "pipeline/computePipeline.h"
#include "pipeline/pipelineFactory.h"
#include "render/backend/rendererBackendVulkan.h"
#include "render/resource/rendererResourceCache.h"
#include "texture.h"

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
}

namespace VL
{
void EnvironmentIblBaker::Initialize(
    PipelineFactory& pipelineFactory,
    RendererBackendVulkan& rendererBackend,
    const std::vector<vk::DescriptorBufferInfo>& globalUniformBufferInfos)
{
    if (initialized)
    {
        return;
    }

    skySHGeneratePipeline = pipelineFactory.CreateComputePipeline("generator/skySHGenerate");
    prefilterEnvMapPipeline = pipelineFactory.CreateComputePipeline("generator/prefilterEnvMap");

    CreatePrefilteredCubeResources(rendererBackend);
    CreateDescriptorResources(rendererBackend, globalUniformBufferInfos);

    this->rendererBackend = &rendererBackend;

    initialized = true;
}

void EnvironmentIblBaker::Shutdown(RendererBackendVulkan& rendererBackend)
{
    if (!initialized)
    {
        return;
    }

    DestroyDescriptorResources(rendererBackend);
    DestroyPrefilteredCubeResources(rendererBackend);

    skySHGeneratePipeline.reset();
    prefilterEnvMapPipeline.reset();

    this->rendererBackend = nullptr;

    initialized = false;
}

void EnvironmentIblBaker::Record(
    vk::CommandBuffer commandBuffer,
    const std::shared_ptr<Texture>& environmentCube,
    uint32_t swapchainImageIndex,
    bool rebuildEvenIfUnchanged)
{
    if (!initialized)
    {
        return;
    }
    if (!environmentCube || rendererBackend == nullptr ||
        swapchainImageIndex >= skySHGenerateDescriptorSets.size() ||
        swapchainImageIndex >= globalUniformBufferInfos.size() ||
        swapchainImageIndex >= boundEnvironmentCubes.size() ||
        prefilterDescriptorSets.size() < (swapchainImageIndex + 1) * prefilterCube.mipLevels)
    {
        throw std::runtime_error("EnvironmentIblBaker resources are missing.");
    }

    const bool environmentChanged =
        boundEnvironmentCubes[swapchainImageIndex] != environmentCube;
    if (!environmentChanged && !rebuildEvenIfUnchanged)
    {
        return;
    }
    if (environmentChanged)
    {
        UpdateEnvironmentCubeDescriptors(
            *rendererBackend,
            environmentCube,
            swapchainImageIndex);
        boundEnvironmentCubes[swapchainImageIndex] = environmentCube;
    }

    vk::ImageSubresourceRange prefilterRange;
    prefilterRange
        .setAspectMask(vk::ImageAspectFlagBits::eColor)
        .setBaseMipLevel(0)
        .setLevelCount(prefilterCube.mipLevels)
        .setBaseArrayLayer(0)
        .setLayerCount(6);

    skySHGeneratePipeline->Bind(commandBuffer);
    commandBuffer.bindDescriptorSets(
        vk::PipelineBindPoint::eCompute,
        skySHGeneratePipeline->GetPipelineLayout(),
        0,
        skySHGenerateDescriptorSets[swapchainImageIndex],
        nullptr);
    skySHGeneratePipeline->Dispatch(commandBuffer, 1, 1, 1);

    // 3. prefilterCube 准备给 prefilterEnvMap.comp 写入全部 mip。
    //    后续帧里上一帧的 graphics pass 可能采样过它，所以从 FragmentShader 等到 ComputeShader。
    vk::ImageMemoryBarrier prefilterToStorageBarrier;
    prefilterToStorageBarrier
        .setOldLayout(prefilterCube.layout)
        .setNewLayout(vk::ImageLayout::eGeneral)
        .setSrcQueueFamilyIndex(VK_QUEUE_FAMILY_IGNORED)
        .setDstQueueFamilyIndex(VK_QUEUE_FAMILY_IGNORED)
        .setImage(prefilterCube.texture->getImage())
        .setSubresourceRange(prefilterRange)
        .setSrcAccessMask(
            prefilterCube.layout == vk::ImageLayout::eUndefined
                ? vk::AccessFlagBits::eNone
                : vk::AccessFlagBits::eShaderRead)
        .setDstAccessMask(vk::AccessFlagBits::eShaderWrite);

    commandBuffer.pipelineBarrier(
        prefilterCube.layout == vk::ImageLayout::eUndefined
            ? vk::PipelineStageFlagBits::eTopOfPipe
            : vk::PipelineStageFlagBits::eFragmentShader,
        vk::PipelineStageFlagBits::eComputeShader,
        vk::DependencyFlags(),
        nullptr,
        nullptr,
        prefilterToStorageBarrier);

    prefilterCube.layout = vk::ImageLayout::eGeneral;

    // 4. 逐 mip 生成 specular IBL 预过滤结果。
    //    每个 mip 对应一个 roughness，descriptorIndex 用 swapchain image 和 mipLevel 展平索引。
    prefilterEnvMapPipeline->Bind(commandBuffer);
    for (uint32_t mipLevel = 0; mipLevel < prefilterCube.mipLevels; ++mipLevel)
    {
        const uint32_t descriptorIndex = swapchainImageIndex * prefilterCube.mipLevels + mipLevel;
        const uint32_t mipSize = std::max(1u, prefilterCube.size >> mipLevel);

        commandBuffer.bindDescriptorSets(
            vk::PipelineBindPoint::eCompute,
            prefilterEnvMapPipeline->GetPipelineLayout(),
            0,
            prefilterDescriptorSets[descriptorIndex],
            nullptr);

        prefilterEnvMapPipeline->Dispatch(
            commandBuffer,
            (mipSize + 7) / 8,
            (mipSize + 7) / 8,
            6);
    }

    // 5. skySHGenerate.comp 写完 diffuse SH 后，让后续 graphics pass 能按 uniform 读取。
    const vk::DescriptorBufferInfo& globalUboInfo = globalUniformBufferInfos[swapchainImageIndex];

    vk::BufferMemoryBarrier shToGraphicsBarrier;
    shToGraphicsBarrier
        .setSrcAccessMask(vk::AccessFlagBits::eShaderWrite)
        .setDstAccessMask(vk::AccessFlagBits::eUniformRead)
        .setSrcQueueFamilyIndex(VK_QUEUE_FAMILY_IGNORED)
        .setDstQueueFamilyIndex(VK_QUEUE_FAMILY_IGNORED)
        .setBuffer(globalUboInfo.buffer)
        .setOffset(globalUboInfo.offset)
        .setSize(globalUboInfo.range);

    commandBuffer.pipelineBarrier(
        vk::PipelineStageFlagBits::eComputeShader,
        vk::PipelineStageFlagBits::eVertexShader |
            vk::PipelineStageFlagBits::eFragmentShader,
        vk::DependencyFlags(),
        nullptr,
        shToGraphicsBarrier,
        nullptr);

    // 6. prefilterEnvMap.comp 写完 specular IBL 后，给 graphics pass 作为 samplerCube 读取。
    vk::ImageMemoryBarrier prefilterToSampleBarrier;
    prefilterToSampleBarrier
        .setOldLayout(vk::ImageLayout::eGeneral)
        .setNewLayout(vk::ImageLayout::eShaderReadOnlyOptimal)
        .setSrcQueueFamilyIndex(VK_QUEUE_FAMILY_IGNORED)
        .setDstQueueFamilyIndex(VK_QUEUE_FAMILY_IGNORED)
        .setImage(prefilterCube.texture->getImage())
        .setSubresourceRange(prefilterRange)
        .setSrcAccessMask(vk::AccessFlagBits::eShaderWrite)
        .setDstAccessMask(vk::AccessFlagBits::eShaderRead);

    commandBuffer.pipelineBarrier(
        vk::PipelineStageFlagBits::eComputeShader,
        vk::PipelineStageFlagBits::eFragmentShader,
        vk::DependencyFlags(),
        nullptr,
        nullptr,
        prefilterToSampleBarrier);

    prefilterCube.layout = vk::ImageLayout::eShaderReadOnlyOptimal;
}

void EnvironmentIblBaker::CreateDescriptorResources(
    RendererBackendVulkan& rendererBackend,
    const std::vector<vk::DescriptorBufferInfo>& globalUniformBufferInfos)
{
    // Global UBO 是按 swapchain image 分配的：
    //   swapchain image 0 -> globalUniformBufferInfos[0]
    //   swapchain image 1 -> globalUniformBufferInfos[1]
    // 所以 compute descriptor set 也要按同样的 index 准备一套。
    const uint32_t descriptorSetCount = static_cast<uint32_t>(globalUniformBufferInfos.size());
    this->globalUniformBufferInfos = globalUniformBufferInfos;
    const uint32_t prefilterDescriptorSetCount = descriptorSetCount * prefilterCube.mipLevels;

    std::array<vk::DescriptorPoolSize, 4> poolSizes = {
        vk::DescriptorPoolSize{ vk::DescriptorType::eStorageBuffer, descriptorSetCount },
        vk::DescriptorPoolSize{ vk::DescriptorType::eCombinedImageSampler, descriptorSetCount + prefilterDescriptorSetCount },
        vk::DescriptorPoolSize{ vk::DescriptorType::eStorageImage, prefilterDescriptorSetCount },
        vk::DescriptorPoolSize{ vk::DescriptorType::eUniformBuffer, prefilterDescriptorSetCount },
    };

    vk::DescriptorPoolCreateInfo poolCreateInfo;
    poolCreateInfo
        .setPoolSizes(poolSizes)
        .setMaxSets(descriptorSetCount + prefilterDescriptorSetCount);

    descriptorPool = rendererBackend.CreateDescriptorPool(
        poolCreateInfo,
        "DescriptorPool: EnvironmentIblBaker");

    // Vulkan 分配 descriptor set 时，需要为“每一个 set”提供一个 layout。
    // 这里把同一个 pipeline 的 set 0 layout 重复 descriptorSetCount 次，
    // 表示要分配 descriptorSetCount 个布局相同的 set。
    std::vector<vk::DescriptorSetLayout> skySHGenerateLayouts(
        descriptorSetCount,
        skySHGeneratePipeline->GetDescriptorSetLayouts()[0]);
    std::vector<vk::DescriptorSetLayout> prefilterLayouts(
        prefilterDescriptorSetCount,
        prefilterEnvMapPipeline->GetDescriptorSetLayouts()[0]);

    skySHGenerateDescriptorSets.resize(descriptorSetCount);
    prefilterDescriptorSets.resize(prefilterDescriptorSetCount);
    prefilterParamBuffers.resize(prefilterDescriptorSetCount);
    boundEnvironmentCubes.resize(descriptorSetCount);

    vk::DescriptorSetAllocateInfo allocInfo;
    allocInfo
        .setDescriptorPool(descriptorPool);

    allocInfo
        .setSetLayouts(skySHGenerateLayouts);
    rendererBackend.AllocateDescriptorSets(allocInfo, skySHGenerateDescriptorSets);

    allocInfo.setSetLayouts(prefilterLayouts);
    rendererBackend.AllocateDescriptorSets(allocInfo, prefilterDescriptorSets);

    for (uint32_t i = 0; i < descriptorSetCount; i++)
    {
        vk::WriteDescriptorSet write;
        write
            .setDstSet(skySHGenerateDescriptorSets[i])
            .setDstBinding(0)
            .setDescriptorCount(1)
            .setDescriptorType(vk::DescriptorType::eStorageBuffer)
            .setBufferInfo(globalUniformBufferInfos[i]);

        rendererBackend.UpdateDescriptorSets({ write });
    }
    for (uint32_t imageIndex = 0; imageIndex < descriptorSetCount; ++imageIndex)
    {
        for (uint32_t mipLevel = 0; mipLevel < prefilterCube.mipLevels; ++mipLevel)
        {
            const uint32_t descriptorIndex = imageIndex * prefilterCube.mipLevels + mipLevel;

            PrefilterParams params;
            params.roughness = prefilterCube.mipLevels > 1
                ? static_cast<float>(mipLevel) / static_cast<float>(prefilterCube.mipLevels - 1)
                : 0.0f;
            params.sampleCount = static_cast<float>(kPrefilterSampleCount);

            auto [paramBuffer, paramMemory] = rendererBackend.CreateBuffer(
                sizeof(PrefilterParams),
                vk::BufferUsageFlagBits::eUniformBuffer,
                vk::MemoryPropertyFlagBits::eHostVisible | vk::MemoryPropertyFlagBits::eHostCoherent,
                "ProceduralSkyPrefilterParams_Mip" + std::to_string(mipLevel));
            prefilterParamBuffers[descriptorIndex] = BufferResource{ paramBuffer, paramMemory };

            void* mapped = rendererBackend.MapMemory(paramMemory, sizeof(PrefilterParams));
            std::memcpy(mapped, &params, sizeof(PrefilterParams));
            rendererBackend.UnmapMemory(paramMemory);

            vk::DescriptorImageInfo outputMipInfo;
            outputMipInfo
                .setImageView(prefilterCube.storageViews[mipLevel])
                .setImageLayout(vk::ImageLayout::eGeneral);

            vk::DescriptorBufferInfo paramBufferInfo;
            paramBufferInfo
                .setBuffer(paramBuffer)
                .setOffset(0)
                .setRange(sizeof(PrefilterParams));

            std::array<vk::WriteDescriptorSet, 2> writes;
            writes[0]
                .setDstSet(prefilterDescriptorSets[descriptorIndex])
                .setDstBinding(1)
                .setDescriptorCount(1)
                .setDescriptorType(vk::DescriptorType::eStorageImage)
                .setImageInfo(outputMipInfo);

            writes[1]
                .setDstSet(prefilterDescriptorSets[descriptorIndex])
                .setDstBinding(2)
                .setDescriptorCount(1)
                .setDescriptorType(vk::DescriptorType::eUniformBuffer)
                .setBufferInfo(paramBufferInfo);

            rendererBackend.UpdateDescriptorSets(
                std::vector<vk::WriteDescriptorSet>(writes.begin(), writes.end()));
        }
    }
}

void EnvironmentIblBaker::UpdateEnvironmentCubeDescriptors(
    RendererBackendVulkan& rendererBackend,
    const std::shared_ptr<Texture>& environmentCube,
    uint32_t swapchainImageIndex)
{
    vk::DescriptorImageInfo imageInfo;
    imageInfo
        .setSampler(environmentCube->getSampler())
        .setImageView(environmentCube->getImageView())
        .setImageLayout(vk::ImageLayout::eShaderReadOnlyOptimal);

    std::vector<vk::WriteDescriptorSet> writes;
    writes.reserve(1 + prefilterCube.mipLevels);

    vk::WriteDescriptorSet shWrite;
    shWrite
        .setDstSet(skySHGenerateDescriptorSets[swapchainImageIndex])
        .setDstBinding(1)
        .setDescriptorCount(1)
        .setDescriptorType(vk::DescriptorType::eCombinedImageSampler)
        .setImageInfo(imageInfo);
    writes.push_back(shWrite);

    for (uint32_t mipLevel = 0; mipLevel < prefilterCube.mipLevels; ++mipLevel)
    {
        const uint32_t descriptorIndex =
            swapchainImageIndex * prefilterCube.mipLevels + mipLevel;
        vk::WriteDescriptorSet write;
        write
            .setDstSet(prefilterDescriptorSets[descriptorIndex])
            .setDstBinding(0)
            .setDescriptorCount(1)
            .setDescriptorType(
                vk::DescriptorType::eCombinedImageSampler)
            .setImageInfo(imageInfo);
        writes.push_back(write);
    }

    rendererBackend.UpdateDescriptorSets(writes);
}

void EnvironmentIblBaker::DestroyDescriptorResources(RendererBackendVulkan& rendererBackend)
{
    for (BufferResource& paramBuffer : prefilterParamBuffers)
    {
        rendererBackend.DestroyBuffer(paramBuffer.buffer, paramBuffer.memory);
    }
    prefilterParamBuffers.clear();
    prefilterDescriptorSets.clear();
    boundEnvironmentCubes.clear();
    globalUniformBufferInfos.clear();
    skySHGenerateDescriptorSets.clear();

    rendererBackend.DestroyDescriptorPool(descriptorPool);
}

void EnvironmentIblBaker::CreatePrefilteredCubeResources(RendererBackendVulkan& rendererBackend)
{
    prefilterCube.mipLevels = static_cast<uint32_t>(std::floor(std::log2(static_cast<float>(prefilterCube.size)))) + 1;

    vk::ImageCreateInfo imageInfo;
    imageInfo
        .setFlags(vk::ImageCreateFlagBits::eCubeCompatible)
        .setImageType(vk::ImageType::e2D)
        .setExtent(vk::Extent3D{ prefilterCube.size, prefilterCube.size, 1 })
        .setMipLevels(prefilterCube.mipLevels)
        .setArrayLayers(6)
        .setFormat(prefilterCube.format)
        .setTiling(vk::ImageTiling::eOptimal)
        .setInitialLayout(vk::ImageLayout::eUndefined)
        .setUsage(vk::ImageUsageFlagBits::eStorage | vk::ImageUsageFlagBits::eSampled)
        .setSharingMode(vk::SharingMode::eExclusive)
        .setSamples(vk::SampleCountFlagBits::e1);

    vk::Image image;
    vk::DeviceMemory memory;
    std::tie(image, memory) = rendererBackend.CreateImage(
        imageInfo,
        vk::MemoryPropertyFlagBits::eDeviceLocal,
        "ProceduralSkyPrefilteredCubeImage");

    prefilterCube.storageViews.resize(prefilterCube.mipLevels);
    for (uint32_t mipLevel = 0; mipLevel < prefilterCube.mipLevels; ++mipLevel)
    {
        prefilterCube.storageViews[mipLevel] = rendererBackend.CreateImageView(
            image,
            vk::ImageViewType::e2DArray,
            prefilterCube.format,
            vk::ImageAspectFlagBits::eColor,
            mipLevel,
            1,
            0,
            6,
            "ProceduralSkyPrefilteredCubeStorageView_Mip" + std::to_string(mipLevel));
    }

    vk::ImageView sampleView = rendererBackend.CreateCubeImageView(
        image,
        prefilterCube.mipLevels,
        prefilterCube.format,
        "ProceduralSkyPrefilteredCubeSampleView");

    vk::Sampler sampler = rendererBackend.CreateCubeSampler(
        static_cast<float>(prefilterCube.mipLevels - 1),
        "ProceduralSkyPrefilteredCubeSampler");

    prefilterCube.texture = std::make_shared<Texture>(
        rendererBackend,
        image,
        memory,
        sampleView,
        sampler,
        prefilterCube.mipLevels,
        prefilterCube.format);

    RendererResourceCache::GetInstance().BindWorldTexture(
        "prefilteredEnvironmentCube",
        prefilterCube.texture);
    
    prefilterCube.layout = vk::ImageLayout::eUndefined;
}

void EnvironmentIblBaker::DestroyPrefilteredCubeResources(RendererBackendVulkan& rendererBackend)
{
    for (vk::ImageView& storageView : prefilterCube.storageViews)
    {
        rendererBackend.DestroyImageView(storageView);
    }
    prefilterCube.storageViews.clear();

    prefilterCube.texture.reset();
    prefilterCube.layout = vk::ImageLayout::eUndefined;
    prefilterCube.mipLevels = 1;
}
} // namespace VL
