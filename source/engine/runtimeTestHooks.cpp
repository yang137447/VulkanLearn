#include "engine/runtimeTestHooks.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <filesystem>
#include <iterator>
#include <limits>
#include <memory>
#include <sstream>
#include <stdexcept>
#include <string>
#include <utility>

#include "engine/diagnosticsSubsystem.h"
#include "engine/runtimeCommandExecutor.h"
#include "engine/testing/runtimeTestFixtures.h"
#include "engine/testing/runtimeValidationServices.h"
#include "material.h"
#include "materialInstance.h"
#include "material/generator/materialParameterIncludeGenerator.h"
#include "render/backend/rendererObjectResourceRegistry.h"
#include "render/resource/rendererResourceCache.h"
#include "render/resource/resourceRetireQueue.h"
#include "shader/build/atomicFile.h"
#include "shader/reload/shaderCompileWorker.h"
#include "world/world.h"
#include "world/worldManager.h"

namespace VL
{
using namespace RuntimeTestFixtures;
RuntimeTestHooks::~RuntimeTestHooks()
{
    if (!shaderReloadTestSourcePath.empty() &&
        !shaderReloadTestOriginalSource.empty())
    {
        try
        {
            WriteTextFileAtomically(
                shaderReloadTestSourcePath,
                shaderReloadTestOriginalSource);
        }
        catch (...)
        {
        }
    }
    if (!shaderReloadTestFixtureDirectory.empty())
    {
        std::error_code removeError;
        std::filesystem::remove_all(
            shaderReloadTestFixtureDirectory,
            removeError);
    }
    if (!shaderAutoReloadTestSourcePath.empty() &&
        !shaderAutoReloadTestOriginalSource.empty())
    {
        try
        {
            WriteTextFileAtomically(
                shaderAutoReloadTestSourcePath,
                shaderAutoReloadTestOriginalSource);
        }
        catch (...)
        {
        }
    }
    if (!shaderAutoReloadTestVertexSourcePath.empty() &&
        !shaderAutoReloadTestOriginalVertexSource.empty())
    {
        try
        {
            WriteTextFileAtomically(
                shaderAutoReloadTestVertexSourcePath,
                shaderAutoReloadTestOriginalVertexSource);
        }
        catch (...)
        {
        }
    }
    if (!shaderAutoReloadTestSurfaceSourcePath.empty() &&
        !shaderAutoReloadTestOriginalSurfaceSource.empty())
    {
        try
        {
            WriteTextFileAtomically(
                shaderAutoReloadTestSurfaceSourcePath,
                shaderAutoReloadTestOriginalSurfaceSource);
        }
        catch (...)
        {
        }
    }
    if (!shaderAutoReloadTestFixtureDirectory.empty())
    {
        std::error_code removeError;
        std::filesystem::remove_all(
            shaderAutoReloadTestFixtureDirectory,
            removeError);
    }
    if (!shaderComputeSkyShSourcePath.empty() &&
        !shaderComputeSkyShOriginal.empty())
    {
        try
        {
            WriteTextFileAtomically(
                shaderComputeSkyShSourcePath,
                shaderComputeSkyShOriginal);
        }
        catch (...)
        {
        }
    }
    if (!shaderComputePrefilterSourcePath.empty() &&
        !shaderComputePrefilterOriginal.empty())
    {
        try
        {
            WriteTextFileAtomically(
                shaderComputePrefilterSourcePath,
                shaderComputePrefilterOriginal);
        }
        catch (...)
        {
        }
    }
    if (!shaderComputeReloadTestFixtureDirectory.empty())
    {
        std::error_code removeError;
        std::filesystem::remove_all(
            shaderComputeReloadTestFixtureDirectory,
            removeError);
    }
    if (!shaderDefinitionReloadTestSourcePath.empty() &&
        !shaderDefinitionReloadTestOriginal.empty())
    {
        try
        {
            WriteTextFileAtomically(
                shaderDefinitionReloadTestSourcePath,
                shaderDefinitionReloadTestOriginal);
            try
            {
                MaterialParameterIncludeGenerator::GenerateInclude(
                    shaderDefinitionReloadTestSourcePath);
            }
            catch (...)
            {
            }
        }
        catch (...)
        {
        }
    }
    if (!shaderDefinitionReloadTestBatchSourcePath.empty() &&
        !shaderDefinitionReloadTestBatchOriginal.empty())
    {
        try
        {
            WriteTextFileAtomically(
                shaderDefinitionReloadTestBatchSourcePath,
                shaderDefinitionReloadTestBatchOriginal);
            try
            {
                MaterialParameterIncludeGenerator::GenerateInclude(
                    shaderDefinitionReloadTestBatchSourcePath);
            }
            catch (...)
            {
            }
        }
        catch (...)
        {
        }
    }
    if (!shaderDefinitionReloadTestFixtureDirectory.empty())
    {
        std::error_code removeError;
        std::filesystem::remove_all(
            shaderDefinitionReloadTestFixtureDirectory,
            removeError);
    }
    if (!worldGraphTransactionTestSourcePath.empty() &&
        !worldGraphTransactionTestOriginalSource.empty())
    {
        try
        {
            WriteTextFileAtomically(
                worldGraphTransactionTestSourcePath,
                worldGraphTransactionTestOriginalSource);
            try
            {
                MaterialParameterIncludeGenerator::GenerateInclude(
                    worldGraphTransactionTestSourcePath);
            }
            catch (...)
            {
            }
        }
        catch (...)
        {
        }
    }
    if (!worldGraphTransactionTestFixtureDirectory.empty())
    {
        std::error_code removeError;
        std::filesystem::remove_all(
            worldGraphTransactionTestFixtureDirectory,
            removeError);
    }
    if (!shaderUiReloadTestVertexPath.empty() &&
        !shaderUiReloadTestVertexOriginal.empty())
    {
        try
        {
            WriteTextFileAtomically(
                shaderUiReloadTestVertexPath,
                shaderUiReloadTestVertexOriginal);
        }
        catch (...)
        {
        }
    }
    if (!shaderUiReloadTestFragmentPath.empty() &&
        !shaderUiReloadTestFragmentOriginal.empty())
    {
        try
        {
            WriteTextFileAtomically(
                shaderUiReloadTestFragmentPath,
                shaderUiReloadTestFragmentOriginal);
        }
        catch (...)
        {
        }
    }
    if (!shaderUiReloadTestFixtureDirectory.empty())
    {
        std::error_code removeError;
        std::filesystem::remove_all(
            shaderUiReloadTestFixtureDirectory,
            removeError);
    }
    if (!shaderShutdownInflightSourcePath.empty() &&
        !shaderShutdownInflightOriginalSource.empty())
    {
        try
        {
            WriteTextFileAtomically(
                shaderShutdownInflightSourcePath,
                shaderShutdownInflightOriginalSource);
        }
        catch (...)
        {
        }
    }
}

void RuntimeTestHooks::Update(
    CommandBus& commandBus,
    const WorldManager& worldManager,
    const EnvironmentUpdateDiagnostics& environmentDiagnostics,
    RuntimeValidationServices& validationServices,
    const DiagnosticsSubsystem& diagnostics)
{
    if (shaderReloadTestActive &&
        shaderReloadTestPhase ==
            ShaderReloadTestPhase::WaitWorldLoad &&
        !waitingForShaderReloadTestWorld)
    {
        RuntimeCommand command;
        command.type = RuntimeCommandType::LoadWorld;
        command.stringValue = shaderReloadTestScenePath;
        command.sourceText =
            "runtime-test: shader-reload";
        commandBus.Queue(std::move(command));
        waitingForShaderReloadTestWorld = true;
        diagnostics.ReportInfo(
            "Shader reload runtime test queued its fixture World.");
        return;
    }

    if (shaderAutoReloadTestActive &&
        shaderAutoReloadTestPhase ==
            ShaderAutoReloadTestPhase::WaitWorldLoad &&
        !waitingForShaderAutoReloadTestWorld)
    {
        RuntimeCommand command;
        command.type = RuntimeCommandType::LoadWorld;
        command.stringValue = shaderAutoReloadTestScenePath;
        command.sourceText =
            "runtime-test: shader-auto-reload";
        commandBus.Queue(std::move(command));
        waitingForShaderAutoReloadTestWorld = true;
        diagnostics.ReportInfo(
            "Shader auto reload runtime test queued its fixture World.");
        return;
    }

    if (shaderComputeReloadTestActive &&
        shaderComputeReloadTestPhase ==
            ShaderComputeReloadTestPhase::WaitWorldLoad &&
        !waitingForShaderComputeReloadTestWorld)
    {
        RuntimeCommand command;
        command.type = RuntimeCommandType::LoadWorld;
        command.stringValue = shaderComputeReloadTestScenePath;
        command.sourceText =
            "runtime-test: shader-compute-reload";
        commandBus.Queue(std::move(command));
        waitingForShaderComputeReloadTestWorld = true;
        diagnostics.ReportInfo(
            "Shader compute reload runtime test queued its fixture World.");
        return;
    }

    if (shaderDefinitionReloadTestActive &&
        shaderDefinitionReloadTestPhase ==
            ShaderDefinitionReloadTestPhase::WaitWorldLoad &&
        !waitingForShaderDefinitionReloadTestWorld)
    {
        RuntimeCommand command;
        command.type = RuntimeCommandType::LoadWorld;
        command.stringValue = shaderDefinitionReloadTestScenePath;
        command.sourceText =
            "runtime-test: shader-definition-reload";
        commandBus.Queue(std::move(command));
        waitingForShaderDefinitionReloadTestWorld = true;
        diagnostics.ReportInfo(
            "Shader definition reload runtime test queued its fixture World.");
        return;
    }

    if (worldGraphTransactionTestActive &&
        worldGraphTransactionTestPhase ==
            WorldGraphTransactionTestPhase::WaitWorldLoad &&
        !waitingForWorldGraphTransactionTestWorld)
    {
        RuntimeCommand command;
        command.type = RuntimeCommandType::LoadWorld;
        command.stringValue =
            worldGraphTransactionTestScenePath;
        command.sourceText =
            "runtime-test: world-graph-transaction";
        commandBus.Queue(std::move(command));
        waitingForWorldGraphTransactionTestWorld = true;
        diagnostics.ReportInfo(
            "World/graph transaction runtime test queued its fixture World.");
        return;
    }

    if (shaderUiReloadTestActive &&
        shaderUiReloadTestPhase ==
            ShaderUiReloadTestPhase::WaitWorldLoad &&
        !waitingForShaderUiReloadTestWorld)
    {
        RuntimeCommand command;
        command.type = RuntimeCommandType::LoadWorld;
        command.stringValue = shaderUiReloadTestScenePath;
        command.sourceText =
            "runtime-test: shader-ui-reload";
        commandBus.Queue(std::move(command));
        waitingForShaderUiReloadTestWorld = true;
        diagnostics.ReportInfo(
            "Shader UI overlay reload runtime test queued its fixture World.");
        return;
    }

    if (environmentUpdateStressActive)
    {
        UpdateEnvironmentUpdateStress(
            commandBus,
            worldManager,
            environmentDiagnostics,
            validationServices,
            diagnostics);
        return;
    }

    if (worldReloadStressActive &&
        waitingForRetireDrain)
    {
        ResourceRetireQueue& retireQueue = ResourceRetireQueue::GetInstance();
        const size_t pendingRetiredResources = retireQueue.GetPendingCount();
        UpdateMaxPendingRetiredResources(pendingRetiredResources, maxPendingRetiredResources);

        if (pendingRetiredResources == 0)
        {
            if (totalWorldReloads > 1 && maxPendingRetiredResources == 0)
            {
                worldReloadStressActive = false;
                waitingForRetireDrain = false;
                runtimeTestStatus = RuntimeTestStatus::Failed;
                diagnostics.ReportError(
                    "World reload stress failed because no retired world-local resources were observed after repeated reloads.");
                CleanupGeneratedRuntimeFixtureIfNeeded(
                    cleanupGeneratedReloadStressFixture,
                    generatedReloadStressFixtureDirectory,
                    diagnostics);
                return;
            }

            worldReloadStressActive = false;
            waitingForRetireDrain = false;
            runtimeTestStatus = RuntimeTestStatus::Succeeded;
            diagnostics.ReportInfo(
                "World reload stress completed: " +
                std::to_string(completedWorldReloads) +
                "/" +
                std::to_string(totalWorldReloads) +
                " reloads succeeded, retire queue max pending=" +
                std::to_string(maxPendingRetiredResources) +
                ", completedEpoch=" +
                std::to_string(retireQueue.GetLastCompletedEpoch()) +
                ".");
            CleanupGeneratedRuntimeFixtureIfNeeded(
                cleanupGeneratedReloadStressFixture,
                generatedReloadStressFixtureDirectory,
                diagnostics);
            return;
        }

        --retireDrainFramesRemaining;
        if (retireDrainFramesRemaining <= 0)
        {
            worldReloadStressActive = false;
            waitingForRetireDrain = false;
            runtimeTestStatus = RuntimeTestStatus::Failed;
            diagnostics.ReportError(
                "World reload stress failed because retired world-local resources did not drain before the frame budget expired. pending=" +
                std::to_string(pendingRetiredResources) +
                ", submittedEpoch=" +
                std::to_string(retireQueue.GetLastSubmittedEpoch()) +
                ", completedEpoch=" +
                std::to_string(retireQueue.GetLastCompletedEpoch()) +
                ".");
            CleanupGeneratedRuntimeFixtureIfNeeded(
                cleanupGeneratedReloadStressFixture,
                generatedReloadStressFixtureDirectory,
                diagnostics);
        }
        return;
    }

    if (worldReloadStressActive &&
        !waitingForWorldReloadResult &&
        remainingWorldReloads > 0)
    {
        RuntimeCommand command;
        command.type = RuntimeCommandType::LoadWorld;
        command.stringValue = worldReloadStressScenePath;
        command.sourceText = "runtime-test: reloadstress";
        commandBus.Queue(std::move(command));

        waitingForWorldReloadResult = true;
        --remainingWorldReloads;

        diagnostics.ReportInfo(
            "World reload stress queued load " +
            std::to_string(completedWorldReloads + 1) +
            "/" +
            std::to_string(totalWorldReloads));
        return;
    }

    if (!failureRollbackTestActive || waitingForFailureRollbackResult)
    {
        return;
    }

    RuntimeCommand command;
    command.type = RuntimeCommandType::LoadWorld;
    command.stringValue = failureRollbackScenePath;
    command.sourceText = "runtime-test: reloadfail";
    commandBus.Queue(std::move(command));

    waitingForFailureRollbackResult = true;
    diagnostics.ReportInfo(
        "World reload failure rollback test queued expected-failure load: " +
        failureRollbackScenePath);
}

void RuntimeTestHooks::UpdateEngineLoopTests(
    RuntimeValidationServices& validationServices,
    const DiagnosticsSubsystem& diagnostics)
{
    if (shaderShutdownInflightTestActive)
    {
        UpdateShaderShutdownInflightTest(
            validationServices,
            diagnostics);
        return;
    }

    if (shaderReloadTestActive &&
        shaderReloadTestPhase !=
            ShaderReloadTestPhase::WaitWorldLoad)
    {
        UpdateShaderReloadTest(
            validationServices,
            diagnostics);
        return;
    }

    if (shaderAutoReloadTestActive &&
        shaderAutoReloadTestPhase !=
            ShaderAutoReloadTestPhase::WaitWorldLoad)
    {
        UpdateShaderAutoReloadTest(
            validationServices,
            diagnostics);
        return;
    }

    if (shaderComputeReloadTestActive &&
        shaderComputeReloadTestPhase !=
            ShaderComputeReloadTestPhase::WaitWorldLoad)
    {
        UpdateShaderComputeReloadTest(
            validationServices,
            diagnostics);
        return;
    }

    if (shaderDefinitionReloadTestActive &&
        shaderDefinitionReloadTestPhase !=
            ShaderDefinitionReloadTestPhase::WaitWorldLoad)
    {
        UpdateShaderDefinitionReloadTest(
            validationServices,
            diagnostics);
        return;
    }

    if (worldGraphTransactionTestActive &&
        worldGraphTransactionTestPhase !=
            WorldGraphTransactionTestPhase::WaitWorldLoad)
    {
        UpdateWorldGraphTransactionTest(
            validationServices,
            diagnostics);
        return;
    }

    if (shaderUiReloadTestActive &&
        shaderUiReloadTestPhase !=
            ShaderUiReloadTestPhase::WaitWorldLoad)
    {
        UpdateShaderUiReloadTest(
            validationServices,
            diagnostics);
        return;
    }

    if (resizeStressActive)
    {
        UpdateResizeStress(validationServices, diagnostics);
        return;
    }

    if (graphReloadStressActive)
    {
        UpdateRenderGraphReloadStress(validationServices, diagnostics);
    }
}

void RuntimeTestHooks::NotifyCommandResult(
    const RuntimeCommandExecutionResult& commandResult,
    const DiagnosticsSubsystem& diagnostics)
{
    if (shaderReloadTestActive &&
        waitingForShaderReloadTestWorld)
    {
        if (!commandResult.loadWorldAttempted)
        {
            return;
        }

        waitingForShaderReloadTestWorld = false;
        if (!commandResult.loadWorldSucceeded ||
            !commandResult.worldRuntimeBindingSucceeded)
        {
            FailShaderReloadTest(
                "Shader reload runtime test failed to load or bind "
                "its fixture World.",
                diagnostics);
            return;
        }

        shaderReloadTestPhase =
            ShaderReloadTestPhase::CompatibleCommit;
        diagnostics.ReportInfo(
            "Shader reload runtime test fixture World is active.");
        return;
    }

    if (shaderAutoReloadTestActive &&
        waitingForShaderAutoReloadTestWorld)
    {
        if (!commandResult.loadWorldAttempted)
        {
            return;
        }

        waitingForShaderAutoReloadTestWorld = false;
        if (!commandResult.loadWorldSucceeded ||
            !commandResult.worldRuntimeBindingSucceeded)
        {
            FailShaderAutoReloadTest(
                nullptr,
                "Shader auto reload runtime test failed to load or bind "
                "its fixture World.",
                diagnostics);
            return;
        }

        shaderAutoReloadTestPhase =
            ShaderAutoReloadTestPhase::WaitA1Gate;
        shaderAutoReloadTestPhaseEntryPending = true;
        diagnostics.ReportInfo(
            "Shader auto reload runtime test fixture World is active.");
        return;
    }

    if (shaderComputeReloadTestActive &&
        waitingForShaderComputeReloadTestWorld)
    {
        if (!commandResult.loadWorldAttempted)
        {
            return;
        }
        waitingForShaderComputeReloadTestWorld = false;
        if (!commandResult.loadWorldSucceeded ||
            !commandResult.worldRuntimeBindingSucceeded)
        {
            FailShaderComputeReloadTest(
                "Shader compute reload runtime test failed to load or bind "
                "its fixture World.",
                diagnostics);
            return;
        }
        shaderComputeReloadTestPhase =
            ShaderComputeReloadTestPhase::WaitCompatibleCommit;
        shaderComputeReloadTestPhaseEntryPending = true;
        diagnostics.ReportInfo(
            "Shader compute reload runtime test fixture World is active.");
        return;
    }

    if (shaderDefinitionReloadTestActive &&
        waitingForShaderDefinitionReloadTestWorld)
    {
        if (!commandResult.loadWorldAttempted)
        {
            return;
        }
        waitingForShaderDefinitionReloadTestWorld = false;
        if (!commandResult.loadWorldSucceeded ||
            !commandResult.worldRuntimeBindingSucceeded)
        {
            FailShaderDefinitionReloadTest(
                "Shader definition reload runtime test failed to load or bind "
                "its fixture World.",
                diagnostics);
            return;
        }
        shaderDefinitionReloadTestPhase =
            ShaderDefinitionReloadTestPhase::WaitAddedParameter;
        shaderDefinitionReloadTestPhaseEntryPending = true;
        diagnostics.ReportInfo(
            "Shader definition reload runtime test fixture World is active.");
        return;
    }

    if (worldGraphTransactionTestActive &&
        waitingForWorldGraphTransactionTestWorld)
    {
        if (!commandResult.loadWorldAttempted)
        {
            return;
        }
        waitingForWorldGraphTransactionTestWorld = false;
        if (!commandResult.loadWorldSucceeded ||
            !commandResult.worldRuntimeBindingSucceeded)
        {
            FailWorldGraphTransactionTest(
                nullptr,
                "World/graph transaction runtime test failed to load or "
                "bind its fixture World.",
                diagnostics);
            return;
        }
        worldGraphTransactionTestPhase =
            WorldGraphTransactionTestPhase::GraphResourceFailure;
        diagnostics.ReportInfo(
            "World/graph transaction runtime test fixture World is active.");
        return;
    }

    if (shaderUiReloadTestActive &&
        waitingForShaderUiReloadTestWorld)
    {
        if (!commandResult.loadWorldAttempted)
        {
            return;
        }
        waitingForShaderUiReloadTestWorld = false;
        if (!commandResult.loadWorldSucceeded ||
            !commandResult.worldRuntimeBindingSucceeded)
        {
            FailShaderUiReloadTest(
                "Shader UI overlay reload runtime test failed to load or "
                "bind its fixture World.",
                diagnostics);
            return;
        }
        shaderUiReloadTestPhase =
            ShaderUiReloadTestPhase::WaitCompatibleCommit;
        shaderUiReloadTestPhaseEntryPending = true;
        diagnostics.ReportInfo(
            "Shader UI overlay reload runtime test fixture World is active.");
        return;
    }

    if (!worldReloadStressActive || !waitingForWorldReloadResult)
    {
        if (!failureRollbackTestActive || !waitingForFailureRollbackResult)
        {
            return;
        }

        if (!commandResult.loadWorldAttempted)
        {
            return;
        }

        waitingForFailureRollbackResult = false;
        failureRollbackTestActive = false;
        const bool shouldCleanupGeneratedFixture = cleanupGeneratedFailureFixture;
        cleanupGeneratedFailureFixture = false;

        if (commandResult.loadWorldSucceeded)
        {
            runtimeTestStatus = RuntimeTestStatus::Failed;
            diagnostics.ReportError(
                "World reload failure rollback test expected load to fail, but it succeeded.");
            failureRollbackExpectedErrorCode.clear();
            if (shouldCleanupGeneratedFixture)
            {
                CleanupGeneratedRuntimeFixture(generatedFailureFixtureDirectory, diagnostics);
                generatedFailureFixtureDirectory.clear();
            }
            return;
        }

        if (!failureRollbackExpectedErrorCode.empty())
        {
            if (!commandResult.loadWorldError.has_value())
            {
                runtimeTestStatus = RuntimeTestStatus::Failed;
                diagnostics.ReportError(
                    "World reload failure rollback test failed because the expected load failure did not expose a RuntimeError.");
                failureRollbackExpectedErrorCode.clear();
                if (shouldCleanupGeneratedFixture)
                {
                    CleanupGeneratedRuntimeFixture(generatedFailureFixtureDirectory, diagnostics);
                    generatedFailureFixtureDirectory.clear();
                }
                return;
            }

            if (commandResult.loadWorldError->code != failureRollbackExpectedErrorCode)
            {
                runtimeTestStatus = RuntimeTestStatus::Failed;
                diagnostics.ReportError(
                    "World reload failure rollback test failed because the load error code was " +
                    commandResult.loadWorldError->code +
                    ", expected " +
                    failureRollbackExpectedErrorCode +
                    ".");
                failureRollbackExpectedErrorCode.clear();
                if (shouldCleanupGeneratedFixture)
                {
                    CleanupGeneratedRuntimeFixture(generatedFailureFixtureDirectory, diagnostics);
                    generatedFailureFixtureDirectory.clear();
                }
                return;
            }
        }

        if (commandResult.worldChanged || commandResult.worldRuntimeBindingAttempted)
        {
            runtimeTestStatus = RuntimeTestStatus::Failed;
            diagnostics.ReportError(
                "World reload failure rollback test failed because a failed load attempted to rebind runtime world state.");
            failureRollbackExpectedErrorCode.clear();
            if (shouldCleanupGeneratedFixture)
            {
                CleanupGeneratedRuntimeFixture(generatedFailureFixtureDirectory, diagnostics);
                generatedFailureFixtureDirectory.clear();
            }
            return;
        }

        if (!SameWorldHandle(
                commandResult.activeWorldBeforeCommand,
                commandResult.activeWorldAfterCommand))
        {
            runtimeTestStatus = RuntimeTestStatus::Failed;
            diagnostics.ReportError(
                "World reload failure rollback test failed because active world handle changed after the expected load failure.");
            failureRollbackExpectedErrorCode.clear();
            if (shouldCleanupGeneratedFixture)
            {
                CleanupGeneratedRuntimeFixture(generatedFailureFixtureDirectory, diagnostics);
                generatedFailureFixtureDirectory.clear();
            }
            return;
        }

        if (!SameRendererResourceFingerprint(
                commandResult.rendererResourcesBeforeLoad,
                commandResult.rendererResourcesAfterLoad))
        {
            runtimeTestStatus = RuntimeTestStatus::Failed;
            diagnostics.ReportError(
                "World reload failure rollback test failed because renderer resource cache or pass material bindings changed after the expected load failure. before={" +
                FormatRendererResourceFingerprint(commandResult.rendererResourcesBeforeLoad) +
                "} after={" +
                FormatRendererResourceFingerprint(commandResult.rendererResourcesAfterLoad) +
                "}");
            failureRollbackExpectedErrorCode.clear();
            if (shouldCleanupGeneratedFixture)
            {
                CleanupGeneratedRuntimeFixture(generatedFailureFixtureDirectory, diagnostics);
                generatedFailureFixtureDirectory.clear();
            }
            return;
        }

        runtimeTestStatus = RuntimeTestStatus::Succeeded;
        diagnostics.ReportInfo(
            "World reload failure rollback test completed: active world, renderer resource cache, and pass material bindings preserved after expected load failure.");
        failureRollbackExpectedErrorCode.clear();
        if (shouldCleanupGeneratedFixture)
        {
            CleanupGeneratedRuntimeFixture(generatedFailureFixtureDirectory, diagnostics);
            generatedFailureFixtureDirectory.clear();
        }
        return;
    }

    if (!commandResult.loadWorldAttempted)
    {
        return;
    }

    waitingForWorldReloadResult = false;

    if (!commandResult.loadWorldSucceeded)
    {
        worldReloadStressActive = false;
        runtimeTestStatus = RuntimeTestStatus::Failed;
        diagnostics.ReportError(
            "World reload stress aborted after " +
            std::to_string(completedWorldReloads) +
            "/" +
            std::to_string(totalWorldReloads) +
            " successful reloads.");
        CleanupGeneratedRuntimeFixtureIfNeeded(
            cleanupGeneratedReloadStressFixture,
            generatedReloadStressFixtureDirectory,
            diagnostics);
        return;
    }

    if (!commandResult.worldRuntimeBindingSucceeded)
    {
        worldReloadStressActive = false;
        runtimeTestStatus = RuntimeTestStatus::Failed;
        diagnostics.ReportError(
            "World reload stress aborted because runtime rebinding failed after load " +
            std::to_string(completedWorldReloads + 1) +
            "/" +
            std::to_string(totalWorldReloads) +
            ".");
        CleanupGeneratedRuntimeFixtureIfNeeded(
            cleanupGeneratedReloadStressFixture,
            generatedReloadStressFixtureDirectory,
            diagnostics);
        return;
    }

    const size_t pendingRetiredResources = ResourceRetireQueue::GetInstance().GetPendingCount();
    UpdateMaxPendingRetiredResources(pendingRetiredResources, maxPendingRetiredResources);

    ++completedWorldReloads;
    if (remainingWorldReloads <= 0)
    {
        waitingForRetireDrain = true;
        retireDrainFramesRemaining = RetireDrainFrameBudget;
        diagnostics.ReportInfo(
            "World reload stress waiting for retire queue drain: pending=" +
            std::to_string(pendingRetiredResources) +
            ", maxPending=" +
            std::to_string(maxPendingRetiredResources) +
            ".");
    }
}

} // namespace VL
