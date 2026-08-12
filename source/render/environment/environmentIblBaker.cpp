#include "render/environment/environmentIblBaker.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <stdexcept>
#include <tuple>

#include "baseStructs.h"
#include "pipeline/computePipeline.h"
#include "pipeline/pipelineFactory.h"
#include "render/backend/rendererBackendVulkan.h"
#include "texture.h"
#include "vulkanDebug.h"

namespace
{

constexpr uint32_t kCubemapFaceCount = 6;
constexpr uint32_t kPrefilterSampleCount = 128;
constexpr vk::DeviceSize kEnvironmentShOffset =
    static_cast<vk::DeviceSize>(offsetof(UBOGlobal, environmentSH));
constexpr vk::DeviceSize kEnvironmentShSize =
    static_cast<vk::DeviceSize>(sizeof(((UBOGlobal*)nullptr)->environmentSH));

struct alignas(16) PrefilterParams
{
    float roughness = 0.0f;
    float sampleCount = 0.0f;
    float pad0 = 0.0f;
    float pad1 = 0.0f;
};

vk::ImageSubresourceRange BuildCubeRange(uint32_t mipLevels)
{
    vk::ImageSubresourceRange range;
    range
        .setAspectMask(vk::ImageAspectFlagBits::eColor)
        .setBaseMipLevel(0)
        .setLevelCount(mipLevels)
        .setBaseArrayLayer(0)
        .setLayerCount(kCubemapFaceCount);
    return range;
}

vk::PipelineStageFlags GetPendingPrefilterSourceStage(vk::ImageLayout layout)
{
    switch (layout)
    {
    case vk::ImageLayout::eUndefined:
        return vk::PipelineStageFlagBits::eTopOfPipe;
    case vk::ImageLayout::eGeneral:
        return vk::PipelineStageFlagBits::eComputeShader;
    case vk::ImageLayout::eTransferSrcOptimal:
    case vk::ImageLayout::eTransferDstOptimal:
        return vk::PipelineStageFlagBits::eTransfer;
    default:
        throw std::runtime_error("Unsupported pending prefilter layout.");
    }
}

vk::AccessFlags GetPendingPrefilterSourceAccess(vk::ImageLayout layout)
{
    switch (layout)
    {
    case vk::ImageLayout::eUndefined:
        return vk::AccessFlagBits::eNone;
    case vk::ImageLayout::eGeneral:
        return vk::AccessFlagBits::eShaderWrite;
    case vk::ImageLayout::eTransferSrcOptimal:
        return vk::AccessFlagBits::eTransferRead;
    case vk::ImageLayout::eTransferDstOptimal:
        return vk::AccessFlagBits::eTransferWrite;
    default:
        throw std::runtime_error("Unsupported pending prefilter access state.");
    }
}

} // namespace

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

    const uint32_t swapchainImageCount = rendererBackend.GetSwapchainImageCount();
    if (globalUniformBufferInfos.size() != swapchainImageCount)
    {
        throw std::runtime_error(
            "Environment IBL requires exactly one Global UBO per swapchain image.");
    }
    for (const vk::DescriptorBufferInfo& globalUboInfo : globalUniformBufferInfos)
    {
        const bool rangeCoversEnvironmentSh =
            globalUboInfo.range == VK_WHOLE_SIZE ||
            globalUboInfo.range >= kEnvironmentShOffset + kEnvironmentShSize;
        if (!globalUboInfo.buffer || !rangeCoversEnvironmentSh)
        {
            throw std::runtime_error(
                "Environment IBL Global UBO binding does not cover environmentSH.");
        }
    }

    skySHGeneratePipeline = pipelineFactory.CreateComputePipeline("generator/skySHGenerate");
    prefilterEnvMapPipeline = pipelineFactory.CreateComputePipeline("generator/prefilterEnvMap");
    CreatePrefilteredCubeResources(rendererBackend);
    CreateEnvironmentShResources(rendererBackend);
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
    DestroyEnvironmentShResources(rendererBackend);
    DestroyPrefilteredCubeResources(rendererBackend);
    skySHGeneratePipeline.reset();
    prefilterEnvMapPipeline.reset();

    this->rendererBackend = nullptr;
    initialized = false;
}

