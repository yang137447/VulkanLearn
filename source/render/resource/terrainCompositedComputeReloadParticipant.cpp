#include "render/resource/terrainCompositedComputeReloadParticipant.h"

#include <stdexcept>

#include "shader/reload/shaderReloadCoordinator.h"

namespace VL
{

class TerrainCompositedComputeReloadParticipants::Participant final :
    public ComputePipelineReloadParticipant
{
public:
    Participant(TerrainCompositedProducerReloadState& selectedState, size_t selectedEntry,
        ClosureLoader selectedLoader,
        TerrainCompositedProducerReloadState::DescriptorBuilder selectedBuilder)
        : state(selectedState), entry(selectedEntry), loadClosure(std::move(selectedLoader)),
          buildDescriptors(std::move(selectedBuilder))
    {
    }
    std::string GetShaderName() const override { return GetActiveArtifact().shaderName; }
    std::shared_ptr<ComputePipeline> GetActivePipeline() const override
    {
        return state.SelectedProducer()->Pipelines()[entry].pipeline;
    }
    const ComputeShaderArtifact& GetActiveArtifact() const override
    {
        return state.SelectedProducer()->Pipelines()[entry].artifact;
    }
    ComputeDescriptorReplacement PrepareReplacementDescriptors(
        const ComputeShaderArtifact&,
        const std::shared_ptr<ComputePipeline>&) const override
    {
        throw std::logic_error("Terrain producer reload requires the worker's frozen build context");
    }
    ComputeDescriptorReplacement PrepareReplacementDescriptors(
        const ComputeShaderArtifact& candidate,
        const std::shared_ptr<ComputePipeline>& pipeline,
        const ShaderBuildArtifact& closure, const ShaderBuildRequest& request) const override
    {
        auto prepared = state.Prepare(
            entry, loadClosure(candidate, pipeline, closure, request), buildDescriptors);
        ComputeDescriptorReplacement replacement;
        replacement.replacementPayload = std::move(prepared);
        return replacement;
    }
    void CommitReplacement(ComputeShaderArtifact,
        std::shared_ptr<ComputePipeline>, ComputeDescriptorReplacement&& replacement) noexcept override
    {
        state.Commit(std::static_pointer_cast<TerrainCompositedProducerReloadState::Prepared>(
            replacement.replacementPayload));
    }

private:
    TerrainCompositedProducerReloadState& state;
    size_t entry;
    ClosureLoader loadClosure;
    TerrainCompositedProducerReloadState::DescriptorBuilder buildDescriptors;
};

TerrainCompositedComputeReloadParticipants::TerrainCompositedComputeReloadParticipants(
    TerrainCompositedProducerReloadState& state, ClosureLoader loadClosure,
    TerrainCompositedProducerReloadState::DescriptorBuilder buildDescriptors)
{
    for (size_t index = 0; index < participants.size(); ++index)
        participants[index] = std::make_unique<Participant>(state, index, loadClosure, buildDescriptors);
}

TerrainCompositedComputeReloadParticipants::~TerrainCompositedComputeReloadParticipants()
{
    SetShaderReloadCoordinator(nullptr);
}

void TerrainCompositedComputeReloadParticipants::SetShaderReloadCoordinator(
    ShaderReloadCoordinator* selectedCoordinator)
{
    if (coordinator == selectedCoordinator)
        return;
    if (coordinator)
        for (auto& participant : participants)
            coordinator->UnregisterComputeParticipant(participant.get());
    coordinator = selectedCoordinator;
    if (coordinator)
        for (auto& participant : participants)
            coordinator->RegisterComputeParticipant(participant.get());
}

}
