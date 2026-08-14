#pragma once

// File responsibility: Captures immutable live graphics reload recipes,
// compiles ABI-compatible CPU candidates, and commits a validated batch at an
// EngineLoop render-thread safe point.

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "material/materialDescriptorSchema.h"
#include "material/materialPipelineReload.h"
#include "shader/build/shaderBuildArtifact.h"
#include "shader/reload/computePipelineReloadParticipant.h"
#include "shader/reload/uiOverlayReloadParticipant.h"

class PipelineFactory;
class ShaderCompiler;

namespace VL
{

enum class ShaderReloadScope
{
    Changed,
    All
};

struct ShaderReloadBuildPlan
{
    ShaderBuildRequest request;
    std::string pipelineFactoryCacheKey;
    std::string displayName;
    GraphicsShaderVariantArtifact activeArtifact;
};

struct ComputeReloadBuildPlan
{
    ShaderBuildRequest request;
    std::string shaderName;
    ComputeShaderArtifact activeArtifact;
    ComputePipelineReloadParticipant* participant = nullptr;
};

struct UiReloadBuildPlan
{
    ShaderBuildRequest request;
    std::string displayName;
    GraphicsShaderVariantArtifact activeArtifact;
    UiOverlayReloadParticipant* participant = nullptr;
};

struct MaterialGraphicsReloadPlan
{
    std::string materialKey;
    MaterialPipelineReloadRecipe recipe;
    MaterialDescriptorSchema descriptorSchema;
    GraphicsShaderVariantArtifact activeSurfaceArtifact;
    std::optional<GraphicsShaderVariantArtifact> activeShadowArtifact;
    std::optional<size_t> surfaceBuildIndex;
    std::optional<size_t> shadowBuildIndex;
};

struct ShaderReloadPlan
{
    uint64_t generation = 0;
    uint64_t sourceEpoch = 0;
    uint64_t worldGeneration = 0;
    ShaderReloadScope scope = ShaderReloadScope::Changed;
    std::vector<std::string> changedSources;
    std::vector<ShaderReloadBuildPlan> builds;
    std::vector<MaterialGraphicsReloadPlan> materials;
    std::vector<ComputeReloadBuildPlan> computeBuilds;
    std::optional<UiReloadBuildPlan> uiBuild;
};

struct ShaderReloadCandidateBuild
{
    ShaderBuildArtifact buildArtifact;
    GraphicsShaderVariantArtifact graphicsArtifact;
    ComputeShaderArtifact computeArtifact;
    bool sourceGenerationChanged = false;
};

struct ComputeReloadCandidate
{
    ComputeReloadBuildPlan plan;
    ShaderBuildArtifact buildArtifact;
    ComputeShaderArtifact candidateArtifact;
    bool sourceGenerationChanged = false;
};

struct UiReloadCandidate
{
    UiReloadBuildPlan plan;
    ShaderBuildArtifact buildArtifact;
    GraphicsShaderVariantArtifact candidateArtifact;
    bool sourceGenerationChanged = false;
};

struct MaterialGraphicsReloadCandidate
{
    MaterialGraphicsReloadPlan plan;
    GraphicsShaderVariantArtifact surfaceArtifact;
    std::optional<GraphicsShaderVariantArtifact> shadowArtifact;
    std::vector<ShaderBinding> activeShaderBindings;
    bool replaceSurface = false;
    bool replaceShadow = false;
};

struct ShaderReloadCandidateBatch
{
    ShaderReloadPlan plan;
    std::vector<ShaderReloadCandidateBuild> builds;
    std::vector<MaterialGraphicsReloadCandidate> materials;
    std::vector<ComputeReloadCandidate> computeBuilds;
    std::optional<UiReloadCandidate> uiBuild;
    uint64_t shadercInvocations = 0;
};

struct ShaderReloadCommitStatistics
{
    uint64_t generation = 0;
    size_t changedSourceCount = 0;
    size_t affectedBuildCount = 0;
    size_t liveMaterialCount = 0;
    size_t compiledBuildCount = 0;
    uint64_t shadercInvocations = 0;
    size_t pipelinesCreated = 0;
    size_t pipelinesRetired = 0;
    bool committed = false;
};

// CPU-only candidate compilation shared by synchronous manual reload and the
// asynchronous compile worker. It performs shaderc compilation, reflection,
// schema validation, and ABI comparison. It must not touch Vulkan objects,
// live Material/PipelineFactory caches, or the disk manifest.
ShaderReloadCandidateBatch CompileGraphicsCandidates(
    ShaderCompiler& shaderCompiler,
    ShaderReloadPlan plan);

class ShaderReloadCoordinator
{
public:
    ShaderReloadCoordinator(
        ShaderCompiler& shaderCompiler,
        PipelineFactory& pipelineFactory);

    void RegisterComputeParticipant(
        ComputePipelineReloadParticipant* participant);
    void UnregisterComputeParticipant(
        ComputePipelineReloadParticipant* participant);
    void SetUiOverlayParticipant(
        UiOverlayReloadParticipant* participant);

    ShaderReloadPlan CaptureGraphicsPlan(
        ShaderReloadScope scope,
        uint64_t generation,
        uint64_t worldGeneration) const;
    ShaderReloadPlan CaptureGraphicsPlanForSources(
        const std::vector<std::string>& changedSources,
        uint64_t generation,
        uint64_t worldGeneration) const;
    ShaderReloadCandidateBatch CompileGraphicsCandidates(
        ShaderReloadPlan plan) const;
    ShaderReloadCommitStatistics CommitGraphicsCandidates(
        ShaderReloadCandidateBatch& batch,
        uint64_t currentWorldGeneration);

private:
    ShaderReloadPlan CaptureGraphicsPlanInternal(
        ShaderReloadScope scope,
        std::vector<std::string> changedSources,
        uint64_t generation,
        uint64_t worldGeneration) const;

    ShaderCompiler& shaderCompiler;
    PipelineFactory& pipelineFactory;
    std::vector<ComputePipelineReloadParticipant*> computeParticipants;
    UiOverlayReloadParticipant* uiOverlayParticipant = nullptr;
};

} // namespace VL