void EnvironmentIblBaker::RecordSphericalHarmonics(
    vk::CommandBuffer commandBuffer,
    const std::shared_ptr<Texture>& environmentCube,
    uint32_t swapchainImageIndex)
{
    if (!initialized)
    {
        return;
    }
    EnsureEnvironmentCubeDescriptors(environmentCube, swapchainImageIndex);

    VulkanDebug::ScopedRegion debugRegion(
        commandBuffer,
        "Environment:SHProjection",
        VulkanDebug::DebugCategory::eResource);
    PrepareEnvironmentShOutputForCompute(commandBuffer);
    skySHGeneratePipeline->Bind(commandBuffer);
    commandBuffer.bindDescriptorSets(
        vk::PipelineBindPoint::eCompute,
        skySHGeneratePipeline->GetPipelineLayout(),
        0,
        skySHGenerateDescriptorSets[swapchainImageIndex],
        nullptr);
    skySHGeneratePipeline->Dispatch(commandBuffer, 1, 1, 1);
    environmentShAccess = ShBufferAccess::ComputeWrite;
}

void EnvironmentIblBaker::RecordPrefilterMip(
    vk::CommandBuffer commandBuffer,
    const std::shared_ptr<Texture>& environmentCube,
    uint32_t swapchainImageIndex,
    uint32_t mipLevel)
{
    if (!initialized)
    {
        return;
    }
    if (mipLevel >= pendingPrefilterCube.mipLevels)
    {
        throw std::runtime_error("Environment prefilter mip index is out of range.");
    }
    EnsureEnvironmentCubeDescriptors(environmentCube, swapchainImageIndex);

    VulkanDebug::ScopedRegion debugRegion(
        commandBuffer,
        "Environment:PrefilterMip" + std::to_string(mipLevel),
        VulkanDebug::DebugCategory::eResource);
    if (mipLevel == 0)
    {
        // pending 可能来自上一次 commit 的 TransferSrc，也可能是被新请求
        // 打断的 General；统一在新代际 mip 0 开始前建立覆盖写依赖。
        PreparePendingPrefilterForCompute(commandBuffer);
    }

    const uint32_t descriptorIndex =
        swapchainImageIndex * pendingPrefilterCube.mipLevels + mipLevel;
    const uint32_t mipSize = std::max(1u, pendingPrefilterCube.size >> mipLevel);
    prefilterEnvMapPipeline->Bind(commandBuffer);
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
        kCubemapFaceCount);
}

