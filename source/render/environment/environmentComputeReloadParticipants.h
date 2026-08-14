#pragma once

// File responsibility: Adapts EnvironmentIblBaker's two long-lived compute
// pipelines to the unified compute reload participant contract. Each adapter
// owns one pipeline's descriptor package rebuild recipe.

#include "render/environment/environmentIblBaker.h"

namespace VL
{

class SkyShGenerateReloadParticipant final
    : public ComputePipelineReloadParticipant
{
public:
    explicit SkyShGenerateReloadParticipant(EnvironmentIblBaker& baker)
        : baker(baker)
    {
    }

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

private:
    EnvironmentIblBaker& baker;
};

class PrefilterEnvMapReloadParticipant final
    : public ComputePipelineReloadParticipant
{
public:
    explicit PrefilterEnvMapReloadParticipant(EnvironmentIblBaker& baker)
        : baker(baker)
    {
    }

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

private:
    EnvironmentIblBaker& baker;
};

} // namespace VL
