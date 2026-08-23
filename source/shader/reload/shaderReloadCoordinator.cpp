#include "shader/reload/shaderReloadCoordinator.h"

#include <algorithm>
#include <map>
#include <sstream>
#include <stdexcept>
#include <type_traits>
#include <unordered_map>
#include <unordered_set>
#include <utility>

#include "material.h"
#include "material/compiler/materialShaderComposer.h"
#include "materialInstance.h"
#include "material/materialPipelineLayout.h"
#include "pipeline/computePipeline.h"
#include "pipeline/graphicsPipelineLayoutDesc.h"
#include "pipeline/pipelineFactory.h"
#include "render/resource/rendererResourceCache.h"
#include "render/resource/resourceRetireQueue.h"
#include "render/shadow/materialShadowPipelineBuilder.h"
#include "renderGraph.h"
#include "shaderCompiler.h"
#include "shader/reload/computeShaderArtifact.h"

namespace VL
{
namespace
{

static_assert(
    std::is_nothrow_move_constructible_v<GraphicsShaderVariantArtifact>);
static_assert(
    std::is_nothrow_move_assignable_v<GraphicsShaderVariantArtifact>);
static_assert(
    std::is_nothrow_move_constructible_v<ComputeShaderArtifact>);
static_assert(
    std::is_nothrow_move_assignable_v<ComputeShaderArtifact>);
static_assert(
    std::is_nothrow_move_constructible_v<MaterialPipelineReloadCommit>);
static_assert(
    std::is_nothrow_move_constructible_v<ComputeDescriptorReplacement>);
static_assert(
    std::is_nothrow_move_constructible_v<UiOverlayPipelineReplacement>);

ShaderBuildRequest BuildShaderRequest(
    ShaderCompiler& shaderCompiler,
    const GraphicsShaderReloadRecipe& recipe)
{
    if (recipe.kind == GraphicsShaderReloadRecipeKind::StandaloneVariant)
    {
        return shaderCompiler.CreateGraphicsVariantBuildRequest(
            recipe.shaderVariantKey);
    }
    if (!recipe.materialCompileRequest.has_value())
    {
        throw std::runtime_error(
            "Material-composed shader reload recipe has no compile request");
    }

    const ComposedMaterialShaderSource source =
        MaterialShaderComposer::Compose(*recipe.materialCompileRequest);
    return shaderCompiler.CreateMaterialGraphicsBuildRequest(
        *recipe.materialCompileRequest,
        source);
}

std::string BuildPipelineFactoryCacheKey(
    const GraphicsShaderReloadRecipe& recipe)
{
    if (recipe.kind == GraphicsShaderReloadRecipeKind::StandaloneVariant)
    {
        return PipelineFactory::GetGraphicsShaderVariantCacheKey(
            recipe.shaderVariantKey);
    }
    if (!recipe.materialCompileRequest.has_value())
    {
        throw std::runtime_error(
            "Material-composed shader reload recipe has no compile request");
    }
    return PipelineFactory::GetMaterialShaderVariantCacheKey(
        *recipe.materialCompileRequest);
}

std::string BuildShaderDisplayName(
    const GraphicsShaderReloadRecipe& recipe)
{
    if (recipe.kind == GraphicsShaderReloadRecipeKind::StandaloneVariant)
    {
        return recipe.shaderVariantKey.GetDisplayName();
    }
    return recipe.materialCompileRequest->GetDisplayName();
}

std::vector<std::shared_ptr<Material>> CaptureLiveMaterials()
{
    const RendererResourceCache::ImmutableWorldLocalResourceRefs
        resourceSnapshot =
            RendererResourceCache::GetInstance()
                .CaptureActiveWorldLocalResources();
    std::map<std::string, std::shared_ptr<Material>> materialsByKey;
    if (resourceSnapshot)
    {
        for (const auto& resourceEntry :
             resourceSnapshot->materials)
        {
            const std::shared_ptr<Material>& material =
                resourceEntry.second;
            if (material)
            {
                materialsByKey[material->GetMaterialKey()] = material;
            }
        }
    }

    const auto passMaterials =
        RenderGraph::GetInstance().CapturePassMaterialInstances();
    for (const auto& passEntry : passMaterials)
    {
        const std::shared_ptr<MaterialInstance>& materialInstance =
            passEntry.second;
        if (!materialInstance)
        {
            continue;
        }
        std::shared_ptr<Material> material =
            materialInstance->GetBaseMaterial().lock();
        if (material)
        {
            materialsByKey[material->GetMaterialKey()] =
                std::move(material);
        }
    }

    std::vector<std::shared_ptr<Material>> materials;
    materials.reserve(materialsByKey.size());
    for (auto& materialEntry : materialsByKey)
    {
        materials.push_back(std::move(materialEntry.second));
    }
    return materials;
}

std::string FormatAbiRejection(
    const std::string& displayName,
    const ShaderAbiSignature& active,
    const ShaderAbiSignature& candidate)
{
    std::ostringstream stream;
    stream << "Shader reload rejected because ABI changed: "
           << displayName;
    for (const std::string& difference :
         active.DescribeDifferences(candidate))
    {
        stream << "\n  " << difference;
    }
    return stream.str();
}


Renderpass& ResolveCurrentPass(
    const MaterialGraphicsPassReloadRecipe& recipe)
{
    auto& renderpasses =
        RenderGraph::GetInstance().GetRenderpasses();
    const auto passIt = renderpasses.find(recipe.passName);
    if (passIt == renderpasses.end())
    {
        throw std::runtime_error(
            "Shader reload pass no longer exists: " +
            recipe.passName);
    }
    if (passIt->second.pipelineContractKey !=
        recipe.passPipelineContractKey)
    {
        throw std::runtime_error(
            "Shader reload pass contract changed since the Material recipe was captured: " +
            recipe.passName);
    }
    return passIt->second;
}

std::shared_ptr<Material> ResolveCurrentMaterial(
    const MaterialGraphicsReloadPlan& plan)
{
    const std::shared_ptr<Material>* material =
        RendererResourceCache::GetInstance().GetMaterial(
            plan.materialKey);
    if (material == nullptr || !*material)
    {
        throw std::runtime_error(
            "Shader reload Material is no longer live: " +
            plan.materialKey);
    }
    if ((*material)->GetSurfaceShaderArtifact()
            .artifactGenerationKey !=
        plan.activeSurfaceArtifact.artifactGenerationKey)
    {
        throw std::runtime_error(
            "Shader reload Material Surface generation changed while the batch was compiling: " +
            plan.materialKey);
    }

    const auto& currentShadow =
        (*material)->GetShadowShaderArtifact();
    if (currentShadow.has_value() !=
        plan.activeShadowArtifact.has_value())
    {
        throw std::runtime_error(
            "Shader reload Material Shadow route changed while the batch was compiling: " +
            plan.materialKey);
    }
    if (currentShadow &&
        currentShadow->artifactGenerationKey !=
            plan.activeShadowArtifact->artifactGenerationKey)
    {
        throw std::runtime_error(
            "Shader reload Material Shadow generation changed while the batch was compiling: " +
            plan.materialKey);
    }
    return *material;
}

size_t FindOrAddBuild(
    ShaderReloadPlan& plan,
    std::unordered_map<std::string, size_t>& buildIndices,
    ShaderBuildRequest request,
    const GraphicsShaderReloadRecipe& recipe,
    const GraphicsShaderVariantArtifact& activeArtifact)
{
    const auto buildIt = buildIndices.find(
        request.logicalBuildId);
    if (buildIt != buildIndices.end())
    {
        const ShaderReloadBuildPlan& existing =
            plan.builds.at(buildIt->second);
        if (existing.activeArtifact.artifactGenerationKey !=
            activeArtifact.artifactGenerationKey)
        {
            throw std::runtime_error(
                "Live Materials disagree about the active generation of one logical shader");
        }
        return buildIt->second;
    }

    const size_t buildIndex = plan.builds.size();
    buildIndices.emplace(request.logicalBuildId, buildIndex);
    ShaderReloadBuildPlan buildPlan;
    buildPlan.request = std::move(request);
    buildPlan.pipelineFactoryCacheKey =
        BuildPipelineFactoryCacheKey(recipe);
    buildPlan.displayName =
        BuildShaderDisplayName(recipe);
    buildPlan.activeArtifact = activeArtifact;
    plan.builds.push_back(std::move(buildPlan));
    return buildIndex;
}

} // namespace

ShaderReloadCoordinator::ShaderReloadCoordinator(
    ShaderCompiler& shaderCompiler,
    PipelineFactory& pipelineFactory)
    : shaderCompiler(shaderCompiler)
    , pipelineFactory(pipelineFactory)
{
}

void ShaderReloadCoordinator::RegisterComputeParticipant(
    ComputePipelineReloadParticipant* participant)
{
    if (participant == nullptr)
    {
        return;
    }
    for (ComputePipelineReloadParticipant* existing :
         computeParticipants)
    {
        if (existing == participant)
        {
            return;
        }
    }
    computeParticipants.push_back(participant);
}

void ShaderReloadCoordinator::UnregisterComputeParticipant(
    ComputePipelineReloadParticipant* participant)
{
    computeParticipants.erase(
        std::remove(
            computeParticipants.begin(),
            computeParticipants.end(),
            participant),
        computeParticipants.end());
}

void ShaderReloadCoordinator::SetUiOverlayParticipant(
    UiOverlayReloadParticipant* participant)
{
    uiOverlayParticipant = participant;
}

ShaderReloadPlan ShaderReloadCoordinator::CaptureGraphicsPlan(
    ShaderReloadScope scope,
    uint64_t generation,
    uint64_t worldGeneration) const
{
    std::vector<std::string> changedSources;
    if (scope == ShaderReloadScope::Changed)
    {
        changedSources =
            shaderCompiler.FindChangedSourcePaths();
    }
    return CaptureGraphicsPlanInternal(
        scope,
        std::move(changedSources),
        generation,
        worldGeneration);
}

ShaderReloadPlan
ShaderReloadCoordinator::CaptureGraphicsPlanForSources(
    const std::vector<std::string>& changedSources,
    uint64_t generation,
    uint64_t worldGeneration) const
{
    return CaptureGraphicsPlanInternal(
        ShaderReloadScope::Changed,
        changedSources,
        generation,
        worldGeneration);
}

ShaderReloadPlan ShaderReloadCoordinator::CaptureGraphicsPlanInternal(
    ShaderReloadScope scope,
    std::vector<std::string> changedSources,
    uint64_t generation,
    uint64_t worldGeneration) const
{
    std::sort(changedSources.begin(), changedSources.end());
    changedSources.erase(
        std::unique(
            changedSources.begin(),
            changedSources.end()),
        changedSources.end());

    std::unordered_set<std::string> affectedLogicalBuildIds;
    if (scope == ShaderReloadScope::Changed)
    {
        for (const std::string& changedSource :
             changedSources)
        {
            const std::vector<std::string> logicalBuildIds =
                shaderCompiler.FindLogicalBuildIdsDependingOn(
                    changedSource);
            affectedLogicalBuildIds.insert(
                logicalBuildIds.begin(),
                logicalBuildIds.end());
        }
    }

    ShaderReloadPlan plan;
    plan.generation = generation;
    plan.sourceEpoch = 0;
    plan.worldGeneration = worldGeneration;
    plan.scope = scope;
    plan.changedSources = std::move(changedSources);

    std::unordered_map<std::string, size_t> buildIndices;
    const std::vector<std::shared_ptr<Material>> liveMaterials =
        CaptureLiveMaterials();
    for (const std::shared_ptr<Material>& material :
         liveMaterials)
    {
        const MaterialPipelineReloadRecipe& recipe =
            material->GetPipelineReloadRecipe();
        const GraphicsShaderVariantArtifact& activeSurface =
            material->GetSurfaceShaderArtifact();

        ShaderBuildRequest surfaceRequest =
            BuildShaderRequest(
                shaderCompiler,
                recipe.surface.shader);
        shaderCompiler.FreezeSourceSnapshot(surfaceRequest);
        const bool surfaceAffected =
            scope == ShaderReloadScope::All ||
            affectedLogicalBuildIds.count(
                surfaceRequest.logicalBuildId) != 0;

        std::optional<ShaderBuildRequest> shadowRequest;
        bool shadowAffected = false;
        if (recipe.shadow)
        {
            shadowRequest = BuildShaderRequest(
                shaderCompiler,
                recipe.shadow->shader);
            shaderCompiler.FreezeSourceSnapshot(*shadowRequest);
            shadowAffected =
                scope == ShaderReloadScope::All ||
                affectedLogicalBuildIds.count(
                    shadowRequest->logicalBuildId) != 0;
        }

        if (!surfaceAffected && !shadowAffected)
        {
            continue;
        }

        MaterialGraphicsReloadPlan materialPlan;
        materialPlan.materialKey =
            material->GetMaterialKey();
        materialPlan.recipe = recipe;
        materialPlan.descriptorSchema =
            material->GetMaterialDescriptorSchema();
        materialPlan.activeSurfaceArtifact =
            activeSurface;
        materialPlan.activeShadowArtifact =
            material->GetShadowShaderArtifact();

        if (surfaceAffected)
        {
            materialPlan.surfaceBuildIndex =
                FindOrAddBuild(
                    plan,
                    buildIndices,
                    std::move(surfaceRequest),
                    recipe.surface.shader,
                    activeSurface);
        }

        if (shadowAffected)
        {
            if (!materialPlan.activeShadowArtifact)
            {
                throw std::runtime_error(
                    "Material Shadow reload recipe has no active artifact: " +
                    materialPlan.materialKey);
            }
            materialPlan.shadowBuildIndex =
                FindOrAddBuild(
                    plan,
                    buildIndices,
                    std::move(*shadowRequest),
                    recipe.shadow->shader,
                    *materialPlan.activeShadowArtifact);
        }

        plan.materials.push_back(
            std::move(materialPlan));
    }

    for (ComputePipelineReloadParticipant* participant :
         computeParticipants)
    {
        if (participant == nullptr)
        {
            continue;
        }

        ShaderBuildRequest request =
            shaderCompiler.CreateComputeStageBuildRequestForShaderName(
                participant->GetShaderName());
        shaderCompiler.FreezeSourceSnapshot(request);
        const bool affected =
            scope == ShaderReloadScope::All ||
            affectedLogicalBuildIds.count(
                request.logicalBuildId) != 0;
        if (!affected)
        {
            continue;
        }

        ComputeReloadBuildPlan computePlan;
        computePlan.request = std::move(request);
        computePlan.shaderName = participant->GetShaderName();
        computePlan.activeArtifact =
            participant->GetActiveArtifact();
        computePlan.participant = participant;
        plan.computeBuilds.push_back(std::move(computePlan));
    }

    if (uiOverlayParticipant != nullptr)
    {
        ShaderVariantKey uiVariant;
        uiVariant.shaderName =
            uiOverlayParticipant->GetShaderName();
        ShaderBuildRequest request =
            shaderCompiler.CreateGraphicsVariantBuildRequest(
                uiVariant);
        shaderCompiler.FreezeSourceSnapshot(request);
        const bool affected =
            scope == ShaderReloadScope::All ||
            affectedLogicalBuildIds.count(
                request.logicalBuildId) != 0;
        if (affected)
        {
            UiReloadBuildPlan uiPlan;
            uiPlan.request = std::move(request);
            uiPlan.displayName = uiVariant.GetDisplayName();
            uiPlan.activeArtifact =
                uiOverlayParticipant->GetActiveArtifact();
            uiPlan.participant = uiOverlayParticipant;
            plan.uiBuild = std::move(uiPlan);
        }
    }
    return plan;
}

ShaderReloadCandidateBatch
ShaderReloadCoordinator::CompileGraphicsCandidates(
    ShaderReloadPlan plan) const
{
    return VL::CompileGraphicsCandidates(
        shaderCompiler,
        std::move(plan));
}

ShaderReloadCandidateBatch
CompileGraphicsCandidates(
    ShaderCompiler& shaderCompiler,
    ShaderReloadPlan plan)
{
    ShaderReloadCandidateBatch batch;
    batch.plan = std::move(plan);
    batch.builds.reserve(batch.plan.builds.size());
    for (const ShaderReloadBuildPlan& buildPlan :
         batch.plan.builds)
    {
        ShaderReloadCandidateBuild candidate;
        candidate.buildArtifact =
            shaderCompiler.CompileCandidate(
                buildPlan.request);
        batch.shadercInvocations +=
            candidate.buildArtifact.shadercInvocations;
        candidate.graphicsArtifact =
            BuildGraphicsShaderVariantArtifact(
                candidate.buildArtifact,
                buildPlan.displayName);
        candidate.sourceGenerationChanged =
            candidate.graphicsArtifact.artifactGenerationKey !=
            buildPlan.activeArtifact.artifactGenerationKey;
        if (candidate.sourceGenerationChanged &&
            candidate.graphicsArtifact.abiSignature !=
                buildPlan.activeArtifact.abiSignature)
        {
            throw std::runtime_error(
                FormatAbiRejection(
                    buildPlan.displayName,
                    buildPlan.activeArtifact.abiSignature,
                    candidate.graphicsArtifact.abiSignature));
        }
        batch.builds.push_back(
            std::move(candidate));
    }

    batch.materials.reserve(batch.plan.materials.size());
    for (const MaterialGraphicsReloadPlan& materialPlan :
         batch.plan.materials)
    {
        MaterialGraphicsReloadCandidate candidate;
        candidate.plan = materialPlan;
        candidate.surfaceArtifact =
            materialPlan.activeSurfaceArtifact;
        candidate.shadowArtifact =
            materialPlan.activeShadowArtifact;

        if (materialPlan.surfaceBuildIndex)
        {
            const ShaderReloadCandidateBuild& build =
                batch.builds.at(
                    *materialPlan.surfaceBuildIndex);
            candidate.replaceSurface =
                build.sourceGenerationChanged;
            if (candidate.replaceSurface)
            {
                candidate.surfaceArtifact =
                    build.graphicsArtifact;
            }
        }
        if (materialPlan.shadowBuildIndex)
        {
            const ShaderReloadCandidateBuild& build =
                batch.builds.at(
                    *materialPlan.shadowBuildIndex);
            candidate.replaceShadow =
                build.sourceGenerationChanged;
            if (candidate.replaceShadow)
            {
                candidate.shadowArtifact =
                    build.graphicsArtifact;
            }
        }

        materialPlan.descriptorSchema.ValidateShaderBindings(
            candidate.surfaceArtifact.shaderBindings,
            candidate.surfaceArtifact.displayName);
        if (candidate.shadowArtifact)
        {
            ValidateMaterialShadowShaderBindings(
                materialPlan.materialKey,
                materialPlan.descriptorSchema,
                candidate.surfaceArtifact.shaderBindings,
                candidate.shadowArtifact->shaderBindings);
            candidate.activeShaderBindings =
                Material::BuildActiveShaderBindings(
                    candidate.surfaceArtifact.shaderBindings,
                    &candidate.shadowArtifact->shaderBindings);
        }
        else
        {
            candidate.activeShaderBindings =
                Material::BuildActiveShaderBindings(
                    candidate.surfaceArtifact.shaderBindings,
                    nullptr);
        }

        if (candidate.replaceSurface ||
            candidate.replaceShadow)
        {
            batch.materials.push_back(
                std::move(candidate));
        }
    }

    // Compute and UI candidates stay pure CPU artifacts until the GT-side
    // commit. ABI validation against live participants happens there, never on
    // the compile worker, so the worker owns no live-cache references.
    batch.computeBuilds.reserve(batch.plan.computeBuilds.size());
    for (const ComputeReloadBuildPlan& buildPlan :
         batch.plan.computeBuilds)
    {
        ComputeReloadCandidate candidate;
        candidate.plan = buildPlan;
        candidate.buildArtifact =
            shaderCompiler.CompileCandidate(buildPlan.request);
        batch.shadercInvocations +=
            candidate.buildArtifact.shadercInvocations;
        candidate.candidateArtifact =
            BuildComputeShaderArtifact(
                candidate.buildArtifact,
                buildPlan.shaderName);
        candidate.sourceGenerationChanged =
            candidate.candidateArtifact.artifactGenerationKey !=
            buildPlan.activeArtifact.artifactGenerationKey;
        batch.computeBuilds.push_back(
            std::move(candidate));
    }

    if (batch.plan.uiBuild)
    {
        const UiReloadBuildPlan& uiPlan = *batch.plan.uiBuild;
        UiReloadCandidate candidate;
        candidate.plan = uiPlan;
        candidate.buildArtifact =
            shaderCompiler.CompileCandidate(uiPlan.request);
        batch.shadercInvocations +=
            candidate.buildArtifact.shadercInvocations;
        candidate.candidateArtifact =
            BuildGraphicsShaderVariantArtifact(
                candidate.buildArtifact,
                uiPlan.displayName);
        candidate.sourceGenerationChanged =
            candidate.candidateArtifact.artifactGenerationKey !=
            uiPlan.activeArtifact.artifactGenerationKey;
        batch.uiBuild = std::move(candidate);
    }
    return batch;
}

ShaderReloadCommitStatistics
ShaderReloadCoordinator::CommitGraphicsCandidates(
    ShaderReloadCandidateBatch& batch,
    uint64_t currentWorldGeneration)
{
    if (batch.plan.worldGeneration !=
        currentWorldGeneration)
    {
        throw std::runtime_error(
            "Shader reload batch became stale because the active World changed");
    }

    ShaderReloadCommitStatistics statistics;
    statistics.generation = batch.plan.generation;
    statistics.changedSourceCount =
        batch.plan.changedSources.size();
    statistics.affectedBuildCount =
        batch.plan.builds.size() +
        batch.plan.computeBuilds.size() +
        (batch.plan.uiBuild.has_value() ? 1u : 0u);
    statistics.liveMaterialCount =
        batch.materials.size();
    statistics.compiledBuildCount =
        batch.builds.size() +
        batch.computeBuilds.size() +
        (batch.uiBuild.has_value() ? 1u : 0u);
    statistics.shadercInvocations =
        batch.shadercInvocations;

    auto validateBuild = [this](
        const ShaderBuildRequest& request,
        const ShaderBuildArtifact& artifact)
    {
        const ShaderCompiler::CandidateSourceValidationResult validation =
            shaderCompiler.ValidateCandidateSourcesStillCurrent(
                request,
                artifact);
        if (!validation.current)
        {
            throw std::runtime_error(
                "Shader reload candidate source validation failed: " +
                validation.reason +
                "; source=" + validation.sourceIdentity +
                "; capturedDigest=" + validation.capturedDigest +
                "; currentDigest=" + validation.currentDigest);
        }
    };

    for (size_t buildIndex = 0;
         buildIndex < batch.builds.size();
         ++buildIndex)
    {
        validateBuild(
            batch.plan.builds[buildIndex].request,
            batch.builds[buildIndex].buildArtifact);
    }
    for (const ComputeReloadCandidate& candidate :
         batch.computeBuilds)
    {
        validateBuild(
            candidate.plan.request,
            candidate.buildArtifact);
    }
    if (batch.uiBuild)
    {
        validateBuild(
            batch.uiBuild->plan.request,
            batch.uiBuild->buildArtifact);
    }

    bool hasComputeCommits = false;
    for (const ComputeReloadCandidate& candidate :
         batch.computeBuilds)
    {
        if (candidate.sourceGenerationChanged)
        {
            hasComputeCommits = true;
            break;
        }
    }
    const bool hasUiCommit =
        batch.uiBuild.has_value() &&
        batch.uiBuild->sourceGenerationChanged;
    if (batch.materials.empty() &&
        !hasComputeCommits &&
        !hasUiCommit)
    {
        statistics.committed = true;
        return statistics;
    }

    struct PendingMaterialCommit
    {
        std::shared_ptr<Material> material;
        MaterialPipelineReloadCommit commit;
    };

    PipelineFactory::GraphicsCandidateState
        graphicsCandidateState;
    std::vector<PendingMaterialCommit> pendingCommits;
    pendingCommits.reserve(batch.materials.size());
    for (MaterialGraphicsReloadCandidate& candidate :
         batch.materials)
    {
        PendingMaterialCommit pending;
        pending.material =
            ResolveCurrentMaterial(candidate.plan);
        pending.commit.replaceSurface =
            candidate.replaceSurface;
        pending.commit.replaceShadow =
            candidate.replaceShadow;
        pending.commit.surfaceArtifact =
            candidate.surfaceArtifact;
        pending.commit.shadowArtifact =
            candidate.shadowArtifact;
        pending.commit.activeShaderBindings =
            candidate.activeShaderBindings;

        if (candidate.replaceSurface)
        {
            Renderpass& surfacePass =
                ResolveCurrentPass(
                    candidate.plan.recipe.surface);
            pending.commit.surfacePipeline =
                pipelineFactory.CreateGraphicsPipelineCandidate(
                    graphicsCandidateState,
                    &surfacePass.renderPass,
                    surfacePass.pipelineContractKey,
                    candidate.surfaceArtifact,
                    candidate.plan.recipe.surface.cullMode,
                    candidate.plan.recipe.surface.blendMode,
                    BuildMaterialSurfacePipelineLayout(
                        surfacePass,
                        candidate.plan.descriptorSchema,
                        candidate.surfaceArtifact.shaderBindings));
            ++statistics.pipelinesCreated;
        }

        if (candidate.replaceShadow)
        {
            Renderpass& shadowPass =
                ResolveCurrentPass(
                    *candidate.plan.recipe.shadow);
            pending.commit.shadowPipeline =
                pipelineFactory.CreateGraphicsPipelineCandidate(
                    graphicsCandidateState,
                    &shadowPass.renderPass,
                    shadowPass.pipelineContractKey,
                    *candidate.shadowArtifact,
                    candidate.plan.recipe.shadow->cullMode,
                    candidate.plan.recipe.shadow->blendMode,
                    BuildMaterialShadowPipelineLayout(
                        candidate.plan.descriptorSchema,
                        candidate.surfaceArtifact.shaderBindings));
            ++statistics.pipelinesCreated;
        }
        pendingCommits.push_back(
            std::move(pending));
    }

    // Compute and UI replacements are fully prevalidated and prepared before
    // any disk publication or live swap, keeping the batch all-or-nothing.
    struct PendingComputeCommit
    {
        const ComputeReloadCandidate* candidate = nullptr;
        ComputeShaderArtifact committedArtifact;
        std::shared_ptr<ComputePipeline> replacementPipeline;
        ComputeDescriptorReplacement descriptors;
    };
    std::vector<PendingComputeCommit> pendingComputeCommits;
    std::optional<UiOverlayPipelineReplacement>
        pendingUiReplacement;
    std::optional<GraphicsShaderVariantArtifact>
        uiCommittedArtifact;
    std::optional<
        PipelineFactory::PreparedGraphicsCandidateCommit>
        preparedGraphicsCommit;
    std::vector<ShaderBuildArtifact> artifactsToCommit;
    std::vector<GraphicsShaderArtifactPublication> publications;
    PreparedResourceRetirements preparedRetirements;
    size_t preparedRetirementCount = 0;
    try
    {
        for (const ComputeReloadCandidate& candidate :
             batch.computeBuilds)
        {
            if (!candidate.sourceGenerationChanged)
            {
                continue;
            }
            ComputePipelineReloadParticipant* participant =
                candidate.plan.participant;
            if (participant == nullptr)
            {
                throw std::runtime_error(
                    "Compute shader reload candidate lost its participant");
            }
            if (participant->GetActiveArtifact().artifactGenerationKey !=
                candidate.plan.activeArtifact.artifactGenerationKey)
            {
                throw std::runtime_error(
                    "Compute shader reload became stale: " +
                    candidate.plan.shaderName);
            }
            participant->ValidateCandidateAbi(
                candidate.candidateArtifact);

            std::shared_ptr<ComputePipeline> replacement =
                pipelineFactory.CreateComputePipeline(
                    candidate.candidateArtifact);
            ComputeDescriptorReplacement descriptors =
                participant->PrepareReplacementDescriptors(
                    candidate.candidateArtifact,
                    replacement);
            pendingComputeCommits.push_back({
                &candidate,
                candidate.candidateArtifact,
                std::move(replacement),
                std::move(descriptors)});
            ++statistics.pipelinesCreated;
        }

        if (batch.uiBuild &&
            batch.uiBuild->sourceGenerationChanged)
        {
            UiReloadCandidate& candidate = *batch.uiBuild;
            if (candidate.plan.participant == nullptr)
            {
                throw std::runtime_error(
                    "UI overlay reload candidate lost its participant");
            }
            if (candidate.plan.participant
                    ->GetActiveArtifact()
                    .artifactGenerationKey !=
                candidate.plan.activeArtifact.artifactGenerationKey)
            {
                throw std::runtime_error(
                    "UI overlay reload became stale");
            }
            if (candidate.candidateArtifact.abiSignature !=
                candidate.plan.activeArtifact.abiSignature)
            {
                throw std::runtime_error(
                    FormatAbiRejection(
                        candidate.plan.participant->GetShaderName(),
                        candidate.plan.activeArtifact.abiSignature,
                        candidate.candidateArtifact.abiSignature));
            }
            pendingUiReplacement =
                candidate.plan.participant
                    ->PrepareReplacementPipelines(
                        candidate.candidateArtifact);
            uiCommittedArtifact = candidate.candidateArtifact;
            ++statistics.pipelinesCreated;
        }

        for (size_t buildIndex = 0;
             buildIndex < batch.builds.size();
             ++buildIndex)
        {
            ShaderReloadCandidateBuild& build =
                batch.builds[buildIndex];
            if (!build.sourceGenerationChanged)
            {
                continue;
            }
            artifactsToCommit.push_back(
                std::move(build.buildArtifact));
            publications.push_back({
                batch.plan.builds[buildIndex]
                    .pipelineFactoryCacheKey,
                build.graphicsArtifact});
        }
        for (ComputeReloadCandidate& candidate :
             batch.computeBuilds)
        {
            if (candidate.sourceGenerationChanged)
            {
                artifactsToCommit.push_back(
                    std::move(candidate.buildArtifact));
            }
        }
        if (batch.uiBuild &&
            batch.uiBuild->sourceGenerationChanged)
        {
            artifactsToCommit.push_back(
                std::move(batch.uiBuild->buildArtifact));
        }

        pipelineFactory.ValidateGraphicsShaderVariantArtifactPublications(
            publications);
        graphicsCandidateState.publications =
            publications;
        preparedGraphicsCommit =
            pipelineFactory.PrepareCandidateCommit(
                graphicsCandidateState);

        ResourceRetireQueue& retireQueue =
            ResourceRetireQueue::GetInstance();
        size_t maximumRetiredPipelineCount = 0;
        for (const PendingMaterialCommit& pending : pendingCommits)
        {
            pending.material->ValidatePipelineReloadCommit(
                pending.commit);
            maximumRetiredPipelineCount +=
                pending.commit.replaceSurface ? 1u : 0u;
            maximumRetiredPipelineCount +=
                pending.commit.replaceShadow ? 1u : 0u;
        }
        maximumRetiredPipelineCount +=
            pendingComputeCommits.size() * 2;
        if (hasUiCommit)
        {
            maximumRetiredPipelineCount += 1;
        }
        const uint64_t lastUsedEpoch =
            retireQueue.GetLastSubmittedEpoch();
        std::vector<RetiredResource> retirementResources;
        retirementResources.reserve(
            maximumRetiredPipelineCount);
        std::unordered_set<const PipelineBase*>
            preparedRetiredPipelines;
        preparedRetiredPipelines.reserve(
            maximumRetiredPipelineCount);
        for (const PendingMaterialCommit& pending : pendingCommits)
        {
            if (pending.commit.replaceSurface)
            {
                const std::shared_ptr<PipelineBase>& oldPipeline =
                    pending.material->GetRenderPipeline();
                if (oldPipeline &&
                    preparedRetiredPipelines.insert(
                        oldPipeline.get()).second)
                {
                    retirementResources.push_back({
                        "ShaderReload:Surface:" +
                            pending.material->GetMaterialKey(),
                        currentWorldGeneration,
                        lastUsedEpoch,
                        std::static_pointer_cast<void>(
                            oldPipeline)});
                }
            }
            if (pending.commit.replaceShadow)
            {
                const std::shared_ptr<PipelineBase>& oldPipeline =
                    pending.material->GetShadowPipeline();
                if (oldPipeline &&
                    preparedRetiredPipelines.insert(
                        oldPipeline.get()).second)
                {
                    retirementResources.push_back({
                        "ShaderReload:Shadow:" +
                            pending.material->GetMaterialKey(),
                        currentWorldGeneration,
                        lastUsedEpoch,
                        std::static_pointer_cast<void>(
                            oldPipeline)});
                }
            }
        }
        for (PendingComputeCommit& pending :
             pendingComputeCommits)
        {
            const uint64_t descriptorRetirementEpoch = std::max(
                lastUsedEpoch,
                pending.descriptors.minimumRetirementEpoch);
            const std::shared_ptr<ComputePipeline> oldPipeline =
                pending.candidate->plan.participant
                    ->GetActivePipeline();
            if (oldPipeline)
            {
                retirementResources.push_back({
                    "ShaderReload:Compute:" +
                        pending.candidate->plan.shaderName,
                    currentWorldGeneration,
                    lastUsedEpoch,
                    std::static_pointer_cast<void>(
                        oldPipeline)});
            }
            if (pending.descriptors.retirement)
            {
                retirementResources.push_back({
                    "ShaderReload:ComputeDescriptors:" +
                        pending.candidate->plan.shaderName,
                    currentWorldGeneration,
                    descriptorRetirementEpoch,
                    pending.descriptors.retirement.TakeResource()});
            }
        }
        if (pendingUiReplacement.has_value() &&
            pendingUiReplacement->retirement)
        {
            retirementResources.push_back({
                "ShaderReload:UiOverlay",
                currentWorldGeneration,
                lastUsedEpoch,
                pendingUiReplacement->retirement.TakeResource()});
        }
        preparedRetirementCount =
            retirementResources.size();
        preparedRetirements =
            retireQueue.PrepareRetirements(
                std::move(retirementResources));

        // Formal disk publication is the final fallible operation covered by
        // candidate rollback. Once it returns, every live-owner update below
        // is a type-level no-throw ownership swap.
        shaderCompiler.CommitArtifacts(
            artifactsToCommit);
    }
    catch (...)
    {
        for (PendingComputeCommit& pending :
             pendingComputeCommits)
        {
            if (pending.descriptors.release)
            {
                pending.descriptors.release();
            }
        }
        if (pendingUiReplacement.has_value() &&
            pendingUiReplacement->release)
        {
            pendingUiReplacement->release();
        }
        throw;
    }

    pipelineFactory.CommitPreparedCandidate(
        std::move(*preparedGraphicsCommit));
    for (PendingMaterialCommit& pending :
         pendingCommits)
    {
        (void)pending.material->CommitPipelineReload(
            std::move(pending.commit));
    }
    for (PendingComputeCommit& pending :
         pendingComputeCommits)
    {
        pending.descriptors.retirement.Activate();
        pending.candidate->plan.participant
            ->CommitReplacement(
                std::move(pending.committedArtifact),
                std::move(pending.replacementPipeline),
                std::move(pending.descriptors));
    }
    if (batch.uiBuild &&
        batch.uiBuild->sourceGenerationChanged)
    {
        UiReloadCandidate& candidate = *batch.uiBuild;
        pendingUiReplacement->retirement.Activate();
        candidate.plan.participant->CommitReplacement(
            std::move(*uiCommittedArtifact),
            std::move(*pendingUiReplacement));
    }

    statistics.pipelinesRetired =
        preparedRetirementCount;
    ResourceRetireQueue::GetInstance()
        .CommitPreparedRetirements(
            std::move(preparedRetirements));
    statistics.committed = true;
    return statistics;
}

} // namespace VL