void EnvironmentIblBaker::RecordCommit(vk::CommandBuffer commandBuffer)
{
    if (!initialized)
    {
        return;
    }
    if (pendingPrefilterCube.layout != vk::ImageLayout::eGeneral ||
        activePrefilterCube.layout != vk::ImageLayout::eShaderReadOnlyOptimal ||
        environmentShAccess != ShBufferAccess::ComputeWrite)
    {
        throw std::runtime_error("Environment IBL commit requires complete pending SH and prefilter data.");
    }

    VulkanDebug::ScopedRegion debugRegion(
        commandBuffer,
        "Environment:IblCommit",
        VulkanDebug::DebugCategory::eResource);

    const vk::ImageSubresourceRange cubeRange =
        BuildCubeRange(pendingPrefilterCube.mipLevels);
    std::array<vk::ImageMemoryBarrier, 2> prepareCopyBarriers;
    prepareCopyBarriers[0]
        .setOldLayout(vk::ImageLayout::eGeneral)
        .setNewLayout(vk::ImageLayout::eTransferSrcOptimal)
        .setSrcQueueFamilyIndex(VK_QUEUE_FAMILY_IGNORED)
        .setDstQueueFamilyIndex(VK_QUEUE_FAMILY_IGNORED)
        .setImage(pendingPrefilterCube.texture->getImage())
        .setSubresourceRange(cubeRange)
        .setSrcAccessMask(vk::AccessFlagBits::eShaderWrite)
        .setDstAccessMask(vk::AccessFlagBits::eTransferRead);
    prepareCopyBarriers[1]
        .setOldLayout(vk::ImageLayout::eShaderReadOnlyOptimal)
        .setNewLayout(vk::ImageLayout::eTransferDstOptimal)
        .setSrcQueueFamilyIndex(VK_QUEUE_FAMILY_IGNORED)
        .setDstQueueFamilyIndex(VK_QUEUE_FAMILY_IGNORED)
        .setImage(activePrefilterCube.texture->getImage())
        .setSubresourceRange(cubeRange)
        .setSrcAccessMask(vk::AccessFlagBits::eShaderRead)
        .setDstAccessMask(vk::AccessFlagBits::eTransferWrite);
    commandBuffer.pipelineBarrier(
        vk::PipelineStageFlagBits::eComputeShader |
            vk::PipelineStageFlagBits::eFragmentShader,
        vk::PipelineStageFlagBits::eTransfer,
        vk::DependencyFlags(),
        nullptr,
        nullptr,
        prepareCopyBarriers);

    std::vector<vk::ImageCopy> copyRegions;
    copyRegions.reserve(pendingPrefilterCube.mipLevels);
    for (uint32_t mipLevel = 0; mipLevel < pendingPrefilterCube.mipLevels; ++mipLevel)
    {
        const uint32_t mipSize = std::max(1u, pendingPrefilterCube.size >> mipLevel);
        vk::ImageSubresourceLayers subresource;
        subresource
            .setAspectMask(vk::ImageAspectFlagBits::eColor)
            .setMipLevel(mipLevel)
            .setBaseArrayLayer(0)
            .setLayerCount(kCubemapFaceCount);

        vk::ImageCopy region;
        region
            .setSrcSubresource(subresource)
            .setDstSubresource(subresource)
            .setExtent(vk::Extent3D{ mipSize, mipSize, 1 });
        copyRegions.push_back(region);
    }
    commandBuffer.copyImage(
        pendingPrefilterCube.texture->getImage(),
        vk::ImageLayout::eTransferSrcOptimal,
        activePrefilterCube.texture->getImage(),
        vk::ImageLayout::eTransferDstOptimal,
        copyRegions);

    vk::ImageMemoryBarrier activeToSampleBarrier;
    activeToSampleBarrier
        .setOldLayout(vk::ImageLayout::eTransferDstOptimal)
        .setNewLayout(vk::ImageLayout::eShaderReadOnlyOptimal)
        .setSrcQueueFamilyIndex(VK_QUEUE_FAMILY_IGNORED)
        .setDstQueueFamilyIndex(VK_QUEUE_FAMILY_IGNORED)
        .setImage(activePrefilterCube.texture->getImage())
        .setSubresourceRange(cubeRange)
        .setSrcAccessMask(vk::AccessFlagBits::eTransferWrite)
        .setDstAccessMask(vk::AccessFlagBits::eShaderRead);
    commandBuffer.pipelineBarrier(
        vk::PipelineStageFlagBits::eTransfer,
        vk::PipelineStageFlagBits::eComputeShader |
            vk::PipelineStageFlagBits::eFragmentShader,
        vk::DependencyFlags(),
        nullptr,
        nullptr,
        activeToSampleBarrier);

    pendingPrefilterCube.layout = vk::ImageLayout::eTransferSrcOptimal;
    activePrefilterCube.layout = vk::ImageLayout::eShaderReadOnlyOptimal;

    // SH 只在 commit 时广播到所有 frame-local UBO，确保漫反射和 specular IBL
    // 在同一代际切换，且不会为每个 swapchain image 重复执行投影。
    BroadcastEnvironmentShToGlobalBuffers(commandBuffer);
}

std::shared_ptr<Texture> EnvironmentIblBaker::GetPrefilteredEnvironmentCube() const
{
    if (!activePrefilterCube.texture)
    {
        throw std::runtime_error("Active prefiltered environment cube is not initialized.");
    }
    return activePrefilterCube.texture;
}

