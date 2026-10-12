#pragma once

#include <array>
#include <memory>
#include <vector>
#include <vulkan/vulkan.hpp>
#include "render/camera/autoExposureContract.h"
#include "shader/reload/computePipelineReloadParticipant.h"

class PipelineFactory;
class RenderGraph;

namespace VL
{
class RendererBackendVulkan;

// Graph candidate 拥有两条 ComputePipeline、临时 histogram 和跨 image 的 GPU 历史；
// 只消费冻结相机与图资源，不创建另一套 Material authoring 或进行 CPU 曝光读回。
class AutoExposureCompute
{
public:
    ~AutoExposureCompute();
    void Initialize(PipelineFactory& factory, RendererBackendVulkan& backend,
        const RenderGraph& graph);
    void Record(vk::CommandBuffer commandBuffer, uint32_t imageIndex,
        const CameraExposure& camera, float deltaSeconds);
    void OnFrameSubmitted() noexcept;
    std::shared_ptr<ComputePipeline> GetPipeline(size_t stage) const;
    const ComputeShaderArtifact& GetArtifact(size_t stage) const;
    ComputeDescriptorReplacement PrepareDescriptors(size_t stage,
        const std::shared_ptr<ComputePipeline>& pipeline) const;
    void CommitReplacement(size_t stage, ComputeShaderArtifact artifact,
        std::shared_ptr<ComputePipeline> pipeline,
        ComputeDescriptorReplacement&& descriptors) noexcept;
    static std::string ShaderName(size_t stage);

private:
    struct Buffer
    {
        vk::Buffer buffer;
        vk::DeviceMemory memory;
        void* mapped = nullptr;
    };
    struct ImageResources
    {
        Buffer params;
        Buffer histogram;
        vk::DescriptorImageInfo sceneColor;
        vk::DescriptorImageInfo output;
    };
    ComputeDescriptorReplacement BuildDescriptors(size_t stage,
        const std::shared_ptr<ComputePipeline>& pipeline) const;
    void DestroyBuffer(Buffer& buffer);

    RendererBackendVulkan* backend = nullptr;
    std::array<std::shared_ptr<ComputePipeline>, 2> pipelines;
    std::array<ComputeShaderArtifact, 2> artifacts;
    std::array<vk::DescriptorPool, 2> pools{};
    std::array<std::vector<vk::DescriptorSet>, 2> sets;
    std::vector<ImageResources> images;
    Buffer history;
    AutoExposureHistorySequence sequence;
    ExposureMode recordedMode = ExposureMode::Ev100;
    uint32_t width = 0;
    uint32_t height = 0;
};

// 适配器地址随 RenderSystem 稳定；实际 owner 随图事务替换。
// coordinator 的 generation 复核拒绝正在编译的旧图候选，pool/pipeline 仍按 epoch 退休。
class AutoExposureReloadParticipant final : public ComputePipelineReloadParticipant
{
public:
    explicit AutoExposureReloadParticipant(size_t stage) : stage(stage) {}
    std::string GetShaderName() const override;
    std::shared_ptr<ComputePipeline> GetActivePipeline() const override;
    const ComputeShaderArtifact& GetActiveArtifact() const override;
    ComputeDescriptorReplacement PrepareReplacementDescriptors(
        const ComputeShaderArtifact& candidate,
        const std::shared_ptr<ComputePipeline>& pipeline) const override;
    void CommitReplacement(ComputeShaderArtifact artifact,
        std::shared_ptr<ComputePipeline> pipeline,
        ComputeDescriptorReplacement&& descriptors) noexcept override;
private:
    size_t stage;
};
}
