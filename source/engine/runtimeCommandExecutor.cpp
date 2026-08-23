#include "engine/runtimeCommandExecutor.h"

#include <cmath>
#include <exception>
#include <fstream>
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
#include "shader/build/atomicFile.h"
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

double RoundSceneFloat(float value)
{
    return std::round(
        static_cast<double>(value) * 10000.0) /
        10000.0;
}

nlohmann::ordered_json BuildDirectionalLightShadowJson(
    const CsmSettings& settings)
{
    nlohmann::ordered_json shadowJson =
        nlohmann::ordered_json::object();
    shadowJson["castShadows"] = settings.castShadows;
    shadowJson["dynamicShadowDistance"] =
        RoundSceneFloat(settings.dynamicShadowDistance);
    shadowJson["dynamicShadowCascades"] =
        settings.cascadeCount;
    shadowJson["cascadeDistributionExponent"] =
        RoundSceneFloat(settings.cascadeDistributionExponent);
    shadowJson["cascadeTransitionFraction"] =
        RoundSceneFloat(settings.cascadeTransitionFraction);
    shadowJson["shadowDistanceFadeoutFraction"] =
        RoundSceneFloat(settings.shadowDistanceFadeoutFraction);
    shadowJson["shadowBias"] =
        RoundSceneFloat(settings.shadowBias);
    shadowJson["shadowSlopeBias"] =
        RoundSceneFloat(settings.shadowSlopeBias);
    shadowJson["shadowCascadeBiasDistribution"] =
        RoundSceneFloat(
            settings.shadowCascadeBiasDistribution);
    return shadowJson;
}

nlohmann::ordered_json* FindPrimaryDirectionalLight(
    nlohmann::ordered_json& sceneJson)
{
    if (!sceneJson.contains("objects") ||
        !sceneJson["objects"].is_array())
    {
        return nullptr;
    }

    for (nlohmann::ordered_json& objectJson :
         sceneJson["objects"])
    {
        if (objectJson.is_object() &&
            objectJson.value("type", std::string()) ==
                "directionalLight")
        {
            return &objectJson;
        }
    }
    return nullptr;
}

} // namespace

