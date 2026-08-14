#include "shader/reload/shaderReloadRuntime.h"

#include <algorithm>
#include <chrono>
#include <exception>
#include <filesystem>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include "engine/diagnosticsSubsystem.h"
#include "engine/runtimeConfig.h"
#include "engine/runtimeCommandExecutor.h"
#include "material/generator/materialDefinitionReloadBatch.h"
#include "material/generator/materialParameterIncludeGenerator.h"
#include "render/resource/resourceRetireQueue.h"
#include "shader/build/contentHash.h"
#include "shader/reload/shaderFileMonitor.h"
#include "shaderCompiler.h"

namespace VL
{

ShaderReloadRuntime::~ShaderReloadRuntime()
{
    Shutdown();
}

void ShaderReloadRuntime::Initialize(
    ShaderCompiler& shaderCompiler,
    ShaderReloadCoordinator& shaderReloadCoordinator,
    const std::filesystem::path& shaderRoot)
{
    Shutdown();
    this->shaderCompiler = &shaderCompiler;
    this->shaderReloadCoordinator = &shaderReloadCoordinator;
    this->shaderRoot = shaderRoot;

    shaderFileMonitor = std::make_unique<ShaderFileMonitor>();
    shaderFileMonitor->Initialize(shaderRoot);
    shaderCompileWorker = std::make_unique<ShaderCompileWorker>();
    shaderCompileWorker->Start(shaderCompiler);
}

ShaderCompileWorkerShutdownDiagnostics
ShaderReloadRuntime::Shutdown()
{
    ShaderCompileWorkerShutdownDiagnostics diagnostics;
    if (shaderCompileWorker)
    {
        shaderCompileWorker->Shutdown();
        diagnostics =
            shaderCompileWorker->GetShutdownDiagnostics();
        shaderCompileWorker.reset();
    }
    shaderFileMonitor.reset();
    shaderReloadCoordinator = nullptr;
    shaderCompiler = nullptr;
    shaderRoot.clear();
    pendingAutoReloadSources.clear();
    pendingMaterialDefinitionSources.clear();
    inFlightAutoReloadGeneration = 0;
    inFlightAutoReloadSourceEpoch = 0;
    return diagnostics;
}

void ShaderReloadRuntime::EnableTestCompileGate()
{
    shaderCompileWorker->EnableTestCompileGate();
}

void ShaderReloadRuntime::ArmTestCompileGateForNextSubmit()
{
    shaderCompileWorker->ArmTestCompileGateForNextSubmit();
}

bool ShaderReloadRuntime::IsWaitingAtTestCompileGate() const
{
    return shaderCompileWorker != nullptr &&
        shaderCompileWorker->IsWaitingAtTestCompileGate();
}

void ShaderReloadRuntime::ReleaseTestCompileGate()
{
    shaderCompileWorker->ReleaseTestCompileGate();
}

void ShaderReloadRuntime::DisableTestCompileGate()
{
    shaderCompileWorker->DisableTestCompileGate();
}

void ShaderReloadRuntime::ArmTestPostCompileGateForNextSubmit()
{
    shaderCompileWorker->ArmTestPostCompileGateForNextSubmit();
}

bool ShaderReloadRuntime::IsWaitingAtTestPostCompileGate() const
{
    return shaderCompileWorker != nullptr &&
        shaderCompileWorker->IsWaitingAtTestPostCompileGate();
}

void ShaderReloadRuntime::SetTestPollInterval(
    std::chrono::milliseconds interval)
{
    shaderFileMonitor->SetTestPollInterval(interval);
}

void ShaderReloadRuntime::SetTestScanSuspended(bool suspended)
{
    shaderFileMonitor->SetTestScanSuspended(suspended);
}

void ShaderReloadRuntime::RefreshBaselineForSources(
    const std::vector<std::string>& sourceIdentities)
{
    shaderFileMonitor->RefreshBaselineForSources(
        sourceIdentities);
}

ShaderReloadRuntimeStateSnapshot
ShaderReloadRuntime::CaptureSnapshot() const
{
    ShaderReloadRuntimeStateSnapshot snapshot;
    snapshot.nextGeneration = nextShaderReloadGeneration;
    snapshot.latestObservedSourceEpoch = latestObservedSourceEpoch;
    snapshot.latestSubmittedAutoReloadGeneration =
        latestSubmittedAutoReloadGeneration;
    snapshot.inFlightAutoReloadGeneration =
        inFlightAutoReloadGeneration;
    snapshot.inFlightAutoReloadSourceEpoch =
        inFlightAutoReloadSourceEpoch;
    snapshot.latestManualShaderReloadGeneration =
        latestManualShaderReloadGeneration;
    snapshot.latestManualShaderReloadCommittedGeneration =
        latestManualShaderReloadCommittedGeneration;
    snapshot.latestManualShaderReloadFailedGeneration =
        latestManualShaderReloadFailedGeneration;
    snapshot.latestAutoReloadStaleDiscardGeneration =
        latestAutoReloadStaleDiscardGeneration;
    snapshot.latestAutoReloadFailedGeneration =
        latestAutoReloadFailedGeneration;
    snapshot.latestAutoReloadCommittedGeneration =
        latestAutoReloadCommittedGeneration;
    snapshot.latestAutoReloadShadercInvocations =
        latestAutoReloadShadercInvocations;
    snapshot.totalAutoReloadShadercInvocations =
        totalAutoReloadShadercInvocations;
    snapshot.pendingAutoReloadSourceEpoch =
        pendingAutoReloadSourceEpoch;
    snapshot.failedPendingAutoReloadSourceEpoch =
        failedPendingAutoReloadSourceEpoch;
    snapshot.pendingMaterialDefinitionSourceEpoch =
        pendingMaterialDefinitionSourceEpoch;
    snapshot.failedPendingMaterialDefinitionSourceEpoch =
        failedPendingMaterialDefinitionSourceEpoch;
    snapshot.latestMaterialDefinitionReloadCommittedGeneration =
        latestMaterialDefinitionReloadCommittedGeneration;
    snapshot.latestMaterialDefinitionReloadFailedGeneration =
        latestMaterialDefinitionReloadFailedGeneration;
    snapshot.pendingAutoReloadSources = pendingAutoReloadSources;
    snapshot.pendingMaterialDefinitionSources =
        pendingMaterialDefinitionSources;
    snapshot.lastSubmittedAutoReloadSources =
        lastSubmittedAutoReloadSources;
    snapshot.lastStaleAutoReloadSources =
        lastStaleAutoReloadSources;
    snapshot.lastCommittedAutoReloadSources =
        lastCommittedAutoReloadSources;
    snapshot.workerRunning =
        shaderCompileWorker != nullptr &&
        shaderCompileWorker->IsRunning();
    snapshot.workerIdle =
        shaderCompileWorker == nullptr ||
        shaderCompileWorker->IsIdle();
    snapshot.workerHasCompletedResult =
        shaderCompileWorker != nullptr &&
        shaderCompileWorker->HasCompletedResult();
    snapshot.workerInFlightGeneration =
        shaderCompileWorker != nullptr
            ? shaderCompileWorker->GetInFlightGeneration()
            : 0;
    snapshot.monitorHasUnstableSourceChanges =
        shaderFileMonitor != nullptr &&
        shaderFileMonitor->HasUnstableSourceChanges();
    snapshot.monitorScanCount =
        shaderFileMonitor != nullptr
            ? shaderFileMonitor->GetScanCount()
            : 0;
    return snapshot;
}

MaterialDefinitionReloadBatch
ShaderReloadRuntime::
    BuildMaterialDefinitionReloadBatchForValidation(
        uint64_t batchId,
        const std::set<std::string>& sourceIdentities) const
{
    return BuildMaterialDefinitionReloadBatch(
        batchId,
        sourceIdentities);
}

void ShaderReloadRuntime::ProcessManualReload(
    const RuntimeCommandExecutionResult& commandResult,
    ShaderReloadRuntimeHost& host,
    const DiagnosticsSubsystem& diagnostics)
{
    if (commandResult.shaderCacheStatisticsRequested)
    {
        const ShaderBuildManifestSnapshot manifest =
            shaderCompiler->CaptureManifestSnapshot();
        diagnostics.ReportInfo(
            ShaderCompiler::FormatStatistics(
                shaderCompiler->GetLastStatistics()) +
            ", manifestArtifacts=" +
            std::to_string(manifest.artifacts.size()));
    }

    if (!commandResult.shaderReloadRequested)
    {
        return;
    }

    const ShaderReloadScope scope =
        commandResult.shaderReloadScope ==
                RuntimeShaderReloadScope::All
            ? ShaderReloadScope::All
            : ShaderReloadScope::Changed;
    const uint64_t generation =
        nextShaderReloadGeneration++;
    latestManualShaderReloadGeneration = generation;

    try
    {
        ShaderReloadPlan plan =
            shaderReloadCoordinator->CaptureGraphicsPlan(
                scope,
                generation,
                host.GetShaderReloadWorldGeneration());
        diagnostics.ReportInfo(
            "Shader reload batch " +
            std::to_string(generation) +
            " prepared: changedSources=" +
            std::to_string(plan.changedSources.size()) +
            ", affectedBuilds=" +
            std::to_string(
                plan.builds.size() +
                plan.computeBuilds.size() +
                (plan.uiBuild.has_value() ? 1u : 0u)) +
            ", liveMaterials=" +
            std::to_string(plan.materials.size()));

        ShaderReloadCandidateBatch batch =
            shaderReloadCoordinator->CompileGraphicsCandidates(
                std::move(plan));

        host.WaitForShaderReloadSafePoint();
        if (host.IsShaderReloadClosing())
        {
            return;
        }

        const ShaderReloadCommitStatistics statistics =
            shaderReloadCoordinator->CommitGraphicsCandidates(
                batch,
                host.GetShaderReloadWorldGeneration());
        host.RefreshSceneAfterShaderReload();

        diagnostics.ReportInfo(
            "Shader reload batch " +
            std::to_string(statistics.generation) +
            ": changedSources=" +
            std::to_string(statistics.changedSourceCount) +
            ", affectedBuilds=" +
            std::to_string(statistics.affectedBuildCount) +
            ", liveMaterials=" +
            std::to_string(statistics.liveMaterialCount) +
            ", compiled=" +
            std::to_string(statistics.compiledBuildCount) +
            ", shaderc=" +
            std::to_string(statistics.shadercInvocations) +
            ", pipelinesCreated=" +
            std::to_string(statistics.pipelinesCreated) +
            ", committed=" +
            std::string(statistics.committed ? "true" : "false") +
            ", retiredPipelines=" +
            std::to_string(statistics.pipelinesRetired) +
            ", retirePending=" +
            std::to_string(
                host.GetShaderReloadRetirePendingCount()));
        latestManualShaderReloadCommittedGeneration = generation;
    }
    catch (const std::exception& exception)
    {
        latestManualShaderReloadFailedGeneration = generation;
        diagnostics.ReportError(
            "Shader reload batch " +
            std::to_string(generation) +
            " rejected; current pipelines and formal artifacts remain active: " +
            exception.what());
    }
}

void ShaderReloadRuntime::ProcessAutomaticReloads(
    ShaderReloadRuntimeHost& host,
    const DiagnosticsSubsystem& diagnostics)
{
    if (!shaderFileMonitor || !shaderCompileWorker ||
        !shaderCompileWorker->IsRunning())
    {
        return;
    }

    const std::optional<ShaderFileMonitor::ChangeBatch> changeBatch =
        shaderFileMonitor->Poll();
    if (changeBatch)
    {
        if (!changeBatch->observedSources.empty())
        {
            ++latestObservedSourceEpoch;
            if (!pendingAutoReloadSources.empty())
            {
                pendingAutoReloadSourceEpoch =
                    latestObservedSourceEpoch;
            }
            diagnostics.ReportInfo(
                "Shader source epoch " +
                std::to_string(latestObservedSourceEpoch) +
                " observed content transitions=" +
                std::to_string(
                    changeBatch->observedSources.size()));
        }

        std::vector<std::string> shaderSources;
        std::vector<std::string> materialDefinitionSources;
        for (const std::string& source :
             changeBatch->changedSources)
        {
            const std::filesystem::path sourcePath(source);
            const bool isMaterialDefinition =
                sourcePath.extension() == ".json" &&
                sourcePath.filename().string().rfind("M_", 0) == 0;
            if (isMaterialDefinition)
            {
                materialDefinitionSources.push_back(source);
            }
            else
            {
                shaderSources.push_back(source);
            }
        }

        if (!shaderSources.empty())
        {
            MergePendingAutomaticShaderSources(
                shaderSources,
                latestObservedSourceEpoch);
            diagnostics.ReportInfo(
                "Shader source epoch " +
                std::to_string(latestObservedSourceEpoch) +
                " accepted stable shader sources=" +
                std::to_string(shaderSources.size()) +
                ", pendingUnion=" +
                std::to_string(
                    pendingAutoReloadSources.size()));
        }

        if (!materialDefinitionSources.empty())
        {
            pendingMaterialDefinitionSources.insert(
                materialDefinitionSources.begin(),
                materialDefinitionSources.end());
            pendingMaterialDefinitionSourceEpoch =
                std::max(
                    pendingMaterialDefinitionSourceEpoch,
                    latestObservedSourceEpoch);
            if (failedPendingMaterialDefinitionSourceEpoch != 0 &&
                pendingMaterialDefinitionSourceEpoch >
                    failedPendingMaterialDefinitionSourceEpoch)
            {
                failedPendingMaterialDefinitionSourceEpoch = 0;
            }
            diagnostics.ReportInfo(
                "Shader source epoch " +
                std::to_string(
                    latestObservedSourceEpoch) +
                " accepted stable material definitions=" +
                std::to_string(
                    materialDefinitionSources.size()) +
                ", pendingM_Union=" +
                std::to_string(
                    pendingMaterialDefinitionSources.size()));
        }
    }

    if (shaderCompileWorker->HasCompletedResult())
    {
        ShaderCompileWorkerResult result =
            shaderCompileWorker->TakeCompletedResult();
        totalAutoReloadShadercInvocations +=
            result.shadercInvocations;
        const uint64_t resultSourceEpoch =
            result.sourceEpoch;
        const bool supersededByObservedSource =
            resultSourceEpoch < latestObservedSourceEpoch;
        const bool supersededByManualReload =
            result.generation <
                latestManualShaderReloadGeneration;
        if (result.generation !=
                inFlightAutoReloadGeneration ||
            supersededByObservedSource ||
            supersededByManualReload)
        {
            latestAutoReloadStaleDiscardGeneration =
                result.generation;
            lastStaleAutoReloadSources =
                result.changedSources;
            MergePendingAutomaticShaderSources(
                result.changedSources,
                latestObservedSourceEpoch);
            diagnostics.ReportInfo(
                "Shader auto reload batch " +
                std::to_string(result.generation) +
                " discarded as stale: capturedSourceEpoch=" +
                std::to_string(resultSourceEpoch) +
                ", latestObservedSourceEpoch=" +
                std::to_string(latestObservedSourceEpoch) +
                ", latestManualGeneration=" +
                    std::to_string(
                    latestManualShaderReloadGeneration) +
                ", discardedSources=" +
                std::to_string(result.changedSources.size()) +
                ", pendingUnion=" +
                std::to_string(pendingAutoReloadSources.size()));
        }
        else if (!result.succeeded)
        {
            latestAutoReloadFailedGeneration =
                result.generation;
            MergePendingAutomaticShaderSources(
                result.changedSources,
                resultSourceEpoch);
            failedPendingAutoReloadSourceEpoch =
                pendingAutoReloadSourceEpoch;
            diagnostics.ReportError(
                "Shader auto reload batch " +
                std::to_string(result.generation) +
                " compile failed; current pipelines remain active: " +
                result.errorMessage);
        }
        else
        {
            try
            {
                host.WaitForShaderReloadSafePoint();
                if (host.IsShaderReloadClosing())
                {
                    return;
                }
                const ShaderReloadCommitStatistics statistics =
                    shaderReloadCoordinator->CommitGraphicsCandidates(
                        result.batch,
                        host.GetShaderReloadWorldGeneration());
                latestAutoReloadCommittedGeneration =
                    result.generation;
                latestAutoReloadShadercInvocations =
                    statistics.shadercInvocations;
                lastCommittedAutoReloadSources =
                    result.changedSources;
                host.RefreshSceneAfterShaderReload();
                diagnostics.ReportInfo(
                    "Shader auto reload batch " +
                    std::to_string(statistics.generation) +
                    ": changedSources=" +
                    std::to_string(statistics.changedSourceCount) +
                    ", affectedBuilds=" +
                    std::to_string(statistics.affectedBuildCount) +
                    ", liveMaterials=" +
                    std::to_string(statistics.liveMaterialCount) +
                    ", compiled=" +
                    std::to_string(statistics.compiledBuildCount) +
                    ", shaderc=" +
                    std::to_string(statistics.shadercInvocations) +
                    ", pipelinesCreated=" +
                    std::to_string(statistics.pipelinesCreated) +
                    ", committed=" +
                    std::string(statistics.committed ? "true" : "false") +
                    ", retiredPipelines=" +
                    std::to_string(statistics.pipelinesRetired) +
                    ", compileMs=" +
                    std::to_string(result.elapsedMilliseconds) +
                    ", retirePending=" +
                    std::to_string(
                        host.GetShaderReloadRetirePendingCount()) +
                    ", sourceEpoch=" +
                    std::to_string(resultSourceEpoch) +
                    ", pendingUnion=" +
                    std::to_string(pendingAutoReloadSources.size()));
            }
            catch (const std::exception& exception)
            {
                latestAutoReloadFailedGeneration =
                    result.generation;
                MergePendingAutomaticShaderSources(
                    result.changedSources,
                    std::max(
                        resultSourceEpoch,
                        latestObservedSourceEpoch));
                failedPendingAutoReloadSourceEpoch =
                    pendingAutoReloadSourceEpoch;
                diagnostics.ReportError(
                    "Shader auto reload batch " +
                    std::to_string(result.generation) +
                    " rejected; current pipelines and formal artifacts remain active: " +
                    exception.what());
            }
        }

        inFlightAutoReloadGeneration = 0;
        inFlightAutoReloadSourceEpoch = 0;
    }

    ProcessPendingMaterialDefinitionReload(host, diagnostics);
    SubmitPendingAutomaticShaderReload(host, diagnostics);
}

void ShaderReloadRuntime::MergePendingAutomaticShaderSources(
    const std::vector<std::string>& sourceIdentities,
    uint64_t sourceEpoch)
{
    pendingAutoReloadSources.insert(
        sourceIdentities.begin(),
        sourceIdentities.end());
    pendingAutoReloadSourceEpoch =
        std::max(
            pendingAutoReloadSourceEpoch,
            sourceEpoch);
    if (failedPendingAutoReloadSourceEpoch != 0 &&
        pendingAutoReloadSourceEpoch >
            failedPendingAutoReloadSourceEpoch)
    {
        failedPendingAutoReloadSourceEpoch = 0;
    }
}

void ShaderReloadRuntime::SubmitPendingAutomaticShaderReload(
    ShaderReloadRuntimeHost& host,
    const DiagnosticsSubsystem& diagnostics)
{
    if (pendingAutoReloadSources.empty() ||
        !shaderCompileWorker->IsIdle() ||
        shaderFileMonitor->HasUnstableSourceChanges() ||
        failedPendingAutoReloadSourceEpoch ==
            pendingAutoReloadSourceEpoch)
    {
        return;
    }

    const std::vector<std::string> shaderSources(
        pendingAutoReloadSources.begin(),
        pendingAutoReloadSources.end());
    const uint64_t generation =
        nextShaderReloadGeneration++;
    try
    {
        ShaderReloadPlan plan =
            shaderReloadCoordinator->CaptureGraphicsPlanForSources(
                shaderSources,
                generation,
                host.GetShaderReloadWorldGeneration());
        plan.sourceEpoch =
            pendingAutoReloadSourceEpoch;
        diagnostics.ReportInfo(
            "Shader auto reload batch " +
            std::to_string(generation) +
            " prepared: sourceEpoch=" +
            std::to_string(plan.sourceEpoch) +
            ", changedSources=" +
            std::to_string(plan.changedSources.size()) +
            ", affectedBuilds=" +
            std::to_string(
                plan.builds.size() +
                plan.computeBuilds.size() +
                (plan.uiBuild.has_value() ? 1u : 0u)) +
            ", liveMaterials=" +
            std::to_string(plan.materials.size()));

        if (!shaderCompileWorker->Submit(std::move(plan)))
        {
            return;
        }

        latestSubmittedAutoReloadGeneration = generation;
        inFlightAutoReloadGeneration = generation;
        inFlightAutoReloadSourceEpoch =
            pendingAutoReloadSourceEpoch;
        lastSubmittedAutoReloadSources = shaderSources;
        for (const std::string& source : shaderSources)
        {
            pendingAutoReloadSources.erase(source);
        }
        if (pendingAutoReloadSources.empty())
        {
            pendingAutoReloadSourceEpoch = 0;
        }
    }
    catch (const std::exception& exception)
    {
        failedPendingAutoReloadSourceEpoch =
            pendingAutoReloadSourceEpoch;
        diagnostics.ReportError(
            "Shader auto reload batch " +
            std::to_string(generation) +
            " plan capture failed; current pipelines remain active: " +
            exception.what());
    }
}

MaterialDefinitionReloadBatch
ShaderReloadRuntime::BuildMaterialDefinitionReloadBatch(
    uint64_t batchId,
    const std::set<std::string>& sourceIdentities) const
{
    MaterialDefinitionReloadBatch batch;
    batch.batchId = batchId;
    const std::filesystem::path glslRoot =
        shaderRoot / "glsl";
    for (const std::string& identity :
         sourceIdentities)
    {
        const std::filesystem::path materialPath =
            glslRoot / identity;
        auto candidate =
            MaterialParameterIncludeGenerator::
                BuildGeneratedIncludeContent(
                    materialPath);
        const std::string includeIdentity =
            std::filesystem::relative(
                candidate.outputPath,
                glslRoot)
                .lexically_normal()
                .generic_string();
        if (!batch.includeOverlays.emplace(
                includeIdentity,
                candidate.generatedBytes)
                 .second)
        {
            throw std::runtime_error(
                "Material definition reload batch contains duplicate generated include identity: " +
                includeIdentity);
        }
        batch.changedSources.push_back(identity);
        batch.sourceDigests.emplace(
            identity,
            ContentHasher::HashFile(
                materialPath).ToHex());
        batch.generatedIncludes.push_back(
            std::move(candidate));
    }
    return batch;
}

void ShaderReloadRuntime::ProcessPendingMaterialDefinitionReload(
    ShaderReloadRuntimeHost& host,
    const DiagnosticsSubsystem& diagnostics)
{
    if (pendingMaterialDefinitionSources.empty() ||
        !shaderCompileWorker->IsIdle() ||
        shaderFileMonitor->HasUnstableSourceChanges() ||
        failedPendingMaterialDefinitionSourceEpoch ==
            pendingMaterialDefinitionSourceEpoch)
    {
        return;
    }

    const uint64_t batchId =
        nextShaderReloadGeneration++;
    try
    {
        MaterialDefinitionReloadBatch batch =
            BuildMaterialDefinitionReloadBatch(
                batchId,
                pendingMaterialDefinitionSources);
        auto transactionResult =
            host.CommitMaterialDefinitionReload(batch);
        if (transactionResult.IsFailure())
        {
            throw std::runtime_error(
                FormatRuntimeError(
                    transactionResult.Error()));
        }

        latestMaterialDefinitionReloadCommittedGeneration =
            batchId;
        pendingMaterialDefinitionSources.clear();
        pendingMaterialDefinitionSourceEpoch = 0;
        failedPendingMaterialDefinitionSourceEpoch = 0;
        diagnostics.ReportInfo(
            "Material definition reload batch " +
            std::to_string(batchId) +
            " committed all-or-nothing: changedM_=" +
            std::to_string(
                batch.changedSources.size()) +
            ", worldGeneration=" +
            std::to_string(
                transactionResult.Value()
                    .generation));
    }
    catch (const std::exception& exception)
    {
        latestMaterialDefinitionReloadFailedGeneration =
            batchId;
        failedPendingMaterialDefinitionSourceEpoch =
            pendingMaterialDefinitionSourceEpoch;
        diagnostics.ReportError(
            "Material definition reload batch " +
            std::to_string(batchId) +
            " rejected; active World/graph/material resources and formal artifacts remain unchanged: " +
            exception.what());
    }
}

} // namespace VL
