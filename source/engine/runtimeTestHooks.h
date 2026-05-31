#pragma once

#include <cstddef>
#include <string>

#include "engine/runtimeCommand.h"

namespace VL
{

class DiagnosticsSubsystem;
struct RuntimeCommandExecutionResult;

enum class RuntimeTestStatus
{
    Idle,
    Running,
    Succeeded,
    Failed
};

// Runtime-only validation hooks for UE-Lite migration work. These hooks drive
// systems through the public command path, so test pressure uses the same
// WorldTransitionCoordinator and EngineLoop rebinding path as a user command.
class RuntimeTestHooks
{
public:
    bool BeginWorldReloadStress(
        std::string scenePath,
        int reloadCount,
        const DiagnosticsSubsystem& diagnostics);
    bool BeginWorldReloadFailureRollbackTest(
        std::string scenePath,
        const DiagnosticsSubsystem& diagnostics);
    bool BeginGeneratedMaterialFailureRollbackTest(
        const std::string& resourcePath,
        const DiagnosticsSubsystem& diagnostics);
    bool BeginGeneratedHighLightReloadStress(
        const std::string& resourcePath,
        int reloadCount,
        const DiagnosticsSubsystem& diagnostics);
    void Update(CommandBus& commandBus, const DiagnosticsSubsystem& diagnostics);
    void NotifyCommandResult(
        const RuntimeCommandExecutionResult& commandResult,
        const DiagnosticsSubsystem& diagnostics);

    bool IsWorldReloadStressActive() const { return worldReloadStressActive; }
    RuntimeTestStatus GetRuntimeTestStatus() const { return runtimeTestStatus; }
    RuntimeTestStatus GetWorldReloadStressStatus() const { return runtimeTestStatus; }

private:
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
    bool failureRollbackTestActive = false;
    bool waitingForFailureRollbackResult = false;
    bool cleanupGeneratedFailureFixture = false;
    std::string generatedFailureFixtureDirectory;

    RuntimeTestStatus runtimeTestStatus = RuntimeTestStatus::Idle;
};

} // namespace VL
