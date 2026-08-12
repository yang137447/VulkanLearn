#include "engine/runtimeCommandExecutor.h"

#include <exception>
#include <memory>
#include <string>
#include <unordered_map>
#include <utility>

#include "engine/diagnosticsSubsystem.h"
#include "engine/runtimeConfig.h"
#include "engine/runtimeTestHooks.h"
#include "materialInstance.h"
#include "render/resource/rendererResourceCache.h"
#include "renderGraph.h"
#include "renderSystem.h"
#include "world/loading/worldTransitionCoordinator.h"
#include "world/world.h"

namespace VL
{

namespace
{

const char* ToString(BloomParameter parameter)
{
    switch (parameter)
    {
    case BloomParameter::Strength:
        return "strength";
    case BloomParameter::Threshold:
        return "threshold";
    case BloomParameter::Knee:
        return "knee";
    case BloomParameter::Clamp:
        return "clamp";
    }

    return "unknown";
}

template <typename T>
std::unordered_map<std::string, std::uintptr_t> CaptureSharedResourcePointers(
    const std::unordered_map<std::string, std::shared_ptr<T>>& resources)
{
    std::unordered_map<std::string, std::uintptr_t> pointers;
    pointers.reserve(resources.size());
    for (const auto& [key, resource] : resources)
    {
        pointers.emplace(key, reinterpret_cast<std::uintptr_t>(resource.get()));
    }
    return pointers;
}

RuntimeRendererResourceFingerprint CaptureRuntimeRendererResourceFingerprint()
{
    RendererResourceCache::WorldLocalResourceSnapshot resourceSnapshot =
        RendererResourceCache::GetInstance().CaptureWorldLocalResources();
    const auto passMaterialSnapshot = RenderGraph::GetInstance().CapturePassMaterialInstances();

    RuntimeRendererResourceFingerprint fingerprint;
    fingerprint.captured = true;
    fingerprint.worldOwnerGeneration = resourceSnapshot.ownerGeneration;
    fingerprint.worldTextures = CaptureSharedResourcePointers(resourceSnapshot.worldTextures);
    fingerprint.renderableObjects = CaptureSharedResourcePointers(resourceSnapshot.renderableObjects);
    fingerprint.materials = CaptureSharedResourcePointers(resourceSnapshot.materials);
    fingerprint.materialInstances = CaptureSharedResourcePointers(resourceSnapshot.materialInstances);
    fingerprint.objectResources = CaptureSharedResourcePointers(resourceSnapshot.objectResources);
    fingerprint.textures = CaptureSharedResourcePointers(resourceSnapshot.textures);
    fingerprint.passMaterialInstances = CaptureSharedResourcePointers(passMaterialSnapshot);
    return fingerprint;
}

} // namespace

RuntimeCommandExecutionResult RuntimeCommandExecutor::ExecuteQueuedCommands(
    CommandBus& commandBus,
    RenderSystem& renderSystem,
    WorldManager& worldManager,
    WorldTransitionCoordinator& worldTransitionCoordinator,
    RuntimeTestHooks& runtimeTestHooks,
    const RuntimeConfig& runtimeConfig,
    const DiagnosticsSubsystem& diagnostics) const
{
    RuntimeCommandExecutionResult executionResult;
    std::vector<RuntimeCommand> commands = commandBus.Drain();
    for (const RuntimeCommand& command : commands)
    {
        ExecuteCommand(
            command,
            renderSystem,
            worldManager,
            worldTransitionCoordinator,
            runtimeTestHooks,
            runtimeConfig,
            diagnostics,
            executionResult);
    }
    return executionResult;
}

void RuntimeCommandExecutor::ExecuteCommand(
    const RuntimeCommand& command,
    RenderSystem& renderSystem,
    WorldManager& worldManager,
    WorldTransitionCoordinator& worldTransitionCoordinator,
    RuntimeTestHooks& runtimeTestHooks,
    const RuntimeConfig& runtimeConfig,
    const DiagnosticsSubsystem& diagnostics,
    RuntimeCommandExecutionResult& executionResult) const
{
    switch (command.type)
    {
    case RuntimeCommandType::LoadWorld:
        ApplyLoadWorld(command, worldTransitionCoordinator, runtimeConfig, diagnostics, executionResult);
        break;
    case RuntimeCommandType::RunWorldReloadStress:
        ApplyWorldReloadStress(command, runtimeTestHooks, diagnostics);
        break;
    case RuntimeCommandType::RunWorldReloadFailureRollbackTest:
        ApplyWorldReloadFailureRollbackTest(command, runtimeTestHooks, diagnostics);
        break;
    case RuntimeCommandType::RunGeneratedMaterialFailureRollbackTest:
        ApplyGeneratedMaterialFailureRollbackTest(runtimeConfig, runtimeTestHooks, diagnostics);
        break;
    case RuntimeCommandType::RunGeneratedMeshFailureRollbackTest:
        ApplyGeneratedMeshFailureRollbackTest(runtimeConfig, runtimeTestHooks, diagnostics);
        break;
    case RuntimeCommandType::RunGeneratedTextureFailureRollbackTest:
        ApplyGeneratedTextureFailureRollbackTest(runtimeConfig, runtimeTestHooks, diagnostics);
        break;
    case RuntimeCommandType::RunGeneratedHighLightReloadStress:
        ApplyGeneratedHighLightReloadStress(command, runtimeConfig, runtimeTestHooks, diagnostics);
        break;
    case RuntimeCommandType::RunResizeStress:
        ApplyResizeStress(command, runtimeTestHooks, diagnostics);
        break;
    case RuntimeCommandType::RunRenderGraphReloadStress:
        ApplyRenderGraphReloadStress(command, runtimeTestHooks, diagnostics);
        break;
    case RuntimeCommandType::RunFrameSmokeTest:
        ApplyFrameSmokeTest(command, runtimeTestHooks, diagnostics);
        break;
    case RuntimeCommandType::RunEnvironmentUpdateStress:
        ApplyEnvironmentUpdateStress(command, runtimeTestHooks, diagnostics);
        break;
    case RuntimeCommandType::SetDebugViewMode:
        renderSystem.SetDebugViewMode(command.intValue);
        diagnostics.ReportInfo("Debug view mode set to " + std::to_string(command.intValue));
        break;
    case RuntimeCommandType::SetEnvironmentIntensity:
        renderSystem.SetEnvironmentIntensity(command.floatValue);
        diagnostics.ReportInfo("Environment intensity set to " + std::to_string(command.floatValue));
        break;
    case RuntimeCommandType::SetProceduralSkyParameters:
        ApplyProceduralSkyParameters(
            command,
            worldManager,
            runtimeTestHooks,
            diagnostics);
        break;
    case RuntimeCommandType::SetSpeedTreeStrength:
        if (renderSystem.SetSpeedTreeStrength(command.floatValue))
        {
            diagnostics.ReportInfo(
                "SpeedTree strength target set to " + std::to_string(command.floatValue) +
                " (range 0..1; response time applies).");
        }
        else
        {
            diagnostics.ReportWarning("SpeedTree strength ignored because no wind-enabled tree is active.");
        }
        break;
    case RuntimeCommandType::SetSpeedTreeGustingEnabled:
        if (renderSystem.SetSpeedTreeGustingEnabled(command.intValue != 0))
        {
            diagnostics.ReportInfo(
                std::string("SpeedTree gusting ") +
                (command.intValue != 0 ? "enabled." : "disabled."));
        }
        else
        {
            diagnostics.ReportWarning("SpeedTree gust toggle ignored because no wind-enabled tree is active.");
        }
        break;
    case RuntimeCommandType::ForceSpeedTreeGust:
        if (renderSystem.ForceSpeedTreeGust())
        {
            diagnostics.ReportInfo("SpeedTree gust forced.");
        }
        else
        {
            diagnostics.ReportWarning("SpeedTree gust ignored because no wind-enabled tree is active or gusting is disabled.");
        }
        break;
    case RuntimeCommandType::SetToneMappingMode:
        ApplyToneMappingMode(command.intValue, renderSystem, diagnostics);
        break;
    case RuntimeCommandType::SetBloomParameter:
        ApplyBloomParameter(command.bloomParameter, command.floatValue, renderSystem, diagnostics);
        break;
    }
}

void RuntimeCommandExecutor::ApplyLoadWorld(
    const RuntimeCommand& command,
    WorldTransitionCoordinator& worldTransitionCoordinator,
    const RuntimeConfig& runtimeConfig,
    const DiagnosticsSubsystem& diagnostics,
    RuntimeCommandExecutionResult& executionResult) const
{
    executionResult.loadWorldAttempted = true;
    executionResult.loadWorldSucceeded = false;
    executionResult.loadWorldCommandPath = command.stringValue;
    executionResult.loadWorldResolvedPath.clear();
    executionResult.rendererResourcesBeforeLoad = CaptureRuntimeRendererResourceFingerprint();

    if (command.stringValue.empty())
    {
        diagnostics.ReportWarning("LoadWorld command ignored because scene path is empty.");
        executionResult.rendererResourcesAfterLoad = CaptureRuntimeRendererResourceFingerprint();
        return;
    }

    std::string resolvedScenePath;
    try
    {
        resolvedScenePath = runtimeConfig.ResolvePath(command.stringValue);
    }
    catch (const std::exception& exception)
    {
        RuntimeError error = MakeRuntimeError(
            "Scene.ResolvePathFailed",
            exception.what(),
            command.stringValue);
        diagnostics.ReportRuntimeError("LoadWorld path resolve failed", error);
        executionResult.loadWorldError = std::move(error);
        executionResult.rendererResourcesAfterLoad = CaptureRuntimeRendererResourceFingerprint();
        return;
    }
    executionResult.loadWorldResolvedPath = resolvedScenePath;

    auto worldLoadResult = worldTransitionCoordinator.RequestWorldLoad(resolvedScenePath);
    if (worldLoadResult.IsFailure())
    {
        diagnostics.ReportRuntimeError("LoadWorld failed", worldLoadResult.Error());
        executionResult.loadWorldError = worldLoadResult.Error();
        executionResult.rendererResourcesAfterLoad = CaptureRuntimeRendererResourceFingerprint();
        return;
    }

    executionResult.worldChanged = true;
    executionResult.loadWorldSucceeded = true;
    executionResult.loadedWorld = worldLoadResult.Value().world;
    diagnostics.ReportInfo("World loaded: " + resolvedScenePath);
    executionResult.rendererResourcesAfterLoad = CaptureRuntimeRendererResourceFingerprint();
}

void RuntimeCommandExecutor::ApplyWorldReloadStress(
    const RuntimeCommand& command,
    RuntimeTestHooks& runtimeTestHooks,
    const DiagnosticsSubsystem& diagnostics) const
{
    (void)runtimeTestHooks.BeginWorldReloadStress(
        command.stringValue,
        command.intValue,
        diagnostics);
}

void RuntimeCommandExecutor::ApplyWorldReloadFailureRollbackTest(
    const RuntimeCommand& command,
    RuntimeTestHooks& runtimeTestHooks,
    const DiagnosticsSubsystem& diagnostics) const
{
    (void)runtimeTestHooks.BeginWorldReloadFailureRollbackTest(
        command.stringValue,
        diagnostics);
}

void RuntimeCommandExecutor::ApplyGeneratedMaterialFailureRollbackTest(
    const RuntimeConfig& runtimeConfig,
    RuntimeTestHooks& runtimeTestHooks,
    const DiagnosticsSubsystem& diagnostics) const
{
    (void)runtimeTestHooks.BeginGeneratedMaterialFailureRollbackTest(
        runtimeConfig.GetResourcePath(),
        diagnostics);
}

void RuntimeCommandExecutor::ApplyGeneratedMeshFailureRollbackTest(
    const RuntimeConfig& runtimeConfig,
    RuntimeTestHooks& runtimeTestHooks,
    const DiagnosticsSubsystem& diagnostics) const
{
    (void)runtimeTestHooks.BeginGeneratedMeshFailureRollbackTest(
        runtimeConfig.GetResourcePath(),
        diagnostics);
}

void RuntimeCommandExecutor::ApplyGeneratedTextureFailureRollbackTest(
    const RuntimeConfig& runtimeConfig,
    RuntimeTestHooks& runtimeTestHooks,
    const DiagnosticsSubsystem& diagnostics) const
{
    (void)runtimeTestHooks.BeginGeneratedTextureFailureRollbackTest(
        runtimeConfig.GetResourcePath(),
        diagnostics);
}

void RuntimeCommandExecutor::ApplyGeneratedHighLightReloadStress(
    const RuntimeCommand& command,
    const RuntimeConfig& runtimeConfig,
    RuntimeTestHooks& runtimeTestHooks,
    const DiagnosticsSubsystem& diagnostics) const
{
    (void)runtimeTestHooks.BeginGeneratedHighLightReloadStress(
        runtimeConfig.GetResourcePath(),
        command.intValue,
        diagnostics);
}

void RuntimeCommandExecutor::ApplyResizeStress(
    const RuntimeCommand& command,
    RuntimeTestHooks& runtimeTestHooks,
    const DiagnosticsSubsystem& diagnostics) const
{
    (void)runtimeTestHooks.BeginResizeStress(command.intValue, diagnostics);
}

void RuntimeCommandExecutor::ApplyRenderGraphReloadStress(
    const RuntimeCommand& command,
    RuntimeTestHooks& runtimeTestHooks,
    const DiagnosticsSubsystem& diagnostics) const
{
    (void)runtimeTestHooks.BeginRenderGraphReloadStress(command.intValue, diagnostics);
}

void RuntimeCommandExecutor::ApplyFrameSmokeTest(
    const RuntimeCommand& command,
    RuntimeTestHooks& runtimeTestHooks,
    const DiagnosticsSubsystem& diagnostics) const
{
    (void)runtimeTestHooks.BeginFrameSmokeTest(command.intValue, diagnostics);
}

void RuntimeCommandExecutor::ApplyEnvironmentUpdateStress(
    const RuntimeCommand& command,
    RuntimeTestHooks& runtimeTestHooks,
    const DiagnosticsSubsystem& diagnostics) const
{
    (void)runtimeTestHooks.BeginEnvironmentUpdateStress(
        command.intValue,
        diagnostics);
}

void RuntimeCommandExecutor::ApplyProceduralSkyParameters(
    const RuntimeCommand& command,
    WorldManager& worldManager,
    RuntimeTestHooks& runtimeTestHooks,
    const DiagnosticsSubsystem& diagnostics) const
{
    const std::shared_ptr<World>& activeWorld = worldManager.GetActiveWorld();
    if (!activeWorld)
    {
        diagnostics.ReportError(
            "Procedural sky parameters update rejected because no active World exists.");
        runtimeTestHooks.NotifyProceduralSkyParametersResult(false, diagnostics);
        return;
    }

    const WorldEnvironment& currentEnvironment = activeWorld->GetEnvironment();
    if (currentEnvironment.type != EnvironmentType::ProceduralSky)
    {
        diagnostics.ReportError(
            "Procedural sky parameters update rejected because the active World uses HDRI.");
        runtimeTestHooks.NotifyProceduralSkyParametersResult(false, diagnostics);
        return;
    }

    WorldEnvironment updatedEnvironment = currentEnvironment;
    updatedEnvironment.skyParameters = command.skyParametersValue;
    activeWorld->SetEnvironment(std::move(updatedEnvironment));
    runtimeTestHooks.NotifyProceduralSkyParametersResult(true, diagnostics);
}

void RuntimeCommandExecutor::ApplyToneMappingMode(
    int mode,
    RenderSystem& renderSystem,
    const DiagnosticsSubsystem& diagnostics) const
{
    std::string message;
    if (!renderSystem.SetToneMappingMode(mode, message))
    {
        diagnostics.ReportWarning(message);
        return;
    }

    diagnostics.ReportInfo(message);
}

void RuntimeCommandExecutor::ApplyBloomParameter(
    BloomParameter parameter,
    float value,
    RenderSystem& renderSystem,
    const DiagnosticsSubsystem& diagnostics) const
{
    std::string message;
    if (parameter == BloomParameter::Strength)
    {
        if (!renderSystem.SetBloomStrength(value, message))
        {
            diagnostics.ReportWarning(message);
            return;
        }

        diagnostics.ReportInfo(message);
        return;
    }

    if (parameter == BloomParameter::Threshold)
    {
        if (!renderSystem.SetBloomThreshold(value, message))
        {
            diagnostics.ReportWarning(message);
            return;
        }
    }
    else if (parameter == BloomParameter::Knee)
    {
        if (!renderSystem.SetBloomKnee(value, message))
        {
            diagnostics.ReportWarning(message);
            return;
        }
    }
    else if (parameter == BloomParameter::Clamp)
    {
        if (!renderSystem.SetBloomClamp(value, message))
        {
            diagnostics.ReportWarning(message);
            return;
        }
    }
    else
    {
        diagnostics.ReportWarning(std::string("Unknown bloom parameter: ") + ToString(parameter));
        return;
    }

    diagnostics.ReportInfo(message);
}

} // namespace VL
