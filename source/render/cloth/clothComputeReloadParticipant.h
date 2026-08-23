#pragma once

#include <atomic>
#include <memory>
#include <string>

#include "shader/reload/computePipelineReloadParticipant.h"

class PipelineFactory;

namespace VL
{

class RendererBackendVulkan;

// Cloth LUT 的 Compute pipeline、生成结果和 World-local resource package
// 必须在同一个 GT commit 中切换，避免 descriptor 采样旧 image。
class ClothComputeReloadParticipant final
    : public ComputePipelineReloadParticipant
{
public:
    ClothComputeReloadParticipant() = default;

    void Initialize(
        PipelineFactory& pipelineFactory,
        RendererBackendVulkan& rendererBackend);
    void Shutdown() noexcept;

    std::string GetShaderName() const override;
    std::shared_ptr<ComputePipeline> GetActivePipeline() const override;
    const ComputeShaderArtifact& GetActiveArtifact() const override;
    ComputeDescriptorReplacement PrepareReplacementDescriptors(
        const ComputeShaderArtifact& candidate,
        const std::shared_ptr<ComputePipeline>& replacementPipeline)
        const override;
    void CommitReplacement(
        ComputeShaderArtifact committedArtifact,
        std::shared_ptr<ComputePipeline> replacementPipeline,
        ComputeDescriptorReplacement&& replacementDescriptors)
        noexcept override;

    bool IsInitialized() const noexcept { return initialized; }
    bool NeedsDescriptorRefresh() const noexcept
    {
        return descriptorRefreshRequired.load(std::memory_order_acquire);
    }
    void MarkDescriptorRefreshHandled() noexcept
    {
        descriptorRefreshRequired.store(false, std::memory_order_release);
    }

private:
    PipelineFactory* pipelineFactory = nullptr;
    RendererBackendVulkan* rendererBackend = nullptr;
    std::shared_ptr<ComputePipeline> activePipeline;
    ComputeShaderArtifact activeArtifact;
    std::shared_ptr<const class ClothResourceSet> activeResourceSet;
    std::atomic_bool descriptorRefreshRequired{false};
    bool initialized = false;
};

} // namespace VL