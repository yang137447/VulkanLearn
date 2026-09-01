#pragma once

// File responsibility: Provides the only test-facing adapter from runtime
// validation state machines to EngineLoop, RenderSystem, RenderGraph, and
// Vulkan renderer owners. Tests receive value snapshots and narrowly named
// operations; they never receive mutable owner pointers.

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <set>
#include <string>
#include <unordered_map>
#include <vector>

#include "core/runtimeResult.h"
#include "shader/reload/shaderReloadRuntime.h"
#include "world/loading/worldGraphTransactionCoordinator.h"
#include "world/worldManager.h"

namespace VL
{

class DiagnosticsSubsystem;
class EngineLoop;
struct RuntimeCommand;
struct RuntimeCommandExecutionResult;

// Identity-only renderer snapshot for rollback validation. It observes owner
// tables without keeping resources alive or exposing renderer owner objects.
struct RuntimeRendererResourceFingerprint
{
    bool captured = false;
    uint64_t worldOwnerGeneration = 0;
    std::unordered_map<std::string, std::uintptr_t> worldTextures;
    std::unordered_map<std::string, std::uintptr_t> renderableObjects;
    std::unordered_map<std::string, std::uintptr_t> materials;
    std::unordered_map<std::string, std::uintptr_t> materialInstances;
    std::unordered_map<std::string, std::uintptr_t> objectResources;
    std::unordered_map<std::string, std::uintptr_t> textures;
    std::unordered_map<std::string, std::uintptr_t> passMaterialInstances;
};

struct RuntimeValidationGraphicsShaderSnapshot
{
    std::weak_ptr<void> material;
    std::uintptr_t surfacePipeline = 0;
    std::uintptr_t shadowPipeline = 0;
    std::string surfaceLogicalBuildId;
    std::string shadowLogicalBuildId;
    std::string surfaceGeneration;
    std::string shadowGeneration;
    std::string manifestDigest;
    std::string resolvedGeneration;
    std::string surfaceVertexDigest;
    std::string surfaceFragmentDigest;
    std::string shadowVertexDigest;
    std::string shadowFragmentDigest;
};

struct RuntimeValidationManualShaderReloadResult
{
    bool succeeded = false;
    std::string failureMessage;
    bool committed = false;
    size_t affectedBuildCount = 0;
    size_t pipelinesCreated = 0;
    size_t pipelinesRetired = 0;
};

struct RuntimeValidationOwnerSnapshot
{
    WorldHandle world;
    std::uintptr_t worldIdentity = 0;
    uint64_t nextWorldGeneration = 0;
    uint64_t worldResourceGeneration = 0;
    uint64_t renderGraphGeneration = 0;
    uint64_t renderSystemGeneration = 0;
    uint64_t controllerGeneration = 0;
    size_t lightCapacity = 0;
    std::vector<uint64_t> lightBufferIds;
    RuntimeRendererResourceFingerprint rendererResources;
    std::string renderGraphGpuFingerprint;
};

struct RuntimeValidationBackendSnapshot
{
    std::array<size_t, 9> identityCounts{};
    std::vector<std::string> imageResourceNames;
    uint32_t swapchainWidth = 0;
    uint32_t swapchainHeight = 0;
};

struct RuntimeValidationWorldPackageIdentities
{
    uint64_t generation = 0;
    std::weak_ptr<void> world;
    std::weak_ptr<void> worldResources;
    std::weak_ptr<void> material;
    std::weak_ptr<void> materialInstance;
    std::weak_ptr<void> objectResources;
    std::weak_ptr<void> primaryTexture;
    std::uintptr_t primaryTextureIdentity = 0;
    bool materialInstanceHasCandidateParameter = false;
};

class RuntimeValidationServices
{
public:
    explicit RuntimeValidationServices(EngineLoop& engineLoop) noexcept;

    std::string GetResourcePath() const;
    std::array<uint32_t, 2> GetConfiguredWindowSize() const;
    int GetDebugViewMode() const noexcept;
    void QueueRuntimeCommand(RuntimeCommand command);

    WorldHandle GetActiveWorldHandle() const;
    RuntimeValidationOwnerSnapshot CaptureOwnerSnapshot() const;
    RuntimeValidationBackendSnapshot CaptureBackendSnapshot() const;
    RuntimeValidationWorldPackageIdentities
        CaptureWorldPackageIdentities() const;
    RuntimeRendererResourceFingerprint
        CaptureRendererResourceFingerprint() const;
    std::string CaptureWorldGraphRuntimeFingerprint(
        const std::string& primaryDefinitionPath,
        const std::string& batchDefinitionPath,
        bool includeFrameLifecycleDiagnostics = true,
        std::string* details = nullptr) const;
    RuntimeValidationGraphicsShaderSnapshot
        CaptureGraphicsShaderSnapshot() const;
    std::string GetComputeShaderGeneration(
        const std::string& shaderName) const;
    RuntimeValidationManualShaderReloadResult
        ExecuteManualGraphicsShaderReload(
            const std::vector<std::string>& sourceIdentities,
            bool injectPipelineFailure);

    ShaderReloadRuntimeStateSnapshot
        CaptureShaderReloadState() const;
    void SetShaderMonitorPollInterval(
        std::chrono::milliseconds interval);
    void SetShaderMonitorScanSuspended(bool suspended);
    void RefreshShaderMonitorBaseline(
        const std::vector<std::string>& sourceIdentities);
    void ArmShaderCompileGate();
    bool IsShaderCompileGateWaiting() const;
    void ReleaseShaderCompileGate();
    void DisableShaderCompileGate();
    void ArmShaderPostCompileGate();
    bool IsShaderPostCompileGateWaiting() const;

    RuntimeResult<WorldHandle> ExecuteWorldGraphTransaction(
        const std::string& scenePath);
    RuntimeResult<WorldHandle>
        ExecuteMaterialDefinitionWorldGraphTransaction(
            const std::set<std::string>& sourceIdentities,
            uint64_t batchId);
    RuntimeResult<void> ReloadRenderGraphResources();
    RuntimeResult<void> RecreateRendererForWindowResize(
        uint32_t width,
        uint32_t height);
    RuntimeResult<void> ResizeWindowAndRenderer(
        uint32_t width,
        uint32_t height);
    void SetWorldGraphTransactionFaultInjection(
        WorldGraphTransactionTestFaultInjection injection) noexcept;
    void ProcessShaderRuntimeRequest(
        const RuntimeCommandExecutionResult& commandResult);
    void WaitForRenderThreadIdle();

    size_t GetPendingRetiredResourceCount() const noexcept;
    std::weak_ptr<void> FindPendingRetiredResource(
        const std::string& label,
        uint64_t generation) const;
    std::uintptr_t GetWorldTextureIdentity(
        const std::string& key) const;

    bool IsClosing() const noexcept;
    int GetExitCode() const noexcept;
    void MarkClosingTestFailure() noexcept;
    void MarkShutdownTestSucceeded() noexcept;

private:
    EngineLoop* engineLoop = nullptr;
};

} // namespace VL
