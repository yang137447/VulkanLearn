#include "engine/runtimeCommandExecutor.h"

#include <algorithm>
#include "engine/destructibleRuntime.h"
#include "engine/projectileRuntime.h"
#include <cmath>
#include <filesystem>
#include <exception>
#include <fstream>
#include <iostream>
#include <map>
#include <memory>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>

#include "engine/diagnosticsSubsystem.h"
#include "engine/editorHost.h"
#include "engine/runtimeConfig.h"
#include "engine/runtimeTestHooks.h"
#include "input/inputSubsystem.h"
#include "materialInstance.h"
#include "particles/particleAsset.h"
#include "particles/particleRuntimeCommand.h"
#include "render/resource/rendererResourceCache.h"
#include "renderGraph.h"
#include "renderSystem.h"
#include "sceneNode.h"
#include "shader/build/atomicFile.h"
#include "world/actorIdentity.h"
#include "world/actorProperty.h"
#include "world/world.h"
#include "animation/skeletalPlayback.h"
#include "script/scriptRuntime.h"
#include "engine/runtimeCommandExecutorInternal.h"

namespace VL
{
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
    RuntimeTestHooks& runtimeTestHooks,
    const RuntimeConfig& runtimeConfig,
    const DiagnosticsSubsystem& diagnostics,
    InputSubsystem* input,
    ScriptRuntimeSubsystem* scripts,
    IEditorHost* editorHost,
    DestructibleRuntimeRegistry* destructibleRuntimes,
    ProjectileRuntime* projectileRuntime) const
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
            runtimeTestHooks,
            runtimeConfig,
            diagnostics,
            executionResult,
            input,
            scripts,
            editorHost,
            destructibleRuntimes,
            projectileRuntime);
    }
    return executionResult;
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
    const RuntimeConfig& runtimeConfig,
    const DiagnosticsSubsystem& diagnostics,
    RuntimeCommandExecutionResult& executionResult) const
{
    executionResult.loadWorldAttempted = true;
    executionResult.loadWorldSucceeded = false;
    executionResult.loadWorldCommandPath = command.stringValue;
    executionResult.loadWorldResolvedPath.clear();
    executionResult.loadWorldExtraSceneObjects = command.extraSceneObjects;
    executionResult.loadWorldParticleDocuments.clear();
    if (command.particleDocument.is_object() &&
        !command.particleDocumentPath.empty())
    {
        executionResult.loadWorldParticleDocuments.emplace(
            command.particleDocumentPath,
            command.particleDocument);
    }
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
