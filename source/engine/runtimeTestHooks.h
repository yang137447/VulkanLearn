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

    bool BeginShaderReloadTest(
        const std::string& resourcePath,
        const DiagnosticsSubsystem& diagnostics);
    bool BeginShaderComputeReloadTest(
        const std::string& resourcePath,
        const DiagnosticsSubsystem& diagnostics);
    bool BeginWorldGraphTransactionTest(
        const std::string& resourcePath,
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
    void NotifyCommandResult(
        const RuntimeCommandExecutionResult& commandResult,
        const DiagnosticsSubsystem& diagnostics);

    RuntimeTestStatus GetRuntimeTestStatus() const { return runtimeTestStatus; }

private:
    enum class WorldGraphTransactionTestPhase;

    void UpdateShaderReloadTest(
        RuntimeValidationServices& validationServices,
        const DiagnosticsSubsystem& diagnostics);
    void UpdateShaderComputeReloadTest(
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
    std::string CaptureWorldGraphRuntimeFingerprint(
        RuntimeValidationServices& validationServices,
        const std::string& primaryDefinitionPath,
        const std::string& batchDefinitionPath,
        bool includeFrameLifecycleDiagnostics = true,
        std::string* details = nullptr) const;
    void FailShaderReloadTest(
        const std::string& message,
        const DiagnosticsSubsystem& diagnostics);
    void FailShaderComputeReloadTest(
        const std::string& message,
        const DiagnosticsSubsystem& diagnostics);
    void FailWorldGraphTransactionTest(
        RuntimeValidationServices* validationServices,
        const std::string& message,
        const DiagnosticsSubsystem& diagnostics);
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

    RuntimeTestStatus runtimeTestStatus = RuntimeTestStatus::Idle;
};

} // namespace VL
