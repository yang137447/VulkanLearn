#include "world/loading/worldGraphTransactionCoordinator.h"

#include <exception>
#include <filesystem>
#include <memory>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include "controller.h"
#include "engine/diagnosticsSubsystem.h"
#include "engine/runtimeConfig.h"
#include "material/generator/materialDefinitionReloadBatch.h"
#include "pipeline/pipelineFactory.h"
#include "render/backend/rendererBackendVulkan.h"
#include "render/renderThread.h"
#include "render/rendergraph/preparedRenderGraphState.h"
#include "render/resource/rendererResourceCache.h"
#include "render/resource/resourceRetireQueue.h"
#include "renderGraph.h"
#include "renderSystem.h"
#include "shader/build/contentHash.h"
#include "shaderCompiler.h"
#include "world/loading/worldTransitionCoordinator.h"
#include "world/worldManager.h"

namespace VL
{

namespace
{

std::vector<AtomicFileWrite>
BuildGeneratedIncludeWrites(
    const MaterialDefinitionReloadBatch* batch)
{
    std::vector<AtomicFileWrite> writes;
    if (batch == nullptr)
    {
        return writes;
    }
    for (const auto& candidate :
         batch->generatedIncludes)
    {
        const std::vector<uint8_t> bytes(
            candidate.generatedBytes.begin(),
            candidate.generatedBytes.end());
        if (std::filesystem::is_regular_file(
                candidate.outputPath) &&
            ReadBinaryFile(candidate.outputPath) ==
                bytes)
        {
            continue;
        }
        writes.push_back({
            candidate.outputPath,
            bytes});
    }
    return writes;
}

void AppendPendingGeneratedResourceWrites(
    const std::vector<PendingGeneratedResourceFile>& pendingFiles,
    std::vector<AtomicFileWrite>& writes)
{
    for (const PendingGeneratedResourceFile& pendingFile : pendingFiles)
    {
        if (std::filesystem::is_regular_file(pendingFile.path) &&
            ReadBinaryFile(pendingFile.path) == pendingFile.bytes)
        {
            continue;
        }
        writes.push_back({
            pendingFile.path,
            pendingFile.bytes});
    }
}

void ValidateMaterialDefinitionSourcesStillCurrent(
    const MaterialDefinitionReloadBatch* batch,
    const std::filesystem::path& shaderRoot)
{
    if (batch == nullptr)
    {
        return;
    }
    const std::filesystem::path glslRoot =
        shaderRoot / "glsl";
    for (const auto& [identity, capturedDigest] :
         batch->sourceDigests)
    {
        const std::filesystem::path sourcePath =
            glslRoot / identity;
        if (!std::filesystem::is_regular_file(
                sourcePath))
        {
            throw std::runtime_error(
                "Material definition source disappeared before commit: " +
                identity);
        }
        const std::string currentDigest =
            ContentHasher::HashFile(
                sourcePath).ToHex();
        if (currentDigest != capturedDigest)
        {
            throw std::runtime_error(
                "Material definition source changed before commit: " +
                identity);
        }
    }
}

RenderGraph::TestFaultInjection BuildRenderGraphTestFaultInjection(
    const WorldGraphTransactionTestFaultInjection& injection)
{
    RenderGraph::TestFaultInjection graphFault;
    graphFault.failResourceCreationAt =
        injection.failGraphResourceCreationAt;
    graphFault.failRenderPassCreationAt =
        injection.failRenderPassCreationAt;
    graphFault.failFramebufferCreationAt =
        injection.failFramebufferCreationAt;
    graphFault.failDescriptorCreationAt =
        injection.failDescriptorCreationAt;
    graphFault.failPassMaterialContract =
        injection.failPassMaterialContract;
    return graphFault;
}

} // namespace

WorldGraphTransactionCoordinator::WorldGraphTransactionCoordinator(
    RendererBackendVulkan& rendererBackend,
    ShaderCompiler& shaderCompiler,
    PipelineFactory& pipelineFactory,
    WorldTransitionCoordinator& worldTransitionCoordinator,
    WorldManager& worldManager,
    Controller& controller,
    const RuntimeConfig& runtimeConfig,
    const DiagnosticsSubsystem& diagnostics,
    RenderThread* renderThread)
    : rendererBackend(rendererBackend)
    , shaderCompiler(shaderCompiler)
    , pipelineFactory(pipelineFactory)
    , worldTransitionCoordinator(worldTransitionCoordinator)
    , worldManager(worldManager)
    , controller(controller)
    , runtimeConfig(runtimeConfig)
    , diagnostics(diagnostics)
    , renderThread(renderThread)
{
}

void WorldGraphTransactionCoordinator::SetFaultInjection(
    WorldGraphTransactionTestFaultInjection injection) noexcept
{
    faultInjection = injection;
}

RuntimeResult<WorldHandle>
WorldGraphTransactionCoordinator::Execute(
    const std::string& scenePath,
    const MaterialDefinitionReloadBatch* materialDefinitionReload)
{
    try
    {
        auto candidateGraph =
            std::make_shared<PreparedRenderGraphState>(
                rendererBackend);
        candidateGraph->GetGraph().SetTestFaultInjection(
            BuildRenderGraphTestFaultInjection(faultInjection));
        candidateGraph->Load(runtimeConfig.GetRenderGraphJson());

        auto preparedWorldResult =
            worldTransitionCoordinator.PrepareWorldLoad(
                scenePath,
                candidateGraph->GetGraph(),
                materialDefinitionReload);
        if (preparedWorldResult.IsFailure())
        {
            return RuntimeResult<WorldHandle>::Failure(
                preparedWorldResult.Error());
        }
        PreparedWorldTransition preparedWorld =
            std::move(preparedWorldResult.Value());
        if (faultInjection.failAfterCandidateWorldBuilt)
        {
            throw std::runtime_error(
                "Injected failure after candidate World build");
        }

        candidateGraph->GetGraph().RestorePassMaterialInstances(
            preparedWorld.passMaterialBindings);
        candidateGraph->GetGraph().SetOwnerGeneration(
            preparedWorld.activation.handle.generation);

        std::shared_ptr<SceneNode> viewTarget =
            preparedWorld.activation.handle.viewTarget.lock();
        if (faultInjection.failViewTargetPrecheck || !viewTarget)
        {
            return RuntimeResult<WorldHandle>::Failure(
                MakeRuntimeError(
                    "WorldGraphTransaction.MissingViewTarget",
                    "Candidate World has no controller view target.",
                    scenePath));
        }

        PreparedRuntimeBinding preparedRuntime =
            RenderSystem::GetInstance().PrepareRuntimeBinding(
                preparedWorld.world,
                *preparedWorld.resourceCache,
                candidateGraph->GetGraph());
        if (faultInjection.failAfterRuntimeBindingPrepared)
        {
            throw std::runtime_error(
                "Injected failure after runtime binding prepare");
        }

        PipelineFactory::PreparedGraphicsCandidateCommit
            preparedPipelineCommit =
                pipelineFactory.PrepareCandidateCommit(
                    preparedWorld.graphicsCandidateState);

        std::vector<ShaderBuildArtifact> artifactsToCommit;
        artifactsToCommit.reserve(
            preparedWorld.graphicsCandidateState.shaderBuildArtifacts.size());
        for (auto& preparedBuild :
             preparedWorld.graphicsCandidateState.shaderBuildArtifacts)
        {
            const ShaderCompiler::CandidateSourceValidationResult
                sourceValidation =
                    shaderCompiler.ValidateCandidateSourcesStillCurrent(
                        preparedBuild.request,
                        preparedBuild.artifact);
            if (!sourceValidation.current)
            {
                throw std::runtime_error(
                    "World transaction shader candidate source validation failed: " +
                    sourceValidation.reason +
                    "; source=" +
                    sourceValidation.sourceIdentity +
                    "; capturedDigest=" +
                    sourceValidation.capturedDigest +
                    "; currentDigest=" +
                    sourceValidation.currentDigest);
            }
            artifactsToCommit.push_back(
                std::move(preparedBuild.artifact));
        }
        ValidateMaterialDefinitionSourcesStillCurrent(
            materialDefinitionReload,
            shaderCompiler.GetShaderRoot());
        std::vector<AtomicFileWrite> additionalPublicationWrites =
            BuildGeneratedIncludeWrites(materialDefinitionReload);
        AppendPendingGeneratedResourceWrites(
            preparedWorld.pendingGeneratedFiles,
            additionalPublicationWrites);

        if (renderThread && renderThread->IsRunning())
        {
            renderThread->WaitUntilIdle();
            if (renderThread->HasFatalError())
            {
                return RuntimeResult<WorldHandle>::Failure(
                    MakeRuntimeError(
                        "WorldGraphTransaction.RenderThreadFailed",
                        renderThread->ConsumeFatalError(),
                        scenePath));
            }
        }

        for (size_t buildIndex = 0;
             buildIndex < artifactsToCommit.size();
             ++buildIndex)
        {
            const auto& preparedBuild =
                preparedWorld.graphicsCandidateState.shaderBuildArtifacts[
                    buildIndex];
            const ShaderCompiler::CandidateSourceValidationResult
                sourceValidation =
                    shaderCompiler.ValidateCandidateSourcesStillCurrent(
                        preparedBuild.request,
                        artifactsToCommit[buildIndex]);
            if (!sourceValidation.current)
            {
                throw std::runtime_error(
                    "World transaction shader candidate became stale before commit: " +
                    sourceValidation.reason +
                    "; source=" +
                    sourceValidation.sourceIdentity);
            }
        }
        ValidateMaterialDefinitionSourcesStillCurrent(
            materialDefinitionReload,
            shaderCompiler.GetShaderRoot());
        if (faultInjection.failBeforeCommit)
        {
            throw std::runtime_error(
                "Injected failure after all World/graph/runtime prepare steps");
        }

        ResourceRetireQueue& retireQueue =
            ResourceRetireQueue::GetInstance();
        constexpr size_t MaximumRetirementCount = 4;
        std::vector<RetiredResource> retirementResources;
        retirementResources.reserve(MaximumRetirementCount);
        const uint64_t oldWorldGeneration =
            worldManager.GetActiveWorldHandle().generation;
        const uint64_t lastUsedEpoch =
            retireQueue.GetLastSubmittedEpoch();
        retirementResources.push_back({
            "WorldTransaction:World",
            oldWorldGeneration,
            lastUsedEpoch,
            {}});
        retirementResources.push_back({
            "WorldTransaction:WorldLocalResources",
            oldWorldGeneration,
            lastUsedEpoch,
            {}});
        retirementResources.push_back({
            "WorldTransaction:RenderGraph",
            oldWorldGeneration,
            lastUsedEpoch,
            {}});
        retirementResources.push_back({
            "WorldTransaction:FrameLightBuffer",
            oldWorldGeneration,
            lastUsedEpoch,
            {}});
        PreparedResourceRetirements preparedRetirements =
            retireQueue.PrepareRetirements(
                std::move(retirementResources));

        const WorldHandle committedHandle =
            preparedWorld.activation.handle;
        const std::string commitDiagnostic =
            "World/graph transaction committed: generation=" +
            std::to_string(committedHandle.generation) +
            ", scene=" +
            committedHandle.scenePath +
            ", graphGeneration=" +
            std::to_string(committedHandle.generation) +
            ", renderSystemGeneration=" +
            std::to_string(committedHandle.generation) +
            ", controllerGeneration=" +
            std::to_string(committedHandle.generation);

        shaderCompiler.CommitArtifactsWithAdditionalFiles(
            artifactsToCommit,
            additionalPublicationWrites);

        // Formal publication above is the final fallible step. Every update
        // below is a prevalidated ownership move or swap.
        RenderSystem::GetInstance().ClearPendingWorldSnapshots();
        pipelineFactory.CommitPreparedCandidate(
            std::move(preparedPipelineCommit));

        RendererResourceCache::WorldLocalResourcePackageHandle
            retiredWorldResources =
                RendererResourceCache::GetInstance().CommitCandidate(
                    std::move(*preparedWorld.resourceCache));
        RenderGraph::GetInstance().SwapState(
            candidateGraph->GetGraph());
        std::shared_ptr<World> retiredWorld =
            worldManager.CommitPreparedActivation(
                std::move(preparedWorld.activation));
        std::shared_ptr<void> retiredLightBuffer =
            RenderSystem::GetInstance().CommitPreparedRuntimeBinding(
                std::move(preparedRuntime));
        controller.SetViewTarget(
            std::move(viewTarget),
            worldManager.GetActiveWorldHandle().generation);

        preparedRetirements.GetAdditionalResource(0).resource =
            std::static_pointer_cast<void>(std::move(retiredWorld));
        preparedRetirements.GetAdditionalResource(1).resource =
            std::static_pointer_cast<void>(
                std::move(retiredWorldResources));
        preparedRetirements.GetAdditionalResource(2).resource =
            std::static_pointer_cast<void>(std::move(candidateGraph));
        preparedRetirements.GetAdditionalResource(3).resource =
            std::move(retiredLightBuffer);
        retireQueue.CommitPreparedRetirements(
            std::move(preparedRetirements));

        try
        {
            diagnostics.ReportInfo(commitDiagnostic);
        }
        catch (...)
        {
            // Diagnostics publication is best effort after ownership swap.
        }
        return RuntimeResult<WorldHandle>::Success(committedHandle);
    }
    catch (const std::exception& exception)
    {
        return RuntimeResult<WorldHandle>::Failure(
            MakeRuntimeError(
                "WorldGraphTransaction.Failed",
                exception.what(),
                scenePath));
    }
}

} // namespace VL
