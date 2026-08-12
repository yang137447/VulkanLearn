#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <unordered_map>

#include "baseStructs.h"
#include "engine/runtimeCommand.h"
#include "render/environment/environmentUpdateDiagnostics.h"

namespace VL
{

class DiagnosticsSubsystem;
class EngineLoop;
class WorldManager;
struct RuntimeCommandExecutionResult;

// Identity-only renderer snapshot for rollback validation. It observes owner
// tables without keeping resources alive or exposing backend objects.
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

enum class RuntimeTestStatus
{
    Idle,
    Running,
    Succeeded,
    Failed
};

// Single owner for runtime validation state machines. Tests drive systems
// through production command/lifecycle paths; EngineLoop only provides stable
// frame hooks and the renderer operations it already owns.
class RuntimeTestHooks
{
public:
    bool BeginWorldReloadStress(
        std::string scenePath,
        int reloadCount,
        const DiagnosticsSubsystem& diagnostics);
    bool BeginWorldReloadFailureRollbackTest(
        std::string scenePath,
        const DiagnosticsSubsystem& diagnostics,
        std::string expectedErrorCode = {});
    bool BeginGeneratedMaterialFailureRollbackTest(
        const std::string& resourcePath,
        const DiagnosticsSubsystem& diagnostics);
    bool BeginGeneratedMeshFailureRollbackTest(
        const std::string& resourcePath,
        const DiagnosticsSubsystem& diagnostics);
    bool BeginGeneratedTextureFailureRollbackTest(
        const std::string& resourcePath,
        const DiagnosticsSubsystem& diagnostics);
    bool BeginGeneratedHighLightReloadStress(
        const std::string& resourcePath,
        int reloadCount,
        const DiagnosticsSubsystem& diagnostics);
    bool BeginResizeStress(
        int resizeCount,
        const DiagnosticsSubsystem& diagnostics);
    bool BeginRenderGraphReloadStress(
        int reloadCount,
        const DiagnosticsSubsystem& diagnostics);
    bool BeginFrameSmokeTest(
        int frameCount,
        const DiagnosticsSubsystem& diagnostics);
    bool BeginEnvironmentUpdateStress(
        int updateCount,
        const DiagnosticsSubsystem& diagnostics);
    void Update(
        CommandBus& commandBus,
        const WorldManager& worldManager,
        const EnvironmentUpdateDiagnostics& environmentDiagnostics,
        const DiagnosticsSubsystem& diagnostics);
    void UpdateEngineLoopTests(
        EngineLoop& engineLoop,
        const DiagnosticsSubsystem& diagnostics);
    bool ShouldCollectFrameTiming() const { return frameSmokeActive; }
    void RecordFrameRenderLoopTime(double renderLoopTimeMs);
    void RecordFrameTime(
        double frameTimeMs,
        const DiagnosticsSubsystem& diagnostics);
    bool ShouldSuppressResizeEvent(uint32_t width, uint32_t height);
    void NotifyProceduralSkyParametersResult(
        bool succeeded,
        const DiagnosticsSubsystem& diagnostics);
    void NotifyCommandResult(
        const RuntimeCommandExecutionResult& commandResult,
        const DiagnosticsSubsystem& diagnostics);

    RuntimeTestStatus GetRuntimeTestStatus() const { return runtimeTestStatus; }

private:
    enum class EnvironmentUpdateStressPhase
    {
        Idle,
        WaitInitialGeneration,
        RequestMutation,
        WaitMutation,
        RequestRestore,
        WaitRestore,
        WaitTimingDrain
    };

    void UpdateEnvironmentUpdateStress(
        CommandBus& commandBus,
        const WorldManager& worldManager,
        const EnvironmentUpdateDiagnostics& environmentDiagnostics,
        const DiagnosticsSubsystem& diagnostics);
    void UpdateResizeStress(
        EngineLoop& engineLoop,
        const DiagnosticsSubsystem& diagnostics);
    void UpdateRenderGraphReloadStress(
        EngineLoop& engineLoop,
        const DiagnosticsSubsystem& diagnostics);
    void ReportFrameSmokeInterval(const DiagnosticsSubsystem& diagnostics);
    void FailEnvironmentUpdateStress(
        const std::string& message,
        const DiagnosticsSubsystem& diagnostics);

    std::string worldReloadStressScenePath;
    int totalWorldReloads = 0;
    int remainingWorldReloads = 0;
    int completedWorldReloads = 0;
    int retireDrainFramesRemaining = 0;
    size_t maxPendingRetiredResources = 0;
    bool worldReloadStressActive = false;
    bool waitingForWorldReloadResult = false;
    bool waitingForRetireDrain = false;
    bool cleanupGeneratedReloadStressFixture = false;
    std::string generatedReloadStressFixtureDirectory;

    std::string failureRollbackScenePath;
    std::string failureRollbackExpectedErrorCode;
    bool failureRollbackTestActive = false;
    bool waitingForFailureRollbackResult = false;
    bool cleanupGeneratedFailureFixture = false;
    std::string generatedFailureFixtureDirectory;

    bool resizeStressActive = false;
    int resizeStressTotal = 0;
    int resizeStressRemaining = 0;
    int resizeStressCompletedCount = 0;
    bool suppressNextResizeEvent = false;
    uint32_t suppressedResizeWidth = 0;
    uint32_t suppressedResizeHeight = 0;

    bool graphReloadStressActive = false;
    bool graphReloadStressWaitingForDrain = false;
    int graphReloadStressTotal = 0;
    int graphReloadStressRemaining = 0;
    int graphReloadStressCompletedCount = 0;
    int graphReloadRetireDrainFramesRemaining = 0;
    size_t graphReloadMaxPendingRetiredResources = 0;

    bool frameSmokeActive = false;
    int frameSmokeTotal = 0;
    int frameSmokeCompletedCount = 0;
    double frameSmokeTotalMs = 0.0;
    double frameSmokeMaxMs = 0.0;
    double frameSmokeMinMs = 0.0;
    int frameSmokeIntervalSize = 5000;
    int frameSmokeIntervalFrameCount = 0;
    double frameSmokeIntervalTotalMs = 0.0;
    double frameSmokeIntervalMaxMs = 0.0;
    double frameSmokeIntervalMinMs = 0.0;
    double frameSmokeIntervalRenderLoopTotalMs = 0.0;
    double frameSmokeIntervalRenderLoopMaxMs = 0.0;

    // 环境压力测试只持有测试状态和冻结基线；参数修改继续通过 CommandBus 进入 owner 侧。
    int environmentUpdateStressTotal = 0;
    int environmentUpdateStressCompletedCount = 0;
    int environmentUpdateStressFrameBudget = 0;
    uint64_t environmentUpdateStressPreviousActiveGeneration = 0;
    bool environmentUpdateStressActive = false;
    bool environmentUpdateStressObservedPreviousResources = false;
    bool waitingForProceduralSkyParametersResult = false;
    SkyParametersGPU environmentUpdateStressOriginalSkyParameters;
    std::array<uint64_t, 4> environmentUpdateStressBaselineTimingSamples{};
    uint32_t environmentUpdateStressPrefilterMipCount = 0;
    std::uintptr_t environmentUpdateStressEnvironmentCubeIdentity = 0;
    std::uintptr_t environmentUpdateStressPrefilterCubeIdentity = 0;
    EnvironmentUpdateStressPhase environmentUpdateStressPhase =
        EnvironmentUpdateStressPhase::Idle;

    RuntimeTestStatus runtimeTestStatus = RuntimeTestStatus::Idle;
};

} // namespace VL
