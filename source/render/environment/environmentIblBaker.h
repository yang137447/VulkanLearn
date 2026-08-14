#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include <vulkan/vulkan.hpp>

#include "shader/reload/computePipelineReloadParticipant.h"
#include "shader/reload/computeShaderArtifact.h"

class ComputePipeline;
class PipelineFactory;
class Texture;

namespace VL
{
class RendererBackendVulkan;

class EnvironmentIblBaker
{
public:
    void Initialize(
        PipelineFactory& pipelineFactory,
        RendererBackendVulkan& rendererBackend,
        const std::vector<vk::DescriptorBufferInfo>& globalUniformBufferInfos);
    void Shutdown(RendererBackendVulkan& rendererBackend);

    bool IsInitialized() const { return initialized; }

    void RecordSphericalHarmonics(
        vk::CommandBuffer commandBuffer,
        const std::shared_ptr<Texture>& environmentCube,
        uint32_t swapchainImageIndex);
    void RecordPrefilterMip(
        vk::CommandBuffer commandBuffer,
        const std::shared_ptr<Texture>& environmentCube,
        uint32_t swapchainImageIndex,
        uint32_t mipLevel);
    void RecordCommit(vk::CommandBuffer commandBuffer);

    std::shared_ptr<Texture> GetPrefilteredEnvironmentCube() const;
    void InvalidateEnvironmentCubeBindings() noexcept;
    uint32_t GetPrefilterMipCount() const { return pendingPrefilterCube.mipLevels; }

    const ComputeShaderArtifact& GetActiveSkySHGenerateArtifact() const
    {
        return activeSkySHGenerateArtifact;
    }
    const ComputeShaderArtifact& GetActivePrefilterEnvMapArtifact() const
    {
        return activePrefilterEnvMapArtifact;
    }
    std::shared_ptr<ComputePipeline> GetSkySHGeneratePipeline() const
    {
        return skySHGeneratePipeline;
    }
    std::shared_ptr<ComputePipeline> GetPrefilterEnvMapPipeline() const
    {
        return prefilterEnvMapPipeline;
    }
    PipelineFactory& GetPipelineFactory() const;

    ComputeDescriptorReplacement PrepareSkyShDescriptorReplacement(
        const std::shared_ptr<ComputePipeline>& replacementPipeline) const;
    void CommitSkyShReplacement(
        ComputeShaderArtifact committedArtifact,
        std::shared_ptr<ComputePipeline> replacementPipeline,
        ComputeDescriptorReplacement&& descriptors) noexcept;

    ComputeDescriptorReplacement PreparePrefilterDescriptorReplacement(
        const std::shared_ptr<ComputePipeline>& replacementPipeline) const;
    void CommitPrefilterReplacement(
        ComputeShaderArtifact committedArtifact,
        std::shared_ptr<ComputePipeline> replacementPipeline,
        ComputeDescriptorReplacement&& descriptors) noexcept;

private:
    enum class ShBufferAccess
    {
        Undefined,
        ComputeWrite,
        TransferRead
    };

    struct BufferResource
    {
        vk::Buffer buffer;
        vk::DeviceMemory memory;
    };

    struct PrefilteredCubeResources
    {
        std::shared_ptr<Texture> texture;
        std::vector<vk::ImageView> storageViews;
        uint32_t size = 128;
        uint32_t mipLevels = 1;
        vk::Format format = vk::Format::eR16G16B16A16Sfloat;
        vk::ImageLayout layout = vk::ImageLayout::eUndefined;
    };

    void CreateDescriptorResources(
        RendererBackendVulkan& rendererBackend,
        const std::vector<vk::DescriptorBufferInfo>& globalUniformBufferInfos);
    void EnsureEnvironmentCubeDescriptors(
        const std::shared_ptr<Texture>& environmentCube,
        uint32_t swapchainImageIndex);
    void CreateEnvironmentShResources(RendererBackendVulkan& rendererBackend);
    void DestroyEnvironmentShResources(RendererBackendVulkan& rendererBackend);
    void PrepareEnvironmentShOutputForCompute(vk::CommandBuffer commandBuffer);
    void BroadcastEnvironmentShToGlobalBuffers(vk::CommandBuffer commandBuffer);
    void DestroyDescriptorResources(RendererBackendVulkan& rendererBackend);
    void CreatePrefilteredCubeResources(RendererBackendVulkan& rendererBackend);
    void CreatePrefilteredCubeResource(
        RendererBackendVulkan& rendererBackend,
        PrefilteredCubeResources& resources,
        bool createStorageViews,
        const std::string& debugName);
    void InitializeActivePrefilteredCube(RendererBackendVulkan& rendererBackend);
    void PreparePendingPrefilterForCompute(vk::CommandBuffer commandBuffer);
    void DestroyPrefilteredCubeResources(RendererBackendVulkan& rendererBackend);
    void DestroyPrefilteredCubeResource(
        RendererBackendVulkan& rendererBackend,
        PrefilteredCubeResources& resources);

    std::shared_ptr<ComputePipeline> skySHGeneratePipeline;
    std::shared_ptr<ComputePipeline> prefilterEnvMapPipeline;
    ComputeShaderArtifact activeSkySHGenerateArtifact;
    ComputeShaderArtifact activePrefilterEnvMapArtifact;
    PipelineFactory* pipelineFactoryService = nullptr;

    vk::DescriptorPool skySHDescriptorPool;
    vk::DescriptorPool prefilterDescriptorPool;
    std::vector<vk::DescriptorSet> skySHGenerateDescriptorSets;
    std::vector<vk::DescriptorBufferInfo> globalUniformBufferInfos;
    std::vector<vk::DescriptorSet> prefilterDescriptorSets;
    std::vector<BufferResource> prefilterParamBuffers;
    std::vector<std::shared_ptr<Texture>> boundEnvironmentCubes;
    BufferResource environmentShOutputBuffer;
    ShBufferAccess environmentShAccess = ShBufferAccess::Undefined;

    // graphics descriptor 永远引用 active；所有 SH/prefilter 工作只写 pending。
    // commit 在同一个命令缓冲中复制完整结果并广播 SH，避免暴露半成品。
    PrefilteredCubeResources activePrefilterCube;
    PrefilteredCubeResources pendingPrefilterCube;

    RendererBackendVulkan* rendererBackend = nullptr;
    bool initialized = false;
};

} // namespace VL
