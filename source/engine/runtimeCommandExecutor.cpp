#include "engine/runtimeCommandExecutor.h"

#include <cmath>
#include <filesystem>
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
#include "sceneNode.h"
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

std::string FormatVector3(const Eigen::Vector3f& value)
{
    return "(" + std::to_string(value.x()) + ", " +
        std::to_string(value.y()) + ", " +
        std::to_string(value.z()) + ")";
}

std::string ResolveScreenshotPath(
    const RuntimeCommand& command,
    const RuntimeConfig& runtimeConfig)
{
    std::filesystem::path requestedPath(command.stringValue);
    if (requestedPath.empty())
    {
        requestedPath = "hair_debug.bmp";
    }
    if (requestedPath.extension().empty())
    {
        requestedPath += ".bmp";
    }
    else if (requestedPath.extension() != ".bmp" &&
             requestedPath.extension() != ".BMP")
    {
        requestedPath.replace_extension(".bmp");
    }
    if (requestedPath.is_absolute())
    {
        return requestedPath.lexically_normal().string();
    }

    return (
        std::filesystem::path(runtimeConfig.GetResourcePath()) /
        "generated" /
        "screenshots" /
        requestedPath).lexically_normal().string();
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
    case RuntimeCommandType::RunShaderReloadTest:
        ApplyShaderReloadTest(runtimeConfig, runtimeTestHooks, diagnostics);
        break;
    case RuntimeCommandType::RunShaderComputeReloadTest:
        ApplyShaderComputeReloadTest(runtimeConfig, runtimeTestHooks, diagnostics);
        break;
    case RuntimeCommandType::RunWorldGraphTransactionTest:
        ApplyWorldGraphTransactionTest(
            runtimeConfig,
            runtimeTestHooks,
            diagnostics);
        break;
    case RuntimeCommandType::SetDebugViewMode:
        renderSystem.SetDebugViewMode(command.intValue);
        diagnostics.ReportInfo("Debug view mode set to " + std::to_string(command.intValue));
        break;
    case RuntimeCommandType::CaptureScreenshot:
    {
        const std::string screenshotPath =
            ResolveScreenshotPath(command, runtimeConfig);
        renderSystem.RequestScreenshot(screenshotPath);
        diagnostics.ReportInfo("Screenshot queued: " + screenshotPath);
        break;
    }
    case RuntimeCommandType::SetCameraPosition:
    case RuntimeCommandType::SetCameraLookAt:
    case RuntimeCommandType::SetCameraPose:
    case RuntimeCommandType::GetCameraState:
    {
        const std::shared_ptr<World>& activeWorld = worldManager.GetActiveWorld();
        if (!activeWorld || !activeWorld->GetCamera())
        {
            diagnostics.ReportWarning("Camera command ignored because no active camera exists.");
            break;
        }

        const std::shared_ptr<Camera>& camera = activeWorld->GetCamera();
        if (command.type == RuntimeCommandType::SetCameraPosition)
        {
            camera->SetPosition(command.cameraPositionValue);
            diagnostics.ReportInfo(
                "Camera position set to " + FormatVector3(camera->GetPosition()));
        }
        else if (command.type == RuntimeCommandType::SetCameraLookAt)
        {
            if ((command.cameraLookAtValue - camera->GetPosition()).squaredNorm() <= 1.0e-8f)
            {
                diagnostics.ReportWarning("Camera look-at ignored because target equals camera position.");
                break;
            }
            camera->SetCamera(
                camera->GetPosition(),
                command.cameraLookAtValue,
                camera->GetUpVector());
            diagnostics.ReportInfo(
                "Camera look-at set to " + FormatVector3(command.cameraLookAtValue));
        }
        else if (command.type == RuntimeCommandType::SetCameraPose)
        {
            if ((command.cameraLookAtValue - command.cameraPositionValue).squaredNorm() <= 1.0e-8f)
            {
                diagnostics.ReportWarning("Camera pose ignored because target equals camera position.");
                break;
            }
            camera->SetCamera(
                command.cameraPositionValue,
                command.cameraLookAtValue,
                command.cameraUpValue);
            diagnostics.ReportInfo(
                "Camera pose set: position=" + FormatVector3(camera->GetPosition()) +
                ", forward=" + FormatVector3(camera->GetForwardVector()));
        }
        else
        {
            diagnostics.ReportInfo(
                "Camera state: position=" + FormatVector3(camera->GetPosition()) +
                ", forward=" + FormatVector3(camera->GetForwardVector()) +
                ", up=" + FormatVector3(camera->GetUpVector()));
        }
        break;
    }
    case RuntimeCommandType::SetEnvironmentIntensity:
        renderSystem.SetEnvironmentIntensity(command.floatValue);
        diagnostics.ReportInfo("Environment intensity set to " + std::to_string(command.floatValue));
        break;
    case RuntimeCommandType::SetProceduralSkyParameters:
        ApplyProceduralSkyParameters(
            command,
            worldManager,
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

void RuntimeCommandExecutor::ApplyShaderReloadTest(
    const RuntimeConfig& runtimeConfig,
    RuntimeTestHooks& runtimeTestHooks,
    const DiagnosticsSubsystem& diagnostics) const
{
    (void)runtimeTestHooks.BeginShaderReloadTest(
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

void RuntimeCommandExecutor::ApplyWorldGraphTransactionTest(
    const RuntimeConfig& runtimeConfig,
    RuntimeTestHooks& runtimeTestHooks,
    const DiagnosticsSubsystem& diagnostics) const
{
    (void)runtimeTestHooks.BeginWorldGraphTransactionTest(
        runtimeConfig.GetResourcePath(),
        diagnostics);
}

void RuntimeCommandExecutor::ApplyProceduralSkyParameters(
    const RuntimeCommand& command,
    WorldManager& worldManager,
    const DiagnosticsSubsystem& diagnostics) const
{
    const std::shared_ptr<World>& activeWorld = worldManager.GetActiveWorld();
    if (!activeWorld)
    {
        diagnostics.ReportError(
            "Procedural sky parameters update rejected because no active World exists.");
        return;
    }

    const WorldEnvironment& currentEnvironment = activeWorld->GetEnvironment();
    if (currentEnvironment.type != EnvironmentType::ProceduralSky)
    {
        diagnostics.ReportError(
            "Procedural sky parameters update rejected because the active World uses HDRI.");
        return;
    }

    WorldEnvironment updatedEnvironment = currentEnvironment;
    updatedEnvironment.skyParameters = command.skyParametersValue;
    activeWorld->SetEnvironment(std::move(updatedEnvironment));
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