RuntimeRendererResourceFingerprint
CaptureRuntimeRendererResourceFingerprint()
{
    const RendererResourceCache::ImmutableWorldLocalResourceRefs
        resourceSnapshot =
            RendererResourceCache::GetInstance()
                .CaptureActiveWorldLocalResources();
    const auto passMaterialSnapshot =
        RenderGraph::GetInstance()
            .CapturePassMaterialInstances();

    RuntimeRendererResourceFingerprint fingerprint;
    fingerprint.captured = true;
    if (resourceSnapshot)
    {
        fingerprint.worldOwnerGeneration =
            resourceSnapshot->ownerGeneration;
        fingerprint.worldTextures =
            CaptureSharedResourcePointers(
                resourceSnapshot->worldTextures);
        fingerprint.renderableObjects =
            CaptureSharedResourcePointers(
                resourceSnapshot->renderableObjects);
        fingerprint.materials =
            CaptureSharedResourcePointers(
                resourceSnapshot->materials);
        fingerprint.materialInstances =
            CaptureSharedResourcePointers(
                resourceSnapshot->materialInstances);
        fingerprint.objectResources =
            CaptureSharedResourcePointers(
                resourceSnapshot->objectResources);
        fingerprint.textures =
            CaptureSharedResourcePointers(
                resourceSnapshot->textures);
    }
    fingerprint.passMaterialInstances =
        CaptureSharedResourcePointers(
            passMaterialSnapshot);
    return fingerprint;
}

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
    case RuntimeCommandType::RunShaderReloadTest:
        ApplyShaderReloadTest(runtimeConfig, runtimeTestHooks, diagnostics);
        break;
    case RuntimeCommandType::RunShaderAutoReloadTest:
        ApplyShaderAutoReloadTest(runtimeConfig, runtimeTestHooks, diagnostics);
        break;
    case RuntimeCommandType::RunShaderComputeReloadTest:
        ApplyShaderComputeReloadTest(runtimeConfig, runtimeTestHooks, diagnostics);
        break;
    case RuntimeCommandType::RunShaderDefinitionReloadTest:
        ApplyShaderDefinitionReloadTest(runtimeConfig, runtimeTestHooks, diagnostics);
        break;
    case RuntimeCommandType::RunWorldGraphTransactionTest:
        ApplyWorldGraphTransactionTest(
            runtimeConfig,
            runtimeTestHooks,
            diagnostics);
        break;
    case RuntimeCommandType::RunShaderUiReloadTest:
        ApplyShaderUiReloadTest(runtimeConfig, runtimeTestHooks, diagnostics);
        break;
    case RuntimeCommandType::RunShaderShutdownInflightTest:
        ApplyShaderShutdownInflightTest(
            runtimeTestHooks,
            diagnostics);
        break;
    case RuntimeCommandType::RunHairValidationTest:
        ApplyHairValidationTest(
            runtimeConfig,
            runtimeTestHooks,
            diagnostics);
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
    case RuntimeCommandType::SetCsmCastShadows:
    {
        std::string message;
        renderSystem.SetCsmCastShadows(
            command.intValue != 0,
            message);
        diagnostics.ReportInfo(message);
        break;
    }
    case RuntimeCommandType::SetCsmDynamicShadowDistance:
    {
        std::string message;
        if (renderSystem.SetCsmDynamicShadowDistance(
                command.floatValue,
                message))
        {
            diagnostics.ReportInfo(message);
        }
        else
        {
            diagnostics.ReportWarning(message);
        }
        break;
    }
    case RuntimeCommandType::SetCsmDynamicShadowCascades:
    {
        std::string message;
        if (renderSystem.SetCsmCascadeCount(
                static_cast<uint32_t>(command.intValue),
                message))
        {
            diagnostics.ReportInfo(message);
        }
        else
        {
            diagnostics.ReportWarning(message);
        }
        break;
    }
    case RuntimeCommandType::SetCsmCascadeDistributionExponent:
    {
        std::string message;
        if (renderSystem.SetCsmCascadeDistributionExponent(
                command.floatValue,
                message))
        {
            diagnostics.ReportInfo(message);
        }
        else
        {
            diagnostics.ReportWarning(message);
        }
        break;
    }
    case RuntimeCommandType::SetCsmCascadeTransitionFraction:
    {
        std::string message;
        if (renderSystem.SetCsmCascadeTransitionFraction(
                command.floatValue,
                message))
        {
            diagnostics.ReportInfo(message);
        }
        else
        {
            diagnostics.ReportWarning(message);
        }
        break;
    }
    case RuntimeCommandType::SetCsmShadowDistanceFadeoutFraction:
    {
        std::string message;
        if (renderSystem.SetCsmShadowDistanceFadeoutFraction(
                command.floatValue,
                message))
        {
            diagnostics.ReportInfo(message);
        }
        else
        {
            diagnostics.ReportWarning(message);
        }
        break;
    }
    case RuntimeCommandType::SetCsmShadowBias:
    {
        std::string message;
        if (renderSystem.SetCsmShadowBias(
                command.floatValue,
                message))
        {
            diagnostics.ReportInfo(message);
        }
        else
        {
            diagnostics.ReportWarning(message);
        }
        break;
    }
    case RuntimeCommandType::SetCsmShadowSlopeBias:
    {
        std::string message;
        if (renderSystem.SetCsmShadowSlopeBias(
                command.floatValue,
                message))
        {
            diagnostics.ReportInfo(message);
        }
        else
        {
            diagnostics.ReportWarning(message);
        }
        break;
    }
    case RuntimeCommandType::SetCsmShadowCascadeBiasDistribution:
    {
        std::string message;
        if (renderSystem.SetCsmShadowCascadeBiasDistribution(
                command.floatValue,
                message))
        {
            diagnostics.ReportInfo(message);
        }
        else
        {
            diagnostics.ReportWarning(message);
        }
        break;
    }
    case RuntimeCommandType::SaveCsmSettingsToScene:
        ApplySaveCsmSettingsToScene(
            renderSystem,
            worldManager,
            diagnostics);
        break;
    case RuntimeCommandType::ReloadShaders:
        executionResult.shaderReloadRequested = true;
        executionResult.shaderReloadScope =
            command.shaderReloadScope;
        break;
    case RuntimeCommandType::ReportShaderCacheStatistics:
        executionResult.shaderCacheStatisticsRequested = true;
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
    executionResult.worldLoadRequested = true;
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

void RuntimeCommandExecutor::ApplyShaderReloadTest(
    const RuntimeConfig& runtimeConfig,
    RuntimeTestHooks& runtimeTestHooks,
    const DiagnosticsSubsystem& diagnostics) const
{
    (void)runtimeTestHooks.BeginShaderReloadTest(
        runtimeConfig.GetResourcePath(),
        diagnostics);
}

void RuntimeCommandExecutor::ApplyShaderAutoReloadTest(
    const RuntimeConfig& runtimeConfig,
    RuntimeTestHooks& runtimeTestHooks,
    const DiagnosticsSubsystem& diagnostics) const
{
    (void)runtimeTestHooks.BeginShaderAutoReloadTest(
        runtimeConfig.GetResourcePath(),
        diagnostics);
}

