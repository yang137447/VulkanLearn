#pragma once

// File responsibility: Owns game-thread Shader monitor/worker scheduling
// state. It prepares CPU candidates and reports commit requests to its host;
// it does not own World, RenderGraph, Controller, or Vulkan resources.

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <set>
#include <string>
#include <vector>

#include "core/runtimeResult.h"
#include "engine/runtimeCommand.h"
#include "shader/reload/shaderCompileWorker.h"
#include "shader/reload/shaderFileMonitor.h"
#include "shader/reload/shaderReloadCoordinator.h"

class ShaderCompiler;

namespace VL
{

class DiagnosticsSubsystem;
struct MaterialDefinitionReloadBatch;
struct RuntimeCommandExecutionResult;
struct WorldHandle;

class ShaderReloadRuntimeHost
{
public:
    virtual ~ShaderReloadRuntimeHost() = default;

    virtual uint64_t GetShaderReloadWorldGeneration() const noexcept = 0;
    virtual void WaitForShaderReloadSafePoint() = 0;
    virtual bool IsShaderReloadClosing() const noexcept = 0;
    virtual void RefreshSceneAfterShaderReload() = 0;
    virtual size_t GetShaderReloadRetirePendingCount() const noexcept = 0;
    virtual RuntimeResult<WorldHandle>
        CommitMaterialDefinitionReload(
            const MaterialDefinitionReloadBatch& batch) = 0;
};

struct ShaderReloadRuntimeStateSnapshot
{
    uint64_t nextGeneration = 1;
    uint64_t latestObservedSourceEpoch = 0;
    uint64_t latestSubmittedAutoReloadGeneration = 0;
    uint64_t inFlightAutoReloadGeneration = 0;
    uint64_t inFlightAutoReloadSourceEpoch = 0;
    uint64_t latestManualShaderReloadGeneration = 0;
    uint64_t latestManualShaderReloadCommittedGeneration = 0;
    uint64_t latestManualShaderReloadFailedGeneration = 0;
    uint64_t latestAutoReloadStaleDiscardGeneration = 0;
    uint64_t latestAutoReloadFailedGeneration = 0;
    uint64_t latestAutoReloadCommittedGeneration = 0;
    uint64_t latestAutoReloadShadercInvocations = 0;
    uint64_t totalAutoReloadShadercInvocations = 0;
    uint64_t pendingAutoReloadSourceEpoch = 0;
    uint64_t failedPendingAutoReloadSourceEpoch = 0;
    uint64_t pendingMaterialDefinitionSourceEpoch = 0;
    uint64_t failedPendingMaterialDefinitionSourceEpoch = 0;
    uint64_t latestMaterialDefinitionReloadCommittedGeneration = 0;
    uint64_t latestMaterialDefinitionReloadFailedGeneration = 0;
    std::set<std::string> pendingAutoReloadSources;
    std::set<std::string> pendingMaterialDefinitionSources;
    std::vector<std::string> lastSubmittedAutoReloadSources;
    std::vector<std::string> lastStaleAutoReloadSources;
    std::vector<std::string> lastCommittedAutoReloadSources;
    bool workerRunning = false;
    bool workerIdle = true;
    bool workerHasCompletedResult = false;
    uint64_t workerInFlightGeneration = 0;
    bool monitorHasUnstableSourceChanges = false;
    size_t monitorScanCount = 0;
};

class ShaderReloadRuntime
{
public:
    ShaderReloadRuntime() = default;
    ~ShaderReloadRuntime();

    ShaderReloadRuntime(const ShaderReloadRuntime&) = delete;
    ShaderReloadRuntime& operator=(const ShaderReloadRuntime&) = delete;

    void Initialize(
        ShaderCompiler& shaderCompiler,
        ShaderReloadCoordinator& shaderReloadCoordinator,
        const std::filesystem::path& shaderRoot);

    ShaderCompileWorkerShutdownDiagnostics Shutdown();

