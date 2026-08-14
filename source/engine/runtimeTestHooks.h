#pragma once

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <string>
#include <unordered_map>
#include <vector>

#include "baseStructs.h"
#include "engine/runtimeCommand.h"
#include "engine/testing/runtimeValidationServices.h"
#include "render/environment/environmentUpdateDiagnostics.h"

namespace VL
{

class DiagnosticsSubsystem;
class WorldManager;
struct ShaderCompileWorkerShutdownDiagnostics;
struct RuntimeCommandExecutionResult;
struct WorldGraphTransactionTestFaultInjection;

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
    ~RuntimeTestHooks();

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
    bool BeginShaderReloadTest(
        const std::string& resourcePath,
        const DiagnosticsSubsystem& diagnostics);
    bool BeginShaderAutoReloadTest(
        const std::string& resourcePath,
        const DiagnosticsSubsystem& diagnostics);
    bool BeginShaderComputeReloadTest(
        const std::string& resourcePath,
        const DiagnosticsSubsystem& diagnostics);
    bool BeginShaderDefinitionReloadTest(
        const std::string& resourcePath,
        const DiagnosticsSubsystem& diagnostics);
    bool BeginWorldGraphTransactionTest(
        const std::string& resourcePath,
        const DiagnosticsSubsystem& diagnostics);
    bool BeginShaderUiReloadTest(
        const std::string& resourcePath,
        const DiagnosticsSubsystem& diagnostics);
    bool BeginShaderShutdownInflightTest(
        const DiagnosticsSubsystem& diagnostics);
    bool FinalizeShaderShutdownInflightTestAfterWorkerShutdown(
        RuntimeValidationServices& validationServices,
        const ShaderCompileWorkerShutdownDiagnostics& workerDiagnostics,
        const DiagnosticsSubsystem& diagnostics);
    void Update(
        CommandBus& commandBus,
        const WorldManager& worldManager,
        const EnvironmentUpdateDiagnostics& environmentDiagnostics,
        RuntimeValidationServices& validationServices,
        const DiagnosticsSubsystem& diagnostics);
    void UpdateEngineLoopTests(
        RuntimeValidationServices& validationServices,
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
    enum class WorldGraphTransactionTestPhase;

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
        RuntimeValidationServices& validationServices,
        const DiagnosticsSubsystem& diagnostics);
    void UpdateResizeStress(
        RuntimeValidationServices& validationServices,
        const DiagnosticsSubsystem& diagnostics);
    void UpdateRenderGraphReloadStress(
        RuntimeValidationServices& validationServices,
        const DiagnosticsSubsystem& diagnostics);
    void UpdateShaderReloadTest(
        RuntimeValidationServices& validationServices,
        const DiagnosticsSubsystem& diagnostics);
    void UpdateShaderAutoReloadTest(
        RuntimeValidationServices& validationServices,
        const DiagnosticsSubsystem& diagnostics);
    void UpdateShaderComputeReloadTest(
        RuntimeValidationServices& validationServices,
        const DiagnosticsSubsystem& diagnostics);
    void UpdateShaderDefinitionReloadTest(
        RuntimeValidationServices& validationServices,
        const DiagnosticsSubsystem& diagnostics);
    void UpdateWorldGraphTransactionTest(
        RuntimeValidationServices& validationServices,
        const DiagnosticsSubsystem& diagnostics);
    void ValidateWorldGraphTransactionFailure(
        RuntimeValidationServices& validationServices,
        const WorldGraphTransactionTestFaultInjection& injection,
        bool graphOnly,
        bool materialDefinitionTransaction,
        const std::string& phaseName,
        const std::string& expectedFailureText,
        const DiagnosticsSubsystem& diagnostics);
    void AdvanceWorldGraphTransactionTestAfterRenderedFrames(
        WorldGraphTransactionTestPhase nextPhase) noexcept;
    std::string CaptureShaderDefinitionRuntimeFingerprint(
        RuntimeValidationServices& validationServices) const;
    std::string CaptureWorldGraphRuntimeFingerprint(
        RuntimeValidationServices& validationServices,
        const std::string& primaryDefinitionPath,
        const std::string& batchDefinitionPath,
        bool includeFrameLifecycleDiagnostics = true,
        std::string* details = nullptr) const;
    void UpdateShaderUiReloadTest(
        RuntimeValidationServices& validationServices,
        const DiagnosticsSubsystem& diagnostics);
    void UpdateShaderShutdownInflightTest(
        RuntimeValidationServices& validationServices,
        const DiagnosticsSubsystem& diagnostics);
    std::string CaptureShaderShutdownInflightFingerprint(
        RuntimeValidationServices& validationServices) const;
    bool SurfaceArtifactDependsOnSourceWithDigest(
        RuntimeValidationServices& validationServices,
        const std::string& surfaceLogicalBuildId,
        const std::string& dependencyIdentity,
        const std::string& expectedDigest);
    bool UiArtifactFragmentMatchesCurrentSource(
        RuntimeValidationServices& validationServices) const;
    void FailShaderReloadTest(
        const std::string& message,
        const DiagnosticsSubsystem& diagnostics);
    void FailShaderAutoReloadTest(
        RuntimeValidationServices* validationServices,
        const std::string& message,
        const DiagnosticsSubsystem& diagnostics);
    void FailShaderComputeReloadTest(
        const std::string& message,
        const DiagnosticsSubsystem& diagnostics);
    void FailShaderDefinitionReloadTest(
        const std::string& message,
        const DiagnosticsSubsystem& diagnostics);
    void FailWorldGraphTransactionTest(
        RuntimeValidationServices* validationServices,
        const std::string& message,
        const DiagnosticsSubsystem& diagnostics);
    void FailShaderUiReloadTest(
        const std::string& message,
        const DiagnosticsSubsystem& diagnostics);
    void FailShaderShutdownInflightTest(
        RuntimeValidationServices& validationServices,
        const std::string& message,
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

    enum class ShaderReloadTestPhase
    {
        Idle,
        WaitWorldLoad,
        CompatibleCommit,
        SyntaxFailure,
        SyntaxRecovery,
        AbiRejection,
        PipelineFailure,
        PipelineRecovery,
        RestoreOriginal,
        WaitRetireDrain,
    };

    std::string shaderReloadTestFixtureDirectory;
    std::string shaderReloadTestScenePath;
    std::string shaderReloadTestSourcePath;
    std::string shaderReloadTestOriginalSource;
    std::string shaderReloadTestCompatibleSourceA;
    std::string shaderReloadTestSyntaxErrorSource;
    std::string shaderReloadTestCompatibleSourceB;
    std::string shaderReloadTestAbiIncompatibleSource;
    bool shaderReloadTestActive = false;
    bool waitingForShaderReloadTestWorld = false;
    int shaderReloadTestRetireDrainFramesRemaining = 0;
    size_t shaderReloadTestMaxPendingRetiredResources = 0;
    ShaderReloadTestPhase shaderReloadTestPhase =
        ShaderReloadTestPhase::Idle;

    enum class ShaderAutoReloadTestPhase
    {
        Idle,
        WaitWorldLoad,
        WaitA1Gate,
        WaitA2Stable,
        WaitA3Stable,
        WaitA1Stale,
        WaitA3Commit,
        WaitUnionGate,
        WaitUnionYStable,
        WaitUnionZStable,
        WaitUnionStale,
        WaitUnionCommit,
        WaitDeleteGate,
        WaitDeleteCandidateRejected,
        WaitDeleteFailure,
        WaitDeleteRecovery,
        WaitMtimeOnlyScan,
        WaitManualGate,
        WaitManualCommit,
        WaitManualStale,
        WaitSyntaxFailure,
        WaitSyntaxRecovery,
        RestoreOriginal,
        WaitRetireDrain,
    };

    std::string shaderAutoReloadTestFixtureDirectory;
    std::string shaderAutoReloadTestScenePath;
    std::string shaderAutoReloadTestSourcePath;
    std::string shaderAutoReloadTestVertexSourcePath;
    std::string shaderAutoReloadTestSurfaceSourcePath;
    std::string shaderAutoReloadTestOriginalSource;
    std::string shaderAutoReloadTestOriginalVertexSource;
    std::string shaderAutoReloadTestOriginalSurfaceSource;
    std::string shaderAutoReloadTestCompatibleSourceA;
    std::string shaderAutoReloadTestSyntaxErrorSource;
    std::string shaderAutoReloadTestCompatibleSourceB;
    std::string shaderAutoReloadTestRapidSourceC1;
    std::string shaderAutoReloadTestRapidSourceC2;
    std::string shaderAutoReloadTestRapidSourceC3;
    std::string shaderAutoReloadTestLeafSourceA;
    std::string shaderAutoReloadTestLeafSourceB;
    std::string shaderAutoReloadTestLeafSourceC;
    std::string shaderAutoReloadTestSurfaceSourceA;
    std::string shaderAutoReloadTestSurfaceSourceB;
    std::string shaderAutoReloadTestSurfaceSourceC;
    std::string shaderAutoReloadTestMtimeOnlySource;
    std::vector<std::string> shaderAutoReloadTestUnionSources;
    std::vector<std::string> shaderAutoReloadTestLastSubmittedSources;
    bool shaderAutoReloadTestActive = false;
    bool waitingForShaderAutoReloadTestWorld = false;
    bool shaderAutoReloadTestPhaseEntryPending = false;
    bool shaderAutoReloadTestQueueManualReload = false;
    bool shaderAutoReloadTestManualReloadQueued = false;
    std::filesystem::file_time_type
        shaderAutoReloadTestOriginalWriteTime{};
    bool shaderAutoReloadTestOriginalWriteTimeCaptured = false;
    uint64_t shaderAutoReloadTestManualGeneration = 0;
    std::chrono::steady_clock::time_point
        shaderAutoReloadTestDeadline{};
    int shaderAutoReloadTestRetireDrainFramesRemaining = 0;
    size_t shaderAutoReloadTestMaxPendingRetiredResources = 0;
    uint64_t shaderAutoReloadTestBaselineLatestGeneration = 0;
    std::string shaderAutoReloadTestBaselineSurfaceGeneration;
    std::string shaderAutoReloadTestBaselineShadowGeneration;
    std::string shaderAutoReloadTestBaselineManifestDigest;
    std::string shaderAutoReloadTestBaselineResolvedGeneration;
    std::string shaderAutoReloadTestInitialSurfaceGeneration;
    std::string shaderAutoReloadTestInitialShadowGeneration;
    std::string shaderAutoReloadTestInitialManifestDigest;
    std::string shaderAutoReloadTestInitialResolvedGeneration;
    std::string shaderAutoReloadTestLastObservedSurfaceGeneration;
    int shaderAutoReloadTestCommitTransitions = 0;
    uint64_t shaderAutoReloadTestBaselineObservedEpoch = 0;
    uint64_t shaderAutoReloadTestBaselineSubmittedGeneration = 0;
    uint64_t shaderAutoReloadTestBaselineCommittedGeneration = 0;
    uint64_t shaderAutoReloadTestBaselineFailedGeneration = 0;
    uint64_t shaderAutoReloadTestBaselineShadercInvocations = 0;
    uint64_t shaderAutoReloadTestBaselineMonitorScanCount = 0;
    uint64_t shaderAutoReloadTestGateGeneration = 0;
    uint64_t shaderAutoReloadTestDeleteRejectedGeneration = 0;
    ShaderAutoReloadTestPhase shaderAutoReloadTestPhase =
        ShaderAutoReloadTestPhase::Idle;

    enum class ShaderComputeReloadTestPhase
    {
        Idle,
        WaitWorldLoad,
        WaitCompatibleCommit,
        WaitAbiRejection,
        RestoreOriginal,
        WaitRetireDrain,
    };

    std::string shaderComputeReloadTestFixtureDirectory;
    std::string shaderComputeReloadTestScenePath;
    std::string shaderComputeSkyShSourcePath;
    std::string shaderComputePrefilterSourcePath;
    std::string shaderComputeSkyShOriginal;
    std::string shaderComputePrefilterOriginal;
    std::string shaderComputeSkyShCompatible;
    std::string shaderComputePrefilterCompatible;
    std::string shaderComputeSkyShAbiIncompatible;
    bool shaderComputeReloadTestActive = false;
    bool waitingForShaderComputeReloadTestWorld = false;
    bool shaderComputeReloadTestPhaseEntryPending = false;
    std::chrono::steady_clock::time_point
        shaderComputeReloadTestDeadline{};
    int shaderComputeReloadTestRetireDrainFramesRemaining = 0;
    size_t shaderComputeReloadTestMaxPendingRetiredResources = 0;
    uint64_t shaderComputeReloadTestBaselineLatestGeneration = 0;
    std::string shaderComputeSkyShBaselineGeneration;
    std::string shaderComputePrefilterBaselineGeneration;
    ShaderComputeReloadTestPhase shaderComputeReloadTestPhase =
        ShaderComputeReloadTestPhase::Idle;

    enum class ShaderDefinitionReloadTestPhase
    {
        Idle,
        WaitWorldLoad,
        WaitAddedParameter,
        WaitDeletedParameter,
        WaitTypeMismatchRetention,
        WaitRequiredTextureFailure,
        WaitMalformedDefinitionFailure,
        WaitIncludeGenerationFailure,
        WaitMultiDefinitionFailure,
        WaitMultiDefinitionRecovery,
        RestoreOriginal,
        WaitRetireDrain,
    };

    std::string shaderDefinitionReloadTestFixtureDirectory;
    std::string shaderDefinitionReloadTestScenePath;
    std::string shaderDefinitionReloadTestSourcePath;
    std::string shaderDefinitionReloadTestBatchSourcePath;
    std::string shaderDefinitionReloadTestOriginal;
    std::string shaderDefinitionReloadTestBatchOriginal;
    std::string shaderDefinitionReloadTestExtended;
    std::string shaderDefinitionReloadTestDeleted;
    std::string shaderDefinitionReloadTestTypeMismatch;
    std::string shaderDefinitionReloadTestRequiredTextureMissing;
    std::string shaderDefinitionReloadTestMalformed;
    std::string shaderDefinitionReloadTestIncludeGenerationFailure;
    std::string shaderDefinitionReloadTestMultiMain;
    std::string shaderDefinitionReloadTestMultiBatchInvalid;
    std::string shaderDefinitionReloadTestMultiBatchValid;
    bool shaderDefinitionReloadTestActive = false;
    bool waitingForShaderDefinitionReloadTestWorld = false;
    bool shaderDefinitionReloadTestPhaseEntryPending = false;
    std::chrono::steady_clock::time_point
        shaderDefinitionReloadTestDeadline{};
    int shaderDefinitionReloadTestRetireDrainFramesRemaining = 0;
    size_t shaderDefinitionReloadTestMaxPendingRetiredResources = 0;
    uint64_t shaderDefinitionReloadTestBaselineWorldGeneration = 0;
    uint64_t shaderDefinitionReloadTestBaselineFailedGeneration = 0;
    uint64_t shaderDefinitionReloadTestBaselineCommittedGeneration = 0;
    int shaderDefinitionReloadTestBaselineParameterCount = 0;
    std::string shaderDefinitionReloadTestBaselineFingerprint;
    std::weak_ptr<void> shaderDefinitionReloadTestOldMaterial;
    std::weak_ptr<void> shaderDefinitionReloadTestOldMaterialInstance;
    std::weak_ptr<void> shaderDefinitionReloadTestOldObjectResources;
    std::weak_ptr<void> shaderDefinitionReloadTestOldPrimaryTexture;
    std::weak_ptr<void> shaderDefinitionReloadTestOldRetiredTexture;
    std::weak_ptr<void> shaderDefinitionReloadTestRetainedTexture;
    std::uintptr_t shaderDefinitionReloadTestRetainedTextureIdentity = 0;
    std::string shaderDefinitionReloadTestRetainedTextureAssetIdentity;
    uint64_t shaderDefinitionReloadTestInitialDescriptorPoolIdentity = 0;
    ShaderDefinitionReloadTestPhase shaderDefinitionReloadTestPhase =
        ShaderDefinitionReloadTestPhase::Idle;

    enum class WorldGraphTransactionTestPhase
    {
        Idle,
        WaitWorldLoad,
        GraphResourceFailure,
        RenderPassFailure,
        FramebufferFailure,
        PassMaterialContractFailure,
        DescriptorFailure,
        CandidateWorldFailure,
        ViewTargetFailure,
        RuntimeBindingFailure,
        BeforeCommitFailure,
        MaterialDefinitionRuntimeFailure,
        GraphOnlyResourceFailure,
        GraphOnlyDescriptorFailure,
        GraphOnlyBeforeCommitFailure,
        SuccessCommit,
        GraphOnlySuccess,
        MinimizeNoop,
        ResizeSuccess,
        ShaderReloadSuccess,
        MaterialDefinitionSuccess,
        RestoreOriginalCommit,
        WaitRetireDrain,
        ResizeFatalFailure,
    };

    std::string worldGraphTransactionTestFixtureDirectory;
    std::string worldGraphTransactionTestScenePath;
    std::string worldGraphTransactionTestHighLightScenePath;
    std::string worldGraphTransactionTestSourcePath;
    std::string worldGraphTransactionTestBatchSourcePath;
    std::string worldGraphTransactionTestOriginalSource;
    std::string worldGraphTransactionTestCandidateSource;
    bool worldGraphTransactionTestActive = false;
    bool waitingForWorldGraphTransactionTestWorld = false;
    bool worldGraphTransactionTestMonitorSuspended = false;
    int worldGraphTransactionTestFramesUntilNextPhase = 0;
    int worldGraphTransactionTestRetireDrainFramesRemaining = 0;
    size_t worldGraphTransactionTestMaxPendingRetiredResources = 0;
    uint64_t worldGraphTransactionTestNextBatchId = 1;
    uint64_t worldGraphTransactionTestGenerationBeforeSuccess = 0;
    std::array<size_t, 9>
        worldGraphTransactionTestBackendCountsBeforeSuccess{};
    std::vector<std::string>
        worldGraphTransactionTestImageResourceNamesBeforeSuccess;
    std::weak_ptr<void> worldGraphTransactionTestOldWorld;
    std::weak_ptr<void>
        worldGraphTransactionTestOldWorldPackage;
    std::weak_ptr<void>
        worldGraphTransactionTestOldGraphPackage;
    std::weak_ptr<void>
        worldGraphTransactionTestGraphReloadPackage;
    std::weak_ptr<void>
        worldGraphTransactionTestOldLightBuffer;
    std::weak_ptr<void> worldGraphTransactionTestOldMaterial;
    std::weak_ptr<void> worldGraphTransactionTestOldMaterialInstance;
    std::weak_ptr<void> worldGraphTransactionTestOldObjectResources;
    std::weak_ptr<void> worldGraphTransactionTestOldTexture;
    std::vector<std::weak_ptr<void>>
        worldGraphTransactionTestResourcesExpectedToExpire;
    WorldGraphTransactionTestPhase worldGraphTransactionTestPhase =
        WorldGraphTransactionTestPhase::Idle;

    enum class ShaderUiReloadTestPhase
    {
        Idle,
        WaitWorldLoad,
        WaitCompatibleCommit,
        WaitAbiRejection,
        RestoreOriginal,
        WaitRetireDrain,
    };

    std::string shaderUiReloadTestFixtureDirectory;
    std::string shaderUiReloadTestScenePath;
    std::string shaderUiReloadTestVertexPath;
    std::string shaderUiReloadTestFragmentPath;
    std::string shaderUiReloadTestVertexOriginal;
    std::string shaderUiReloadTestFragmentOriginal;
    std::string shaderUiReloadTestFragmentCompatible;
    std::string shaderUiReloadTestFragmentAbiIncompatible;
    bool shaderUiReloadTestActive = false;
    bool waitingForShaderUiReloadTestWorld = false;
    bool shaderUiReloadTestPhaseEntryPending = false;
    std::chrono::steady_clock::time_point
        shaderUiReloadTestDeadline{};
    int shaderUiReloadTestRetireDrainFramesRemaining = 0;
    size_t shaderUiReloadTestMaxPendingRetiredResources = 0;
    uint64_t shaderUiReloadTestBaselineLatestGeneration = 0;
    ShaderUiReloadTestPhase shaderUiReloadTestPhase =
        ShaderUiReloadTestPhase::Idle;

    enum class ShaderShutdownInflightTestPhase
    {
        Idle,
        WaitMonitorBaseline,
        WaitPostCompileGate,
        AwaitShutdown,
    };

    std::string shaderShutdownInflightSourcePath;
    std::string shaderShutdownInflightOriginalSource;
    std::string shaderShutdownInflightCandidateSource;
    std::string shaderShutdownInflightBaselineFingerprint;
    bool shaderShutdownInflightTestActive = false;
    uint64_t shaderShutdownInflightBaselineCommittedGeneration = 0;
    uint64_t shaderShutdownInflightCandidateGeneration = 0;
    std::chrono::steady_clock::time_point
        shaderShutdownInflightDeadline{};
    ShaderShutdownInflightTestPhase
        shaderShutdownInflightTestPhase =
            ShaderShutdownInflightTestPhase::Idle;

    RuntimeTestStatus runtimeTestStatus = RuntimeTestStatus::Idle;
};

} // namespace VL