void EnvironmentIblBaker::CreateDescriptorResources(
    RendererBackendVulkan& rendererBackend,
    const std::vector<vk::DescriptorBufferInfo>& globalUniformBufferInfos)
{
    const uint32_t descriptorSetCount = static_cast<uint32_t>(globalUniformBufferInfos.size());
    this->globalUniformBufferInfos = globalUniformBufferInfos;
    const uint32_t prefilterDescriptorSetCount =
        descriptorSetCount * pendingPrefilterCube.mipLevels;

    std::array<vk::DescriptorPoolSize, 4> poolSizes = {
        vk::DescriptorPoolSize{ vk::DescriptorType::eStorageBuffer, descriptorSetCount },
        vk::DescriptorPoolSize{
            vk::DescriptorType::eCombinedImageSampler,
            descriptorSetCount + prefilterDescriptorSetCount },
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

    std::vector<vk::DescriptorSetLayout> shLayouts(
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
    allocInfo.setDescriptorPool(descriptorPool).setSetLayouts(shLayouts);
    rendererBackend.AllocateDescriptorSets(allocInfo, skySHGenerateDescriptorSets);
    allocInfo.setSetLayouts(prefilterLayouts);
    rendererBackend.AllocateDescriptorSets(allocInfo, prefilterDescriptorSets);

    vk::DescriptorBufferInfo shOutputInfo;
    shOutputInfo
        .setBuffer(environmentShOutputBuffer.buffer)
        .setOffset(0)
        .setRange(kEnvironmentShSize);
    for (uint32_t imageIndex = 0; imageIndex < descriptorSetCount; ++imageIndex)
    {
        vk::WriteDescriptorSet write;
        write
            .setDstSet(skySHGenerateDescriptorSets[imageIndex])
            .setDstBinding(0)
            .setDescriptorCount(1)
            .setDescriptorType(vk::DescriptorType::eStorageBuffer)
            .setBufferInfo(shOutputInfo);
        rendererBackend.UpdateDescriptorSets({ write });
    }

    for (uint32_t imageIndex = 0; imageIndex < descriptorSetCount; ++imageIndex)
    {
        for (uint32_t mipLevel = 0; mipLevel < pendingPrefilterCube.mipLevels; ++mipLevel)
        {
            const uint32_t descriptorIndex =
                imageIndex * pendingPrefilterCube.mipLevels + mipLevel;
            PrefilterParams params;
            params.roughness = pendingPrefilterCube.mipLevels > 1
                ? static_cast<float>(mipLevel) /
                    static_cast<float>(pendingPrefilterCube.mipLevels - 1)
                : 0.0f;
            params.sampleCount = static_cast<float>(kPrefilterSampleCount);

            auto [paramBuffer, paramMemory] = rendererBackend.CreateBuffer(
                sizeof(PrefilterParams),
                vk::BufferUsageFlagBits::eUniformBuffer,
                vk::MemoryPropertyFlagBits::eHostVisible |
                    vk::MemoryPropertyFlagBits::eHostCoherent,
                "EnvironmentPrefilterParams_Image" + std::to_string(imageIndex) +
                    "_Mip" + std::to_string(mipLevel));
            prefilterParamBuffers[descriptorIndex] = BufferResource{ paramBuffer, paramMemory };

            void* mapped = rendererBackend.MapMemory(paramMemory, sizeof(PrefilterParams));
            std::memcpy(mapped, &params, sizeof(params));
            rendererBackend.UnmapMemory(paramMemory);

            vk::DescriptorImageInfo outputMipInfo;
            outputMipInfo
                .setImageView(pendingPrefilterCube.storageViews[mipLevel])
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

void EnvironmentIblBaker::EnsureEnvironmentCubeDescriptors(
    const std::shared_ptr<Texture>& environmentCube,
    uint32_t swapchainImageIndex)
{
    if (!environmentCube || rendererBackend == nullptr ||
        swapchainImageIndex >= boundEnvironmentCubes.size())
    {
        throw std::runtime_error("Environment IBL source or descriptor index is invalid.");
    }
    if (boundEnvironmentCubes[swapchainImageIndex] == environmentCube)
    {
        return;
    }

    // 只更新当前已 acquire 且 fence 已等待完成的 swapchain descriptor set。
    // 其余 image 在真正承担后续 mip/SH 工作时再更新，避免改写仍在飞行中的 set。
    vk::DescriptorImageInfo imageInfo;
    imageInfo
        .setSampler(environmentCube->getSampler())
        .setImageView(environmentCube->getImageView())
        .setImageLayout(vk::ImageLayout::eShaderReadOnlyOptimal);

    std::vector<vk::WriteDescriptorSet> writes;
    writes.reserve(1 + pendingPrefilterCube.mipLevels);
    vk::WriteDescriptorSet shWrite;
    shWrite
        .setDstSet(skySHGenerateDescriptorSets[swapchainImageIndex])
        .setDstBinding(1)
        .setDescriptorCount(1)
        .setDescriptorType(vk::DescriptorType::eCombinedImageSampler)
        .setImageInfo(imageInfo);
    writes.push_back(shWrite);

    for (uint32_t mipLevel = 0; mipLevel < pendingPrefilterCube.mipLevels; ++mipLevel)
    {
        const uint32_t descriptorIndex =
            swapchainImageIndex * pendingPrefilterCube.mipLevels + mipLevel;
        vk::WriteDescriptorSet write;
        write
            .setDstSet(prefilterDescriptorSets[descriptorIndex])
            .setDstBinding(0)
            .setDescriptorCount(1)
            .setDescriptorType(vk::DescriptorType::eCombinedImageSampler)
            .setImageInfo(imageInfo);
        writes.push_back(write);
    }
    rendererBackend->UpdateDescriptorSets(writes);
    boundEnvironmentCubes[swapchainImageIndex] = environmentCube;
}

void EnvironmentIblBaker::CreateEnvironmentShResources(
    RendererBackendVulkan& rendererBackend)
{
    auto [buffer, memory] = rendererBackend.CreateBuffer(
        kEnvironmentShSize,
        vk::BufferUsageFlagBits::eStorageBuffer |
            vk::BufferUsageFlagBits::eTransferSrc,
        vk::MemoryPropertyFlagBits::eDeviceLocal,
        "EnvironmentSHPendingOutput");
    environmentShOutputBuffer = BufferResource{ buffer, memory };
    environmentShAccess = ShBufferAccess::Undefined;
}

void EnvironmentIblBaker::DestroyEnvironmentShResources(
    RendererBackendVulkan& rendererBackend)
{
    rendererBackend.DestroyBuffer(
        environmentShOutputBuffer.buffer,
        environmentShOutputBuffer.memory);
    environmentShOutputBuffer = BufferResource{};
    environmentShAccess = ShBufferAccess::Undefined;
}

void EnvironmentIblBaker::PrepareEnvironmentShOutputForCompute(
    vk::CommandBuffer commandBuffer)
{
    vk::PipelineStageFlags sourceStage = vk::PipelineStageFlagBits::eTopOfPipe;
    vk::AccessFlags sourceAccess = vk::AccessFlagBits::eNone;
    if (environmentShAccess == ShBufferAccess::ComputeWrite)
    {
        sourceStage = vk::PipelineStageFlagBits::eComputeShader;
        sourceAccess = vk::AccessFlagBits::eShaderWrite;
    }
    else if (environmentShAccess == ShBufferAccess::TransferRead)
    {
        sourceStage = vk::PipelineStageFlagBits::eTransfer;
        sourceAccess = vk::AccessFlagBits::eTransferRead;
    }

    vk::BufferMemoryBarrier barrier;
    barrier
        .setSrcAccessMask(sourceAccess)
        .setDstAccessMask(vk::AccessFlagBits::eShaderWrite)
        .setSrcQueueFamilyIndex(VK_QUEUE_FAMILY_IGNORED)
        .setDstQueueFamilyIndex(VK_QUEUE_FAMILY_IGNORED)
        .setBuffer(environmentShOutputBuffer.buffer)
        .setOffset(0)
        .setSize(kEnvironmentShSize);
    commandBuffer.pipelineBarrier(
        sourceStage,
        vk::PipelineStageFlagBits::eComputeShader,
        vk::DependencyFlags(),
        nullptr,
        barrier,
        nullptr);
}

void EnvironmentIblBaker::BroadcastEnvironmentShToGlobalBuffers(
    vk::CommandBuffer commandBuffer)
{
    if (globalUniformBufferInfos.empty() ||
        environmentShAccess != ShBufferAccess::ComputeWrite)
    {
        throw std::runtime_error("Environment SH broadcast has no complete pending result.");
    }

    std::vector<vk::BufferMemoryBarrier> prepareCopyBarriers;
    prepareCopyBarriers.reserve(globalUniformBufferInfos.size() + 1);
    vk::BufferMemoryBarrier outputToTransferBarrier;
    outputToTransferBarrier
        .setSrcAccessMask(vk::AccessFlagBits::eShaderWrite)
        .setDstAccessMask(vk::AccessFlagBits::eTransferRead)
        .setSrcQueueFamilyIndex(VK_QUEUE_FAMILY_IGNORED)
        .setDstQueueFamilyIndex(VK_QUEUE_FAMILY_IGNORED)
        .setBuffer(environmentShOutputBuffer.buffer)
        .setOffset(0)
        .setSize(kEnvironmentShSize);
    prepareCopyBarriers.push_back(outputToTransferBarrier);

    for (const vk::DescriptorBufferInfo& globalUboInfo : globalUniformBufferInfos)
    {
        vk::BufferMemoryBarrier globalUboToTransferBarrier;
        globalUboToTransferBarrier
            .setSrcAccessMask(
                vk::AccessFlagBits::eUniformRead |
                vk::AccessFlagBits::eTransferWrite)
            .setDstAccessMask(vk::AccessFlagBits::eTransferWrite)
            .setSrcQueueFamilyIndex(VK_QUEUE_FAMILY_IGNORED)
            .setDstQueueFamilyIndex(VK_QUEUE_FAMILY_IGNORED)
            .setBuffer(globalUboInfo.buffer)
            .setOffset(globalUboInfo.offset + kEnvironmentShOffset)
            .setSize(kEnvironmentShSize);
        prepareCopyBarriers.push_back(globalUboToTransferBarrier);
    }

    commandBuffer.pipelineBarrier(
        vk::PipelineStageFlagBits::eTransfer |
            vk::PipelineStageFlagBits::eComputeShader |
            vk::PipelineStageFlagBits::eVertexShader |
            vk::PipelineStageFlagBits::eFragmentShader,
        vk::PipelineStageFlagBits::eTransfer,
        vk::DependencyFlags(),
        nullptr,
        prepareCopyBarriers,
        nullptr);

    for (const vk::DescriptorBufferInfo& globalUboInfo : globalUniformBufferInfos)
    {
        vk::BufferCopy copyRegion;
        copyRegion
            .setSrcOffset(0)
            .setDstOffset(globalUboInfo.offset + kEnvironmentShOffset)
            .setSize(kEnvironmentShSize);
        commandBuffer.copyBuffer(
            environmentShOutputBuffer.buffer,
            globalUboInfo.buffer,
            copyRegion);
    }

    std::vector<vk::BufferMemoryBarrier> shToGraphicsBarriers;
    shToGraphicsBarriers.reserve(globalUniformBufferInfos.size());
    for (const vk::DescriptorBufferInfo& globalUboInfo : globalUniformBufferInfos)
    {
        vk::BufferMemoryBarrier barrier;
        barrier
            .setSrcAccessMask(vk::AccessFlagBits::eTransferWrite)
            .setDstAccessMask(vk::AccessFlagBits::eUniformRead)
            .setSrcQueueFamilyIndex(VK_QUEUE_FAMILY_IGNORED)
            .setDstQueueFamilyIndex(VK_QUEUE_FAMILY_IGNORED)
            .setBuffer(globalUboInfo.buffer)
            .setOffset(globalUboInfo.offset + kEnvironmentShOffset)
            .setSize(kEnvironmentShSize);
        shToGraphicsBarriers.push_back(barrier);
    }
    commandBuffer.pipelineBarrier(
        vk::PipelineStageFlagBits::eTransfer,
        vk::PipelineStageFlagBits::eComputeShader |
            vk::PipelineStageFlagBits::eVertexShader |
            vk::PipelineStageFlagBits::eFragmentShader,
        vk::DependencyFlags(),
        nullptr,
        shToGraphicsBarriers,
        nullptr);
    environmentShAccess = ShBufferAccess::TransferRead;
}

void EnvironmentIblBaker::DestroyDescriptorResources(
    RendererBackendVulkan& rendererBackend)
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

void EnvironmentIblBaker::CreatePrefilteredCubeResources(
    RendererBackendVulkan& rendererBackend)
{
    const uint32_t mipLevels = static_cast<uint32_t>(
        std::floor(std::log2(static_cast<float>(activePrefilterCube.size)))) + 1;
    activePrefilterCube.mipLevels = mipLevels;
    pendingPrefilterCube.mipLevels = mipLevels;
    CreatePrefilteredCubeResource(
        rendererBackend,
        activePrefilterCube,
        false,
        "EnvironmentActivePrefilterCube");
    CreatePrefilteredCubeResource(
        rendererBackend,
        pendingPrefilterCube,
        true,
        "EnvironmentPendingPrefilterCube");
    InitializeActivePrefilteredCube(rendererBackend);
}

void EnvironmentIblBaker::CreatePrefilteredCubeResource(
    RendererBackendVulkan& rendererBackend,
    PrefilteredCubeResources& resources,
    bool createStorageViews,
    const std::string& debugName)
{
    vk::ImageCreateInfo imageInfo;
    imageInfo
        .setFlags(vk::ImageCreateFlagBits::eCubeCompatible)
        .setImageType(vk::ImageType::e2D)
        .setExtent(vk::Extent3D{ resources.size, resources.size, 1 })
        .setMipLevels(resources.mipLevels)
        .setArrayLayers(kCubemapFaceCount)
        .setFormat(resources.format)
        .setTiling(vk::ImageTiling::eOptimal)
        .setInitialLayout(vk::ImageLayout::eUndefined)
        .setUsage(
            vk::ImageUsageFlagBits::eStorage |
            vk::ImageUsageFlagBits::eSampled |
            vk::ImageUsageFlagBits::eTransferSrc |
            vk::ImageUsageFlagBits::eTransferDst)
        .setSharingMode(vk::SharingMode::eExclusive)
        .setSamples(vk::SampleCountFlagBits::e1);

    vk::Image image;
    vk::DeviceMemory memory;
    std::tie(image, memory) = rendererBackend.CreateImage(
        imageInfo,
        vk::MemoryPropertyFlagBits::eDeviceLocal,
        debugName + "Image");

    if (createStorageViews)
    {
        resources.storageViews.resize(resources.mipLevels);
        for (uint32_t mipLevel = 0; mipLevel < resources.mipLevels; ++mipLevel)
        {
            resources.storageViews[mipLevel] = rendererBackend.CreateImageView(
                image,
                vk::ImageViewType::e2DArray,
                resources.format,
                vk::ImageAspectFlagBits::eColor,
                mipLevel,
                1,
                0,
                kCubemapFaceCount,
                debugName + "StorageView_Mip" + std::to_string(mipLevel));
        }
    }

    vk::ImageView sampleView = rendererBackend.CreateCubeImageView(
        image,
        resources.mipLevels,
        resources.format,
        debugName + "SampleView");
    vk::Sampler sampler = rendererBackend.CreateCubeSampler(
        static_cast<float>(resources.mipLevels - 1),
        debugName + "Sampler");
    resources.texture = std::make_shared<Texture>(
        rendererBackend,
        image,
        memory,
        sampleView,
        sampler,
        resources.mipLevels,
        resources.format);
    resources.layout = vk::ImageLayout::eUndefined;
}

void EnvironmentIblBaker::InitializeActivePrefilteredCube(
    RendererBackendVulkan& rendererBackend)
{
    vk::CommandBuffer commandBuffer = rendererBackend.BeginSingleTimeCommands();
    const vk::ImageSubresourceRange cubeRange =
        BuildCubeRange(activePrefilterCube.mipLevels);

    vk::ImageMemoryBarrier toClearBarrier;
    toClearBarrier
        .setOldLayout(vk::ImageLayout::eUndefined)
        .setNewLayout(vk::ImageLayout::eTransferDstOptimal)
        .setSrcQueueFamilyIndex(VK_QUEUE_FAMILY_IGNORED)
        .setDstQueueFamilyIndex(VK_QUEUE_FAMILY_IGNORED)
        .setImage(activePrefilterCube.texture->getImage())
        .setSubresourceRange(cubeRange)
        .setSrcAccessMask(vk::AccessFlagBits::eNone)
        .setDstAccessMask(vk::AccessFlagBits::eTransferWrite);
    commandBuffer.pipelineBarrier(
        vk::PipelineStageFlagBits::eTopOfPipe,
        vk::PipelineStageFlagBits::eTransfer,
        vk::DependencyFlags(),
        nullptr,
        nullptr,
        toClearBarrier);
    commandBuffer.clearColorImage(
        activePrefilterCube.texture->getImage(),
        vk::ImageLayout::eTransferDstOptimal,
        vk::ClearColorValue(std::array<float, 4>{ 0.0f, 0.0f, 0.0f, 1.0f }),
        cubeRange);

    vk::ImageMemoryBarrier toSampleBarrier;
    toSampleBarrier
        .setOldLayout(vk::ImageLayout::eTransferDstOptimal)
        .setNewLayout(vk::ImageLayout::eShaderReadOnlyOptimal)
        .setSrcQueueFamilyIndex(VK_QUEUE_FAMILY_IGNORED)
        .setDstQueueFamilyIndex(VK_QUEUE_FAMILY_IGNORED)
        .setImage(activePrefilterCube.texture->getImage())
        .setSubresourceRange(cubeRange)
        .setSrcAccessMask(vk::AccessFlagBits::eTransferWrite)
        .setDstAccessMask(vk::AccessFlagBits::eShaderRead);
    commandBuffer.pipelineBarrier(
        vk::PipelineStageFlagBits::eTransfer,
        vk::PipelineStageFlagBits::eComputeShader |
            vk::PipelineStageFlagBits::eFragmentShader,
        vk::DependencyFlags(),
        nullptr,
        nullptr,
        toSampleBarrier);

    rendererBackend.EndSingleTimeCommands(commandBuffer);
    activePrefilterCube.layout = vk::ImageLayout::eShaderReadOnlyOptimal;
}

void EnvironmentIblBaker::PreparePendingPrefilterForCompute(
    vk::CommandBuffer commandBuffer)
{
    const vk::ImageLayout oldLayout = pendingPrefilterCube.layout;
    vk::ImageMemoryBarrier barrier;
    barrier
        .setOldLayout(oldLayout)
        .setNewLayout(vk::ImageLayout::eGeneral)
        .setSrcQueueFamilyIndex(VK_QUEUE_FAMILY_IGNORED)
        .setDstQueueFamilyIndex(VK_QUEUE_FAMILY_IGNORED)
        .setImage(pendingPrefilterCube.texture->getImage())
        .setSubresourceRange(BuildCubeRange(pendingPrefilterCube.mipLevels))
        .setSrcAccessMask(GetPendingPrefilterSourceAccess(oldLayout))
        .setDstAccessMask(vk::AccessFlagBits::eShaderWrite);
    commandBuffer.pipelineBarrier(
        GetPendingPrefilterSourceStage(oldLayout),
        vk::PipelineStageFlagBits::eComputeShader,
        vk::DependencyFlags(),
        nullptr,
        nullptr,
        barrier);
    pendingPrefilterCube.layout = vk::ImageLayout::eGeneral;
}

void EnvironmentIblBaker::DestroyPrefilteredCubeResources(
    RendererBackendVulkan& rendererBackend)
{
    DestroyPrefilteredCubeResource(rendererBackend, pendingPrefilterCube);
    DestroyPrefilteredCubeResource(rendererBackend, activePrefilterCube);
}

void EnvironmentIblBaker::DestroyPrefilteredCubeResource(
    RendererBackendVulkan& rendererBackend,
    PrefilteredCubeResources& resources)
{
    for (vk::ImageView& storageView : resources.storageViews)
    {
        rendererBackend.DestroyImageView(storageView);
    }
    resources.storageViews.clear();
    resources.texture.reset();
    resources.layout = vk::ImageLayout::eUndefined;
    resources.mipLevels = 1;
}

} // namespace VL