    void ProcessManualReload(
        const RuntimeCommandExecutionResult& commandResult,
        ShaderReloadRuntimeHost& host,
        const DiagnosticsSubsystem& diagnostics);
    void ProcessAutomaticReloads(
        ShaderReloadRuntimeHost& host,
        const DiagnosticsSubsystem& diagnostics);

    ShaderReloadRuntimeStateSnapshot CaptureSnapshot() const;
    uint64_t AllocateGenerationForValidation() noexcept
    {
        return nextShaderReloadGeneration++;
    }

    MaterialDefinitionReloadBatch
        BuildMaterialDefinitionReloadBatchForValidation(
            uint64_t batchId,
            const std::set<std::string>& sourceIdentities) const;

    void EnableTestCompileGate();
    void ArmTestCompileGateForNextSubmit();
    bool IsWaitingAtTestCompileGate() const;
    void ReleaseTestCompileGate();
    void DisableTestCompileGate();
    void ArmTestPostCompileGateForNextSubmit();
    bool IsWaitingAtTestPostCompileGate() const;
    void SetTestPollInterval(std::chrono::milliseconds interval);
    void SetTestScanSuspended(bool suspended);
    void RefreshBaselineForSources(
        const std::vector<std::string>& sourceIdentities);

private:
    MaterialDefinitionReloadBatch BuildMaterialDefinitionReloadBatch(
        uint64_t batchId,
        const std::set<std::string>& sourceIdentities) const;
    void ProcessPendingMaterialDefinitionReload(
        ShaderReloadRuntimeHost& host,
        const DiagnosticsSubsystem& diagnostics);
    void MergePendingAutomaticShaderSources(
        const std::vector<std::string>& sourceIdentities,
        uint64_t sourceEpoch);
    void SubmitPendingAutomaticShaderReload(
        ShaderReloadRuntimeHost& host,
        const DiagnosticsSubsystem& diagnostics);

    ShaderCompiler* shaderCompiler = nullptr;
    ShaderReloadCoordinator* shaderReloadCoordinator = nullptr;
    std::unique_ptr<ShaderFileMonitor> shaderFileMonitor;
    std::unique_ptr<ShaderCompileWorker> shaderCompileWorker;
    std::filesystem::path shaderRoot;

    uint64_t nextShaderReloadGeneration = 1;
    uint64_t latestObservedSourceEpoch = 0;
    uint64_t latestSubmittedAutoReloadGeneration = 0;
    uint64_t inFlightAutoReloadGeneration = 0;
    uint64_t inFlightAutoReloadSourceEpoch = 0;
    uint64_t latestManualShaderReloadGeneration = 0;
    uint64_t latestManualShaderReloadCommittedGeneration = 0;
    uint64_t latestManualShaderReloadFailedGeneration = 0;
    uint64_t latestAutoReloadStaleDiscardGeneration = 0;
    uint64_t latestAutoReloadFailedGeneration = 0;
    uint64_t latestAutoReloadCommittedGeneration = 0;
    uint64_t latestAutoReloadShadercInvocations = 0;
    uint64_t totalAutoReloadShadercInvocations = 0;
    uint64_t pendingAutoReloadSourceEpoch = 0;
    uint64_t failedPendingAutoReloadSourceEpoch = 0;
    uint64_t pendingMaterialDefinitionSourceEpoch = 0;
    uint64_t failedPendingMaterialDefinitionSourceEpoch = 0;
    uint64_t latestMaterialDefinitionReloadCommittedGeneration = 0;
    uint64_t latestMaterialDefinitionReloadFailedGeneration = 0;
    std::set<std::string> pendingAutoReloadSources;
    std::set<std::string> pendingMaterialDefinitionSources;
    std::vector<std::string> lastSubmittedAutoReloadSources;
    std::vector<std::string> lastStaleAutoReloadSources;
    std::vector<std::string> lastCommittedAutoReloadSources;
};

} // namespace VL
