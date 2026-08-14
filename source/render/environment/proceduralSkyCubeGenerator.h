#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include <vulkan/vulkan.hpp>

#include "baseStructs.h"
#include "shader/reload/computePipelineReloadParticipant.h"
#include "shader/reload/computeShaderArtifact.h"

class ComputePipeline;
class PipelineFactory;
class Texture;

namespace VL
{
class RendererBackendVulkan;

class ProceduralSkyCubeGenerator
    : public VL::ComputePipelineReloadParticipant
{
public:
    void Initialize(
        PipelineFactory& pipelineFactory,
        RendererBackendVulkan& rendererBackend);
    void Shutdown(RendererBackendVulkan& rendererBackend);

    bool IsInitialized() const { return initialized; }

    void RecordFace(
        vk::CommandBuffer commandBuffer,
        uint32_t swapchainImageIndex,
        const SkyParametersGPU& skyParameters,
        uint32_t faceIndex);
    void RecordCommit(vk::CommandBuffer commandBuffer);

    std::shared_ptr<Texture> GetActiveEnvironmentCube() const;
    std::shared_ptr<Texture> GetPendingEnvironmentCube() const;

    std::string GetShaderName() const override;
    std::shared_ptr<ComputePipeline> GetActivePipeline() const override;
    const ComputeShaderArtifact& GetActiveArtifact() const override;
    VL::ComputeDescriptorReplacement PrepareReplacementDescriptors(
        const ComputeShaderArtifact& candidate,
        const std::shared_ptr<ComputePipeline>& replacementPipeline)
        const override;
    void CommitReplacement(
        ComputeShaderArtifact committedArtifact,
        std::shared_ptr<ComputePipeline> replacementPipeline,
        VL::ComputeDescriptorReplacement&& replacementDescriptors)
        noexcept override;

private:
    struct SkyCubeResources
    {
        std::shared_ptr<Texture> texture;
        vk::ImageView storageView;
        uint32_t size = 128;
        vk::Format format = vk::Format::eR16G16B16A16Sfloat;
        vk::ImageLayout layout = vk::ImageLayout::eUndefined;
    };

    struct BufferResource
    {
        vk::Buffer buffer;
        vk::DeviceMemory memory;
        void* mapped = nullptr;
    };

    void CreateSkyCubeResources(RendererBackendVulkan& rendererBackend);
    void CreateSkyCubeResource(
        RendererBackendVulkan& rendererBackend,
        SkyCubeResources& resources,
        bool createStorageView,
        const std::string& debugName);
    void InitializeActiveCube(RendererBackendVulkan& rendererBackend);
    void DestroySkyCubeResources(RendererBackendVulkan& rendererBackend);
    void DestroySkyCubeResource(
        RendererBackendVulkan& rendererBackend,
        SkyCubeResources& resources);
    void CreateDescriptorResources(RendererBackendVulkan& rendererBackend);
    void DestroyDescriptorResources(RendererBackendVulkan& rendererBackend);
    void PreparePendingCubeForCompute(vk::CommandBuffer commandBuffer);
    void FinalizePendingCubeForSampling(vk::CommandBuffer commandBuffer);

    std::shared_ptr<ComputePipeline> skyToCubemapPipeline;
    ComputeShaderArtifact activeSkyToCubemapArtifact;
    PipelineFactory* pipelineFactoryService = nullptr;

    vk::DescriptorPool descriptorPool;
    std::vector<vk::DescriptorSet> skyToCubemapDescriptorSets;
    std::vector<BufferResource> dispatchParamBuffers;

    // active 的 image/view/sampler 地址在运行期保持不变；pending 完整后只复制像素内容。
    // 这样所有已建立的 graphics descriptor 都能继续安全引用 active。
    SkyCubeResources activeCube;
    SkyCubeResources pendingCube;

    RendererBackendVulkan* rendererBackend = nullptr;
    bool initialized = false;
};

} // namespace VL
