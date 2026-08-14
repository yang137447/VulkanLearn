#include "render/environment/environmentComputeReloadParticipants.h"

#include "pipeline/computePipeline.h"
#include "pipeline/pipelineFactory.h"

namespace VL
{

std::string SkyShGenerateReloadParticipant::GetShaderName() const
{
    return "generator/skySHGenerate";
}

std::shared_ptr<ComputePipeline>
SkyShGenerateReloadParticipant::GetActivePipeline() const
{
    return baker.GetSkySHGeneratePipeline();
}

const ComputeShaderArtifact&
SkyShGenerateReloadParticipant::GetActiveArtifact() const
{
    return baker.GetActiveSkySHGenerateArtifact();
}

ComputeDescriptorReplacement
SkyShGenerateReloadParticipant::PrepareReplacementDescriptors(
    const ComputeShaderArtifact& candidate,
    const std::shared_ptr<ComputePipeline>& replacementPipeline) const
{
    (void)candidate;
    return baker.PrepareSkyShDescriptorReplacement(
        replacementPipeline);
}

void
SkyShGenerateReloadParticipant::CommitReplacement(
    ComputeShaderArtifact committedArtifact,
    std::shared_ptr<ComputePipeline> replacementPipeline,
    ComputeDescriptorReplacement&& replacementDescriptors) noexcept
{
    baker.CommitSkyShReplacement(
        std::move(committedArtifact),
        std::move(replacementPipeline),
        std::move(replacementDescriptors));
}

std::string PrefilterEnvMapReloadParticipant::GetShaderName() const
{
    return "generator/prefilterEnvMap";
}

std::shared_ptr<ComputePipeline>
PrefilterEnvMapReloadParticipant::GetActivePipeline() const
{
    return baker.GetPrefilterEnvMapPipeline();
}

const ComputeShaderArtifact&
PrefilterEnvMapReloadParticipant::GetActiveArtifact() const
{
    return baker.GetActivePrefilterEnvMapArtifact();
}

ComputeDescriptorReplacement
PrefilterEnvMapReloadParticipant::PrepareReplacementDescriptors(
    const ComputeShaderArtifact& candidate,
    const std::shared_ptr<ComputePipeline>& replacementPipeline) const
{
    (void)candidate;
    return baker.PreparePrefilterDescriptorReplacement(
        replacementPipeline);
}

void
PrefilterEnvMapReloadParticipant::CommitReplacement(
    ComputeShaderArtifact committedArtifact,
    std::shared_ptr<ComputePipeline> replacementPipeline,
    ComputeDescriptorReplacement&& replacementDescriptors) noexcept
{
    baker.CommitPrefilterReplacement(
        std::move(committedArtifact),
        std::move(replacementPipeline),
        std::move(replacementDescriptors));
}

} // namespace VL
