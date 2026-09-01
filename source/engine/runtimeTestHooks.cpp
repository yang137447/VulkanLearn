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

    (void)worldManager;
    (void)environmentDiagnostics;
}


void RuntimeTestHooks::UpdateEngineLoopTests(
    RuntimeValidationServices& validationServices,
    const DiagnosticsSubsystem& diagnostics)
{
    if (shaderReloadTestActive &&
        shaderReloadTestPhase !=
            ShaderReloadTestPhase::WaitWorldLoad)
    {
        UpdateShaderReloadTest(
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

    if (worldGraphTransactionTestActive &&
        worldGraphTransactionTestPhase !=
            WorldGraphTransactionTestPhase::WaitWorldLoad)
    {
        UpdateWorldGraphTransactionTest(
            validationServices,
            diagnostics);
        return;
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

}

} // namespace VL
