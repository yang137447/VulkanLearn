#include "proceduralSkyCubeGenerator.h"

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

namespace VL
{
void ProceduralSkyCubeGenerator::Initialize(
    PipelineFactory& pipelineFactory,
    RendererBackendVulkan& rendererBackend,
    const std::vector<vk::DescriptorBufferInfo>& globalUniformBufferInfos)
{
    if (initialized)
    {
        return;
    }

    skyToCubemapPipeline = pipelineFactory.CreateComputePipeline("generator/skyToCubemap");

    CreateSkyCubeResources(rendererBackend);
    CreateDescriptorResources(rendererBackend, globalUniformBufferInfos);

    initialized = true;
}

void ProceduralSkyCubeGenerator::Shutdown(RendererBackendVulkan& rendererBackend)
{
    if (!initialized)
    {
        return;
    }

    DestroyDescriptorResources(rendererBackend);
    DestroySkyCubeResources(rendererBackend);

    skyToCubemapPipeline.reset();

    initialized = false;
}

void ProceduralSkyCubeGenerator::Record(vk::CommandBuffer commandBuffer, uint32_t swapchainImageIndex)
{
    if (!initialized)
    {
        return;
    }
    if (swapchainImageIndex >= skyToCubemapDescriptorSets.size() ||
        swapchainImageIndex >= globalUniformBufferInfos.size())
    {
        throw std::runtime_error("ProceduralSkyCubeGenerator descriptor set is missing for this swapchain image.");
    }

    vk::ImageSubresourceRange cubeRange;
    cubeRange
        .setAspectMask(vk::ImageAspectFlagBits::eColor)
        .setBaseMipLevel(0)
        .setLevelCount(1)
        .setBaseArrayLayer(0)
        .setLayerCount(6);


    // 1. skyCube 准备给 skyToCubemap.comp 写入。
    //    第一帧从 Undefined 过来；后续帧会从 ShaderReadOnlyOptimal 重新切回 General。
    vk::ImageMemoryBarrier cubeToStorageBarrier;
    cubeToStorageBarrier
        .setOldLayout(skyCube.layout)
        .setNewLayout(vk::ImageLayout::eGeneral)
        .setSrcQueueFamilyIndex(VK_QUEUE_FAMILY_IGNORED)
        .setDstQueueFamilyIndex(VK_QUEUE_FAMILY_IGNORED)
        .setImage(skyCube.texture->getImage())
        .setSubresourceRange(cubeRange)
        .setSrcAccessMask(
            skyCube.layout == vk::ImageLayout::eUndefined
                ? vk::AccessFlagBits::eNone
                : vk::AccessFlagBits::eShaderRead)
        .setDstAccessMask(vk::AccessFlagBits::eShaderWrite);

    commandBuffer.pipelineBarrier(
        skyCube.layout == vk::ImageLayout::eUndefined
            ? vk::PipelineStageFlagBits::eTopOfPipe
            : vk::PipelineStageFlagBits::eComputeShader,
        vk::PipelineStageFlagBits::eComputeShader,
        vk::DependencyFlags(),
        nullptr,
        nullptr,
        cubeToStorageBarrier);

    skyCube.layout = vk::ImageLayout::eGeneral;

    skyToCubemapPipeline->Bind(commandBuffer);
    commandBuffer.bindDescriptorSets(
        vk::PipelineBindPoint::eCompute,
        skyToCubemapPipeline->GetPipelineLayout(),
        0,
        skyToCubemapDescriptorSets[swapchainImageIndex],
        nullptr);
    skyToCubemapPipeline->Dispatch(
        commandBuffer,
        (skyCube.size + 7) / 8,
        (skyCube.size + 7) / 8,
        6);

    // 2. skyToCubemap.comp 写完 skyCube 后，转成 samplerCube 可读状态。
    //    后续 IBL baker 将采样该 cubemap
    vk::ImageMemoryBarrier cubeToSampleBarrier;
    cubeToSampleBarrier
        .setOldLayout(vk::ImageLayout::eGeneral)
        .setNewLayout(vk::ImageLayout::eShaderReadOnlyOptimal)
        .setSrcQueueFamilyIndex(VK_QUEUE_FAMILY_IGNORED)
        .setDstQueueFamilyIndex(VK_QUEUE_FAMILY_IGNORED)
        .setImage(skyCube.texture->getImage())
        .setSubresourceRange(cubeRange)
        .setSrcAccessMask(vk::AccessFlagBits::eShaderWrite)
        .setDstAccessMask(vk::AccessFlagBits::eShaderRead);

    commandBuffer.pipelineBarrier(
        vk::PipelineStageFlagBits::eComputeShader,
        vk::PipelineStageFlagBits::eComputeShader,
        vk::DependencyFlags(),
        nullptr,
        nullptr,
        cubeToSampleBarrier);

    skyCube.layout = vk::ImageLayout::eShaderReadOnlyOptimal;
}

void ProceduralSkyCubeGenerator::CreateSkyCubeResources(RendererBackendVulkan& rendererBackend)
{
    vk::ImageCreateInfo imageInfo;
    imageInfo
        .setFlags(vk::ImageCreateFlagBits::eCubeCompatible)
        .setImageType(vk::ImageType::e2D)
        .setExtent(vk::Extent3D{ skyCube.size, skyCube.size, 1 })
        .setMipLevels(1)
        .setArrayLayers(6)
        .setFormat(skyCube.format)
        .setTiling(vk::ImageTiling::eOptimal)
        .setInitialLayout(vk::ImageLayout::eUndefined)
        .setUsage(
            vk::ImageUsageFlagBits::eStorage |
            vk::ImageUsageFlagBits::eSampled)
        .setSharingMode(vk::SharingMode::eExclusive)
        .setSamples(vk::SampleCountFlagBits::e1);
    
    vk::Image image;
    vk::DeviceMemory memory;
    std::tie(image, memory) = rendererBackend.CreateImage(
        imageInfo,
        vk::MemoryPropertyFlagBits::eDeviceLocal,
        "ProceduralSkyCubeImage");

    skyCube.storageView = rendererBackend.CreateCubeStorageImageView(
        image,
        skyCube.format,
        "ProceduralSkyCubeStorageView");
    
    vk::ImageView sampleView = rendererBackend.CreateCubeImageView(
        image,
        1,
        skyCube.format,
        "ProceduralSkyCubeSampleView");

    vk::Sampler sampler = rendererBackend.CreateCubeSampler(
        0.0f,
        "ProceduralSkyCubeSampler");

    skyCube.texture = std::make_shared<Texture>(
        rendererBackend,
        image,
        memory,
        sampleView,
        sampler,
        1,
        skyCube.format);

    RendererResourceCache::GetInstance().BindWorldTexture(
        "environmentCube", 
        skyCube.texture);
}

void ProceduralSkyCubeGenerator::DestroySkyCubeResources(RendererBackendVulkan& rendererBackend)
{
    rendererBackend.DestroyImageView(skyCube.storageView);
    skyCube.texture.reset();
}

void ProceduralSkyCubeGenerator::CreateDescriptorResources(
    RendererBackendVulkan& rendererBackend,
    const std::vector<vk::DescriptorBufferInfo>& globalUniformBufferInfos)
{
    // Global UBO 是按 swapchain image 分配的：
    //   swapchain image 0 -> globalUniformBufferInfos[0]
    //   swapchain image 1 -> globalUniformBufferInfos[1]
    // 所以 compute descriptor set 也要按同样的 index 准备一套。
    const uint32_t descriptorSetCount = static_cast<uint32_t>(globalUniformBufferInfos.size());
    this->globalUniformBufferInfos = globalUniformBufferInfos;

    // 这里统计的是 descriptor pool 里需要多少“资源槽位”，不是 set 数量。
    // 每个 swapchain image 需要：
    //   skyToCubemap   binding 0: UniformBuffer, binding 1: StorageImage
    std::array<vk::DescriptorPoolSize, 2> poolSizes = {
        vk::DescriptorPoolSize{ vk::DescriptorType::eUniformBuffer, descriptorSetCount },
        vk::DescriptorPoolSize{ vk::DescriptorType::eStorageImage, descriptorSetCount },
    };

    vk::DescriptorPoolCreateInfo poolCreateInfo;
    poolCreateInfo
        .setPoolSizes(poolSizes)
        .setMaxSets(descriptorSetCount);

    descriptorPool = rendererBackend.CreateDescriptorPool(
        poolCreateInfo,
        "DescriptorPool: ProceduralSkyCube");

    // Vulkan 分配 descriptor set 时，需要为“每一个 set”提供一个 layout。
    // 这里把同一个 pipeline 的 set 0 layout 重复 descriptorSetCount 次，
    // 表示要分配 descriptorSetCount 个布局相同的 set。
    std::vector<vk::DescriptorSetLayout> skyToCubemapLayouts(
        descriptorSetCount,
        skyToCubemapPipeline->GetDescriptorSetLayouts()[0]);

    skyToCubemapDescriptorSets.resize(descriptorSetCount);

    vk::DescriptorSetAllocateInfo allocInfo;
    allocInfo
        .setDescriptorPool(descriptorPool)
        .setSetLayouts(skyToCubemapLayouts);
    rendererBackend.AllocateDescriptorSets(allocInfo, skyToCubemapDescriptorSets);

    for (uint32_t i = 0; i < descriptorSetCount; i++)
    {
        // skyToCubemap.comp 的 binding 1:
        //   layout(set = 0, binding = 1, rgba16f) uniform writeonly image2DArray outCubemap;
        // compute 写 storage image 时，descriptor 里记录 General layout。
        vk::DescriptorImageInfo storageImageInfo;
        storageImageInfo
            .setImageView(skyCube.storageView)
            .setImageLayout(vk::ImageLayout::eGeneral);

        std::array<vk::WriteDescriptorSet, 2> writes;

        // skyToCubemap.comp binding 0:
        //   用 global UBO 读取程序化天空参数。
        writes[0]
            .setDstSet(skyToCubemapDescriptorSets[i])
            .setDstBinding(0)
            .setDescriptorCount(1)
            .setDescriptorType(vk::DescriptorType::eUniformBuffer)
            .setBufferInfo(globalUniformBufferInfos[i]);

        // skyToCubemap.comp binding 1:
        //   把程序化天空颜色写入 sky cube 的 6 个 face。
        writes[1]
            .setDstSet(skyToCubemapDescriptorSets[i])
            .setDstBinding(1)
            .setDescriptorCount(1)
            .setDescriptorType(vk::DescriptorType::eStorageImage)
            .setImageInfo(storageImageInfo);

        rendererBackend.UpdateDescriptorSets(
            std::vector<vk::WriteDescriptorSet>(writes.begin(), writes.end()));
    }
}

std::shared_ptr<Texture> ProceduralSkyCubeGenerator::GetEnvironmentCube()
{
    if(skyCube.texture == nullptr)
    {
        throw std::runtime_error("Procedural Sky cube texture is not initialized.");
    }
    
    return skyCube.texture;
}


void ProceduralSkyCubeGenerator::DestroyDescriptorResources(RendererBackendVulkan& rendererBackend)
{
    globalUniformBufferInfos.clear();
    skyToCubemapDescriptorSets.clear();

    rendererBackend.DestroyDescriptorPool(descriptorPool);
}
} // namespace VL
