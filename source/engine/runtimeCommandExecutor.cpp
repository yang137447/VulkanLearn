#include "engine/runtimeCommandExecutor.h"

#include <cmath>
#include <filesystem>
#include <exception>
#include <fstream>
#include <memory>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>

#include "engine/diagnosticsSubsystem.h"
#include "editor/ui/editorSceneActorTree.h"
#include "editor/scene/editorSceneActorBuilder.h"
#include "editor/service/sceneDocumentService.h"
#include "engine/runtimeConfig.h"
#include "engine/runtimeTestHooks.h"
#include "input/inputSubsystem.h"
#include "materialInstance.h"
#include "render/resource/rendererResourceCache.h"
#include "renderGraph.h"
#include "renderSystem.h"
#include "sceneNode.h"
#include "shader/build/atomicFile.h"
#include "world/loading/worldTransitionCoordinator.h"
#include "world/world.h"
#include "animation/skeletalPlayback.h"
#include "script/scriptRuntime.h"

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

// 可见性写入端：把"大纲身份 → World 对象"这一步收在一个地方，
// 让命令分支只说类型语义，不重复 World 的表结构。返回 false 表示对象不在当前 World。
bool ApplyActorVisibility(
    World& world,
    const EditorUi::EditorSceneActorIdentity& identity,
    bool visible)
{
    using EditorUi::EditorSceneActorType;
    switch (identity.type)
    {
    case EditorSceneActorType::Mesh:
        // 网格 actor 的身份就是 sceneObjectIdentity（大纲按它聚合 section），
        // 因此这里一次写全该 actor 的所有 section，而不是找同名的单个渲染对象。
        return world.SetMeshSceneObjectVisible(identity.name, visible);
    case EditorSceneActorType::DirectionalLight:
    case EditorSceneActorType::PointLight:
    case EditorSceneActorType::SpotLight:
        return world.SetLightVisible(identity.name, visible);
    case EditorSceneActorType::SquareAreaLight:
        return world.SetSquareAreaLightVisible(identity.name, visible);
    // 相机 / 环境 / 场景风 / 风剖面没有渲染侧可见性；蓝图实例的可见性属于脚本状态
    //（destroyed）。调用方已用 SceneActorHasVisibilityToggle() 挡住，这里不提供任何
    // 默认成功路径。
    case EditorSceneActorType::Camera:
    case EditorSceneActorType::Environment:
    case EditorSceneActorType::WindProfile:
    case EditorSceneActorType::SceneWind:
    case EditorSceneActorType::Script:
        return false;
    }
    return false;
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
        "Generated" /
        "Screenshots" /
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
    const DiagnosticsSubsystem& diagnostics,
    InputSubsystem* input,
    ScriptRuntimeSubsystem* scripts,
    Editor::SceneDocumentService* sceneDocuments) const
{
    RuntimeCommandExecutionResult executionResult;
    // sweep 是显式命令驱动的有限时长运动：这里每个 GT 帧推进一次，未激活时为零开销。
    // 必须在 drain 之前推进，保证本帧新入队的命令仍然只改变意图、不额外多走一步。
    TickMeshSweep(worldManager, diagnostics);
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
            executionResult,
            input,
            scripts,
            sceneDocuments);
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
    RuntimeCommandExecutionResult& executionResult,
    InputSubsystem* input,
    ScriptRuntimeSubsystem* scripts,
    Editor::SceneDocumentService* sceneDocuments) const
{
    switch (command.type)
    {
    case RuntimeCommandType::LoadWorld:
        // 整 World 重载会换掉 generation 和全部 actor，sweep 的起点/身份随即失效，
        // 因此在这里显式取消，避免它在新 World 上继续写同名对象。
        CancelMeshSweep(diagnostics);
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
    case RuntimeCommandType::ControlAnimation:
    {
        const auto& world = worldManager.GetActiveWorld();
        if (!world || renderSystem.IsOfflineMode()) break;
        const uint64_t expected = command.sourceText == "ui"
            ? command.actorProperty.worldGeneration : world->GetGeneration();
        if (command.stringValue != "get" &&
            !world->ControlAnimation(expected, command.stringValue, command.floatValue))
        {
            diagnostics.ReportWarning("Animation control rejected for this World or clip range");
            break;
        }
        std::unordered_set<const Animation::SkeletalPlayback*> reported;
        for (const auto& entry : world->GetMeshObjects())
            if (entry.second.animation && reported.insert(entry.second.animation.get()).second)
            {
                const auto& player = *entry.second.animation;
                diagnostics.ReportInfo("Animation[" + entry.first + "]: clip=" +
                    std::to_string(player.SelectedClip()) + " id=" + player.SelectedClipId() +
                    " time=" + std::to_string(player.Time()) +
                    " duration=" + std::to_string(player.Duration()) +
                    " playing=" + std::to_string(player.IsPlaying()));
            }
        break;
    }
    case RuntimeCommandType::ControlScriptAnimation:
    {
        const auto& world = worldManager.GetActiveWorld();
        if (!world || renderSystem.IsOfflineMode()) break;
        const uint64_t expected = command.sourceText == "ui"
            ? command.actorProperty.worldGeneration : world->GetGeneration();
        if (!world->ControlScriptAnimation(expected, command.stringValue, command.intValue, command.floatValue))
            diagnostics.ReportWarning("Script animation control rejected for this World or component");
        break;
    }
    case RuntimeCommandType::ControlDebugDraw:
    {
        const auto& world = worldManager.GetActiveWorld();
        if (!world || renderSystem.IsOfflineMode()) break;
        if (command.sourceText == "ui" && command.actorProperty.worldGeneration != world->GetGeneration())
        { diagnostics.ReportWarning("DrawDebug rejected for stale World"); break; }
        try { diagnostics.ReportInfo(Debug::ApplyControl(world->GetDebugDraw(), command.debugDrawControl)); }
        catch (const std::invalid_argument& error) { diagnostics.ReportWarning(error.what()); }
        break;
    }
    case RuntimeCommandType::ControlMotionReplay:
    {
        const auto& world = worldManager.GetActiveWorld();
        if (!world || renderSystem.IsOfflineMode()) break;
        if (command.sourceText == "ui" && command.actorProperty.worldGeneration != world->GetGeneration())
        {
            diagnostics.ReportWarning("Motion replay rejected for stale World");
            break;
        }
        world->SetMotionDebugReplayFrame(command.intValue);
        break;
    }
    case RuntimeCommandType::ControlMotionDebug:
    {
        const auto& world = worldManager.GetActiveWorld();
        if (!world || renderSystem.IsOfflineMode()) break;
        if (command.sourceText == "ui" && command.actorProperty.worldGeneration != world->GetGeneration())
        {
            diagnostics.ReportWarning("Motion debug rejected for stale World");
            break;
        }
        world->SetMotionDebugEnabled(command.intValue == 1);
        diagnostics.ReportInfo("Motion debug " + std::string(command.intValue == 1 ? "enabled" : "disabled"));
        break;
    }
    case RuntimeCommandType::SetOfflineMode:
    {
        const bool fromUi = command.sourceText == "ui";
        OfflineModeCommandFeedback feedback;
        feedback.requestedMode = command.intValue;
        if (command.intValue != 0 && command.intValue != 1)
        {
            feedback.rejectionReason = "Offline mode must be 0 or 1";
            diagnostics.ReportWarning(feedback.rejectionReason);
            if (fromUi) executionResult.offlineUiModeFeedback = std::move(feedback);
            break;
        }
        if (renderSystem.IsOfflineMode() == (command.intValue == 1))
        {
            if (fromUi) executionResult.offlineUiModeFeedback = std::move(feedback);
            break;
        }
        std::string message;
        if (renderSystem.SetOfflineMode(command.intValue != 0, message))
            diagnostics.ReportInfo(message);
        else
        {
            feedback.rejectionReason = message;
            diagnostics.ReportWarning("Offline mode: " + message);
        }
        // 失败不改 renderer 的模式/有效版本，只把拒绝诊断带回 UI 快照。
        if (fromUi) executionResult.offlineUiModeFeedback = std::move(feedback);
        break;
    }
    case RuntimeCommandType::SetOfflineSetting:
    {
        if (const char* error = OfflineSettingValidationError(
                command.stringValue, command.intValue))
        {
            diagnostics.ReportWarning(error);
            break;
        }
        std::string message;
        if (!renderSystem.SetOfflineSetting(command.stringValue, command.intValue, message))
            diagnostics.ReportWarning(message);
        break;
    }
    case RuntimeCommandType::SetOfflineAov:
        if (command.intValue >= 0 && command.intValue <= 2)
            renderSystem.SetOfflineAov(static_cast<OfflineAov>(command.intValue));
        else diagnostics.ReportWarning("Invalid offline AOV");
        break;
    case RuntimeCommandType::SetOfflineExposure:
    {
        std::string message;
        if (!renderSystem.SetOfflineExposure(command.floatValue, message))
            diagnostics.ReportWarning(message);
        break;
    }
    case RuntimeCommandType::ControlOfflineSession:
    {
        if (!renderSystem.IsOfflineMode() ||
            command.intValue < 0 || command.intValue > 3)
        {
            diagnostics.ReportWarning("Offline session command requires offline mode and a valid action");
            break;
        }
        const auto state = renderSystem.GetOfflineState();
        const bool canPause = state == Offline::SessionState::Preparing ||
            state == Offline::SessionState::Rendering;
        const bool canResume = state == Offline::SessionState::Paused;
        const bool canStop = canPause || canResume;
        if ((command.intValue == 0 && !canPause) ||
            (command.intValue == 1 && !canResume) ||
            (command.intValue == 2 && !canStop))
        {
            diagnostics.ReportWarning("Offline session action is unavailable in the current state");
            break;
        }
        renderSystem.ControlOfflineSession(command.intValue);
        break;
    }
    case RuntimeCommandType::ExportOffline:
    {
        const auto result = renderSystem.CaptureOfflineResult();
        if (!result)
        {
            diagnostics.ReportWarning("No published offline result to export");
            break;
        }
        const auto root = std::filesystem::path(runtimeConfig.GetResourcePath()) /
            "Generated" / "OfflineRenderer";
        const std::string scene = std::filesystem::path(
            result->manifest.value("scene", std::string("scene"))).stem().string();
        const std::string digest = result->manifest.value("inputDigest", std::string());
        const std::string base = scene + "-" + digest.substr(0, 12) + "-" +
            std::to_string(result->film->completedSamples);
        std::filesystem::path directory = root / base;
        // 单次结果仍只 Capture 一回；重复导出分配新目录，既不覆盖旧 manifest，
        // 也不把不同进程碰巧相同的 task counter 当作全局唯一身份。
        for (unsigned sequence = 2;
             std::filesystem::exists(directory) ||
             std::filesystem::exists(directory.string() + ".partial");
             ++sequence)
            directory = root / (base + "-" + std::to_string(sequence));
        std::string message;
        if (renderSystem.ExportOffline(*result, directory, message)) diagnostics.ReportInfo(message);
        else diagnostics.ReportWarning(message);
        break;
    }
    case RuntimeCommandType::CaptureScreenshot:
    {
        const std::string screenshotPath =
            ResolveScreenshotPath(command, runtimeConfig);
        renderSystem.RequestScreenshot(screenshotPath);
        diagnostics.ReportInfo("Screenshot queued: " + screenshotPath);
        break;
    }
    case RuntimeCommandType::SetCameraExposure:
    {
        const std::shared_ptr<World>& activeWorld = worldManager.GetActiveWorld();
        if (!activeWorld || !activeWorld->GetCamera())
        {
            diagnostics.ReportWarning("Exposure command ignored because no active camera exists.");
            break;
        }
        const std::shared_ptr<Camera>& camera = activeWorld->GetCamera();
        try
        {
            // 按执行时的 Camera 状态只改一个字段；完整候选校验成功才写回，
            // 避免连续 UI 命令覆盖彼此，或无效 EV 范围污染下一帧快照。
            const CameraExposure candidate = EditCameraExposure(
                camera->GetExposure(), command.stringValue, command.floatValue);
            camera->SetExposure(candidate);
            diagnostics.ReportInfo("Camera exposure " + command.stringValue +
                " set to " + std::to_string(command.floatValue));
        }
        catch (const std::invalid_argument& error)
        {
            diagnostics.ReportWarning(error.what());
        }
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
    case RuntimeCommandType::SetSunIntensity:
    {
        const std::shared_ptr<World>& activeWorld =
            worldManager.GetActiveWorld();
        if (!activeWorld || !activeWorld->GetPrimaryDirectionalLight())
        {
            diagnostics.ReportWarning(
                "Sun intensity ignored because the active World has no directionalLight.");
            break;
        }

        // 主太阳沿用 WorldBuilder 的约定：场景中第一个 directionalLight。
        activeWorld->GetPrimaryDirectionalLight()->SetIntensity(
            command.floatValue);
        diagnostics.ReportInfo(
            "Sun intensity set to " + std::to_string(command.floatValue));
        break;
    }
    case RuntimeCommandType::SetActorVisible:
    {
        const std::shared_ptr<World>& activeWorld =
            worldManager.GetActiveWorld();
        if (!activeWorld)
        {
            diagnostics.ReportWarning(
                "Actor visibility ignored because no active World exists.");
            break;
        }

        // 命令跨越 GT→执行边界只带稳定身份字符串，因此这里解析并核对对象是否仍在
        // 当前 World 里：旧场景 / 已改名 / 伪造身份都必须被拒绝并给出诊断，
        // 而不是静默改不到东西或套用到同名的新对象上。
        EditorUi::EditorSceneActorIdentity identity;
        if (!EditorUi::ParseSceneActorIdentity(command.stringValue, identity))
        {
            diagnostics.ReportWarning(
                "Actor visibility rejected because the identity is not a known actor: " +
                command.stringValue);
            break;
        }
        if (!EditorUi::SceneActorHasVisibilityToggle(identity.type))
        {
            diagnostics.ReportWarning(
                "Actor visibility rejected because this actor type has no visibility state: " +
                command.stringValue);
            break;
        }
        if (!ApplyActorVisibility(*activeWorld, identity, command.intValue != 0))
        {
            diagnostics.ReportWarning(
                "Actor visibility rejected because the actor is not in the active World: " +
                command.stringValue);
            break;
        }

        diagnostics.ReportInfo(
            std::string("Actor ") + identity.name + " " +
            (command.intValue != 0 ? "shown." : "hidden."));
        break;
    }
    case RuntimeCommandType::SetActorProperty:
    {
        const auto& activeWorld = worldManager.GetActiveWorld();
        std::string error;
        if (!activeWorld)
            error = "No active World.";
        else if (sceneDocuments && !sceneDocuments->PrepareActorEdit(
            activeWorld->GetScenePath(), command.actorProperty.identity, error))
        {
            // 文档身份必须先验证，避免 World 接受了修改却没有可保存的作者目标。
        }
        else if (ApplyEditorActorProperty(*activeWorld, command.actorProperty, error))
        {
            if (command.actorProperty.property == EditorActorProperty::CastShadows)
                renderSystem.SetCsmSettings(activeWorld->GetCsmSettings());
            if (sceneDocuments)
                for (const auto& actor : BuildEditorSceneActors(*activeWorld))
                    if (actor.identity == command.actorProperty.identity)
                        sceneDocuments->RecordActorEdit(actor, command.actorProperty);
            break;
        }
        diagnostics.ReportWarning("Actor edit rejected: " + error);
        break;
    }
    case RuntimeCommandType::SetMeshPlacement:
        ApplyMeshPlacement(command, worldManager, diagnostics);
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
    case RuntimeCommandType::InjectInput:
    {
        if (!input)
        {
            diagnostics.ReportWarning("Input injection unavailable");
            break;
        }
        try
        {
            input->InjectAction(command.stringValue, command.intValue != 0);
            diagnostics.ReportInfo("Input injected: " + command.stringValue +
                (command.intValue != 0 ? " down" : " up"));
        }
        catch (const std::exception& error)
        {
            diagnostics.ReportWarning(std::string("Input injection rejected: ") + error.what());
        }
        break;
    }
    case RuntimeCommandType::ReloadAssembly:
    {
        const auto& world = worldManager.GetActiveWorld();
        if (!world || renderSystem.IsOfflineMode() || !scripts)
        {
            diagnostics.ReportWarning("Assembly reload unavailable");
            break;
        }
        std::string error;
        if (!scripts->ReloadAssembly(*world, error))
            diagnostics.ReportWarning("Assembly reload rejected: " + error);
        else
            diagnostics.ReportInfo("Assembly reload committed");
        break;
    }
    case RuntimeCommandType::SetScriptPlayState:
    {
        const auto& world = worldManager.GetActiveWorld();
        if (!world || renderSystem.IsOfflineMode() || !scripts)
        {
            diagnostics.ReportWarning("Script play state unavailable");
            break;
        }
        const auto state = static_cast<ScriptPlayState>(command.intValue);
        std::string error;
        if (!scripts->Control(*world, state, error))
            diagnostics.ReportWarning("Script play state rejected: " + error);
        else
            diagnostics.ReportInfo("Script play state: " + std::to_string(command.intValue));
        break;
    }
    case RuntimeCommandType::ReloadShaders:
        executionResult.shaderReloadRequested = true;
        executionResult.shaderReloadScope =
            command.shaderReloadScope;
        break;
    case RuntimeCommandType::ReportShaderCacheStatistics:
        executionResult.shaderCacheStatisticsRequested = true;
        break;
    case RuntimeCommandType::PrintGpuPassStats:
        renderSystem.PrintGpuPassStats();
        break;
    case RuntimeCommandType::PrintPerformanceJson:
        renderSystem.PrintPerformanceJson();
        break;
    case RuntimeCommandType::RequestHairTrace:
        // 只校验值语义并登记；GPU buffer/文件由渲染线程在 Record 中创建，
        // Console/GT 不直接接触 World 或 Vulkan 资源。
        if (command.stringValue.empty() || command.hairTraceFrames == 0 ||
            command.hairTraceGuideStride == 0)
        {
            diagnostics.ReportWarning(
                "Hair trace rejected: path must be non-empty and frames/stride positive");
            break;
        }
        renderSystem.RequestHairTrace(command.stringValue, command.hairTraceFrames,
            command.hairTraceGuideStride);
        diagnostics.ReportInfo("Hair trace queued: file=" + command.stringValue +
            " frames=" + std::to_string(command.hairTraceFrames) +
            " guideStride=" + std::to_string(command.hairTraceGuideStride));
        break;
    case RuntimeCommandType::ResetHairSimulation:
        // 只登记值语义；ResetPositions 由渲染线程在下一个正 delta 帧执行，
        // 不重载 World、不重新解析 TFX。
        renderSystem.RequestHairSimulationReset(command.stringValue);
        diagnostics.ReportInfo(command.stringValue.empty()
            ? "Hair reset queued for all TressFX objects"
            : "Hair reset queued for object \"" + command.stringValue + "\"");
        break;
    case RuntimeCommandType::SetHairSimulationParameter:
    {
        // 字段白名单与值域在 GT 先收口；失败不排队，避免把非法参数带进下一帧
        // 的 params UBO。对象是否存在/是否 GPU backend 由渲染线程判定。
        VL::Hair::HairLiveParameterRequest request = command.hairLiveParameter;
        std::string rejection;
        if (!VL::Hair::ValidateHairLiveParameterRequest(request, rejection))
        {
            diagnostics.ReportWarning("Hair parameter rejected: " + rejection);
            break;
        }
        std::string values;
        for (uint32_t index = 0; index < request.valueCount; ++index)
        {
            if (index != 0) values += ",";
            values += std::to_string(request.values[index]);
        }
        renderSystem.RequestHairSimulationParameter(request);
        diagnostics.ReportInfo("Hair parameter queued: object=" + request.objectName +
            " field=" + request.field + " value=" + values);
        break;
    }
    }
}

void RuntimeCommandExecutor::ApplyMeshPlacement(
    const RuntimeCommand& command,
    WorldManager& worldManager,
    const DiagnosticsSubsystem& diagnostics) const
{
    const MeshPlacementRequest& request = command.meshPlacement;
    const std::shared_ptr<World>& activeWorld = worldManager.GetActiveWorld();
    if (!activeWorld)
    {
        diagnostics.ReportWarning(
            "Mesh placement ignored because no active World exists.");
        return;
    }

    // UI 预览命令可能在同批 LoadWorld 之前入队；执行时再次拒绝旧代数，Console 不受此门控。
    if (command.sourceText == "ui" &&
        command.actorProperty.worldGeneration != activeWorld->GetGeneration())
    {
        diagnostics.ReportWarning("Mesh preview placement rejected because the World generation changed.");
        return;
    }

    // 绝对/增量/扫掠共用同一条身份校验：scene identity 不在当前 World 时必须拒绝并给出
    // 诊断，而不是静默改不到东西。加载中的新 World 可能同名，所以这里始终查活动 World。
    Eigen::Vector3f currentPosition = Eigen::Vector3f::Zero();
    Eigen::Vector3f currentRotation = Eigen::Vector3f::Zero();
    Eigen::Vector3f currentScale = Eigen::Vector3f::Ones();
    if (!activeWorld->GetMeshSceneObjectTransform(
        request.identity, currentPosition, currentRotation, currentScale))
    {
        diagnostics.ReportWarning(
            "Mesh placement rejected because the scene object identity is not in the active World: " +
            request.identity);
        return;
    }

    if (command.meshPlacementMode == MeshPlacementMode::Absolute)
    {
        if (!request.position.allFinite() || !request.rotation.allFinite())
        {
            diagnostics.ReportWarning(
                "Mesh placement rejected because the transform is not finite: " +
                request.identity);
            return;
        }
        // 绝对命令固定 scale=1：scale 会重标定局部 rest length/胶囊/SDF 度量，
        // 不属于本夹具要验证的 placement 惯性，因此不在命令面开放。
        if (!activeWorld->SetMeshSceneObjectTransform(
            request.identity, request.position, request.rotation,
            Eigen::Vector3f::Ones()))
        {
            diagnostics.ReportWarning(
                "Mesh transform rejected because the actor disappeared during execution: " +
                request.identity);
            return;
        }
        diagnostics.ReportInfo(
            "Mesh " + request.identity + " transform: position=" +
            FormatVector3(request.position) + " rotation=" +
            FormatVector3(request.rotation) + " scale=" +
            FormatVector3(Eigen::Vector3f::Ones()));
        return;
    }

    if (command.meshPlacementMode == MeshPlacementMode::Offset)
    {
        if (!request.delta.allFinite())
        {
            diagnostics.ReportWarning(
                "Mesh offset rejected because the delta is not finite: " +
                request.identity);
            return;
        }
        // 增量只叠加 position，保留当前 rotation/scale：否则一次平移会把朝向复位，
        // 让"连续平移"和"转身"两类证据混在一起。
        const Eigen::Vector3f nextPosition = currentPosition + request.delta;
        if (!nextPosition.allFinite() ||
            !activeWorld->SetMeshSceneObjectTransform(
                request.identity, nextPosition, currentRotation, currentScale))
        {
            diagnostics.ReportWarning(
                "Mesh offset rejected because the actor is gone or the result is not finite: " +
                request.identity);
            return;
        }
        diagnostics.ReportInfo(
            "Mesh " + request.identity + " offset: delta=" +
            FormatVector3(request.delta) + " position=" +
            FormatVector3(nextPosition) + " rotation=" +
            FormatVector3(currentRotation));
        return;
    }

    // Sweep：两个 identity 同时从当前根位置线性推进到 start+delta。角色与发丝必须一起动，
    // 否则 sourceToHair 会变，验的就不再是"共同 Actor placement"。
    if (request.secondIdentity.empty())
    {
        diagnostics.ReportWarning(
            "Mesh sweep rejected because a second scene object identity is required.");
        return;
    }
    if (!std::isfinite(request.seconds) || request.seconds <= 0.0f)
    {
        diagnostics.ReportWarning(
            "Mesh sweep rejected because the duration must be finite and positive.");
        return;
    }
    if (!request.delta.allFinite())
    {
        diagnostics.ReportWarning(
            "Mesh sweep rejected because the delta is not finite.");
        return;
    }
    Eigen::Vector3f secondPosition = Eigen::Vector3f::Zero();
    Eigen::Vector3f secondRotation = Eigen::Vector3f::Zero();
    Eigen::Vector3f secondScale = Eigen::Vector3f::Ones();
    if (!activeWorld->GetMeshSceneObjectTransform(
        request.secondIdentity, secondPosition, secondRotation, secondScale))
    {
        diagnostics.ReportWarning(
            "Mesh sweep rejected because the second scene object identity is not in the active World: " +
            request.secondIdentity);
        return;
    }

    // 新 sweep 直接替换旧状态，避免两个隐式运动源同时写同一个 actor。
    if (meshSweep.active)
        diagnostics.ReportInfo("Mesh sweep replaced the previously active sweep.");

    meshSweep.active = true;
    meshSweep.identity = request.identity;
    meshSweep.secondIdentity = request.secondIdentity;
    meshSweep.startPosition = currentPosition;
    meshSweep.secondStartPosition = secondPosition;
    meshSweep.rotation = currentRotation;
    meshSweep.secondRotation = secondRotation;
    meshSweep.scale = currentScale;
    meshSweep.secondScale = secondScale;
    meshSweep.delta = request.delta;
    meshSweep.seconds = request.seconds;
    meshSweep.started = std::chrono::steady_clock::now();
    diagnostics.ReportInfo(
        "Mesh sweep started: a=" + request.identity + " b=" + request.secondIdentity +
        " delta=" + FormatVector3(request.delta) +
        " seconds=" + std::to_string(request.seconds));
}

void RuntimeCommandExecutor::TickMeshSweep(
    WorldManager& worldManager,
    const DiagnosticsSubsystem& diagnostics) const
{
    if (!meshSweep.active) return;

    const std::shared_ptr<World>& activeWorld = worldManager.GetActiveWorld();
    if (!activeWorld)
    {
        meshSweep.active = false;
        diagnostics.ReportWarning(
            "Mesh sweep stopped because the active World disappeared.");
        return;
    }

    // 位置由"起点 + 总位移 * t"直接算出，而不是每帧累加：GT 掉帧或命令注入延迟只会
    // 改变采样点，不会让总位移漂移；t 被截断在 1，所以停下的位置精确等于 start+delta。
    const double elapsedSeconds = std::chrono::duration<double>(
        std::chrono::steady_clock::now() - meshSweep.started).count();
    const double progress = elapsedSeconds / static_cast<double>(meshSweep.seconds);
    const float t = static_cast<float>(progress >= 1.0 ? 1.0 : (progress < 0.0 ? 0.0 : progress));
    const Eigen::Vector3f position = meshSweep.startPosition + meshSweep.delta * t;
    const Eigen::Vector3f secondPosition = meshSweep.secondStartPosition + meshSweep.delta * t;
    const bool firstApplied = activeWorld->SetMeshSceneObjectTransform(
        meshSweep.identity, position, meshSweep.rotation, meshSweep.scale);
    const bool secondApplied = activeWorld->SetMeshSceneObjectTransform(
        meshSweep.secondIdentity, secondPosition, meshSweep.secondRotation,
        meshSweep.secondScale);
    if (!firstApplied || !secondApplied)
    {
        meshSweep.active = false;
        diagnostics.ReportWarning(
            "Mesh sweep stopped because an actor left the active World: " +
            (firstApplied ? meshSweep.secondIdentity : meshSweep.identity));
        return;
    }

    if (progress < 1.0) return;
    meshSweep.active = false;
    diagnostics.ReportInfo(
        "Mesh sweep finished: a=" + meshSweep.identity + " position=" +
        FormatVector3(position) + " b=" + meshSweep.secondIdentity + " position=" +
        FormatVector3(secondPosition));
}

void RuntimeCommandExecutor::CancelMeshSweep(
    const DiagnosticsSubsystem& diagnostics) const
{
    if (!meshSweep.active) return;
    meshSweep.active = false;
    diagnostics.ReportInfo(
        "Mesh sweep cancelled by World reload: a=" + meshSweep.identity +
        " b=" + meshSweep.secondIdentity);
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
    executionResult.loadWorldExtraSceneObjects = command.extraSceneObjects;
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
            // 读取作用域必须在暂存文件落盘并替换目标文件之前结束。
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