void RuntimeCommandExecutor::ApplyShaderComputeReloadTest(
    const RuntimeConfig& runtimeConfig,
    RuntimeTestHooks& runtimeTestHooks,
    const DiagnosticsSubsystem& diagnostics) const
{
    (void)runtimeTestHooks.BeginShaderComputeReloadTest(
        runtimeConfig.GetResourcePath(),
        diagnostics);
}

void RuntimeCommandExecutor::ApplyShaderDefinitionReloadTest(
    const RuntimeConfig& runtimeConfig,
    RuntimeTestHooks& runtimeTestHooks,
    const DiagnosticsSubsystem& diagnostics) const
{
    (void)runtimeTestHooks.BeginShaderDefinitionReloadTest(
        runtimeConfig.GetResourcePath(),
        diagnostics);
}

void RuntimeCommandExecutor::ApplyWorldGraphTransactionTest(
    const RuntimeConfig& runtimeConfig,
    RuntimeTestHooks& runtimeTestHooks,
    const DiagnosticsSubsystem& diagnostics) const
{
    (void)runtimeTestHooks.BeginWorldGraphTransactionTest(
        runtimeConfig.GetResourcePath(),
        diagnostics);
}

void RuntimeCommandExecutor::ApplyShaderUiReloadTest(
    const RuntimeConfig& runtimeConfig,
    RuntimeTestHooks& runtimeTestHooks,
    const DiagnosticsSubsystem& diagnostics) const
{
    (void)runtimeTestHooks.BeginShaderUiReloadTest(
        runtimeConfig.GetResourcePath(),
        diagnostics);
}

void RuntimeCommandExecutor::ApplyShaderShutdownInflightTest(
    RuntimeTestHooks& runtimeTestHooks,
    const DiagnosticsSubsystem& diagnostics) const
{
    (void)runtimeTestHooks.BeginShaderShutdownInflightTest(
        diagnostics);
}

void RuntimeCommandExecutor::ApplyHairValidationTest(
    const RuntimeConfig& runtimeConfig,
    RuntimeTestHooks& runtimeTestHooks,
    const DiagnosticsSubsystem& diagnostics) const
{
    (void)runtimeTestHooks.BeginHairValidationTest(
        runtimeConfig.GetResourcePath(),
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

void RuntimeCommandExecutor::ApplySaveCsmSettingsToScene(
    RenderSystem& renderSystem,
    WorldManager& worldManager,
    const DiagnosticsSubsystem& diagnostics) const
{
    const std::shared_ptr<World>& activeWorld =
        worldManager.GetActiveWorld();
    if (!activeWorld)
    {
        diagnostics.ReportError(
            "CSM settings cannot be saved because no active World exists.");
        return;
    }

    const std::string& scenePath =
        activeWorld->GetScenePath();
    try
    {
        nlohmann::ordered_json sceneJson;
        {
            // Windows 无法原子替换仍被当前进程的 ifstream 占用的文件。
            // 读取作用域必须在临时文件落盘和替换目标文件之前结束。
            std::ifstream sceneFile(scenePath);
            if (!sceneFile.is_open())
            {
                diagnostics.ReportError(
                    "CSM settings cannot be saved because the active scene file is unavailable: " +
                    scenePath);
                return;
            }
            sceneJson =
                nlohmann::ordered_json::parse(sceneFile);
        }

        if (!sceneJson.is_object())
        {
            diagnostics.ReportError(
                "CSM settings cannot be saved because the active scene root is not an object: " +
                scenePath);
            return;
        }

        nlohmann::ordered_json* directionalLightJson =
            FindPrimaryDirectionalLight(sceneJson);
        if (directionalLightJson == nullptr)
        {
            diagnostics.ReportError(
                "CSM settings cannot be saved because the active scene has no directionalLight: " +
                scenePath);
            return;
        }

        const CsmSettings& settings =
            renderSystem.GetCsmSettings();
        (*directionalLightJson)["shadow"] =
            BuildDirectionalLightShadowJson(settings);
        WriteTextFileAtomically(
            scenePath,
            sceneJson.dump(4) + "\n");

        // World 记录最后一次持久化的场景值；
        // 尚未保存的运行时调节仍由 RenderSystem 持有。
        activeWorld->SetCsmSettings(settings);
        diagnostics.ReportInfo(
            "CSM settings saved to the active scene's primary directional light: " +
            scenePath);
    }
    catch (const std::exception& exception)
    {
        diagnostics.ReportError(
            "Failed to save CSM settings to active scene: " +
            scenePath +
            " error=" +
            exception.what());
    }
}

} // namespace VL
