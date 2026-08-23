#pragma once

#include <atomic>
#include <memory>
#include <string>

#include "shader/reload/computePipelineReloadParticipant.h"

class PipelineFactory;

namespace VL
{

class RendererBackendVulkan;

// Eye LUT 不是独立于 shader 的静态贴图：generator ABI、profile 输入和
// 生成结果必须在同一 GT commit 中切换。该 participant 负责把 Eye 的
// Compute pipeline 与 World-local LUT/resource package 绑定到统一热重载事务。
class EyeComputeReloadParticipant final
    : public ComputePipelineReloadParticipant
{
public:
    EyeComputeReloadParticipant() = default;

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
    std::shared_ptr<const class EyeResourceSet> activeResourceSet;
    std::atomic_bool descriptorRefreshRequired{false};
    bool initialized = false;
};

} // namespace VL