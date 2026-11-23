#pragma once

#include "render/resource/terrainCompositedProducerSnapshot.h"
#include "shader/reload/computePipelineReloadParticipant.h"

namespace VL
{

class ShaderReloadCoordinator;

class TerrainCompositedComputeReloadParticipants
{
public:
    using ClosureLoader = std::function<TerrainCompositedProducerEntry(
        const ComputeShaderArtifact&, const std::shared_ptr<ComputePipeline>&,
        const ShaderBuildArtifact&, const ShaderBuildRequest&)>;
    TerrainCompositedComputeReloadParticipants(
        TerrainCompositedProducerReloadState& state,
        ClosureLoader loadClosure,
        TerrainCompositedProducerReloadState::DescriptorBuilder buildDescriptors);
    ~TerrainCompositedComputeReloadParticipants();
    void SetShaderReloadCoordinator(ShaderReloadCoordinator* coordinator);

private:
    class Participant;
    std::array<std::unique_ptr<Participant>, 2> participants;
    ShaderReloadCoordinator* coordinator = nullptr;
};

}
