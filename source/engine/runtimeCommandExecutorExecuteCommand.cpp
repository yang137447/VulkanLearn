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
void RuntimeCommandExecutor::ExecuteCommand(
    const RuntimeCommand& command,
    RenderSystem& renderSystem,
    WorldManager& worldManager,
    RuntimeTestHooks& runtimeTestHooks,
    const RuntimeConfig& runtimeConfig,
    const DiagnosticsSubsystem& diagnostics,
    RuntimeCommandExecutionResult& executionResult,
    InputSubsystem* input,
    ScriptRuntimeSubsystem* scripts,
    IEditorHost* editorHost,
    DestructibleRuntimeRegistry* destructibleRuntimes,
    ProjectileRuntime* projectileRuntime) const
{
    switch (command.type)
    {
    case RuntimeCommandType::LoadWorld:
        // 整 World 重载会换掉 generation 和全部 actor，sweep 的起点/身份随即失效，
        // 因此在这里显式取消，避免它在新 World 上继续写同名对象。
        CancelMeshSweep(diagnostics);
        ApplyLoadWorld(command, runtimeConfig, diagnostics, executionResult);
        break;
    case RuntimeCommandType::UpdateParticlePreview:
    {
        const std::shared_ptr<World>& world =
            worldManager.GetActiveWorld();
        if (!world)
        {
            diagnostics.ReportWarning(
                "Particle preview update ignored because no active World exists.");
            break;
        }
        if (!command.particleDocument.is_object())
        {
            diagnostics.ReportWarning(
                "Particle preview update ignored because the draft document is not a JSON object.");
            break;
        }
        try
        {
            ParticleSystemAsset asset =
                ParticleAssetLoader::ParseJson(
                    command.particleDocument,
                    command.stringValue);
            const size_t replaced =
                world->UpdateParticleSystemAsset(
                    command.stringValue,
                    std::move(asset));
            if (replaced == 0)
            {
                diagnostics.ReportWarning(
                    "Particle preview update found no matching particle system: " +
                    command.stringValue);
            }
            else
            {
                diagnostics.ReportInfo(
                    "Particle preview updated in place: " +
                    command.stringValue + " (" +
                    std::to_string(replaced) + " object(s))");
            }
        }
        catch (const std::exception& error)
        {
            diagnostics.ReportWarning(
                std::string("Particle preview update rejected: ") +
                error.what());
        }
        break;
    }
    case RuntimeCommandType::DumpParticles:
    {
        const uint32_t maxPerEmitter = static_cast<uint32_t>(
            std::clamp(command.intValue, 0, 4096));
        std::string message;
        if (renderSystem.DumpParticles(
                command.stringValue,
                maxPerEmitter,
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
    case RuntimeCommandType::ReportParticleStats:
    {
        const ParticleFrameStats stats =
            renderSystem.GetParticleFrameStats();
        std::cout << stats.FormatLine() << std::endl;
        std::cout << stats.ToJson().dump() << std::endl;
        diagnostics.ReportInfo(stats.FormatLine());
        break;
    }
    case RuntimeCommandType::SpawnParticleSystem:
    {
        const std::shared_ptr<World>& world =
            worldManager.GetActiveWorld();
        if (!world)
        {
            diagnostics.ReportWarning(
                "Particle spawn ignored because no active World exists.");
            break;
        }

        std::string validationError;
        if (!ValidateParticleSpawnRequest(
                command.stringValue,
                command.secondaryStringValue,
                command.particleSpawnPosition,
                validationError))
        {
            diagnostics.ReportWarning(validationError);
            break;
        }

        try
        {
            ParticleSystemAsset asset =
                ParticleAssetLoader::Prepare(
                    command.secondaryStringValue);
            const bool replacing =
                world->FindParticleSystem(command.stringValue) != nullptr;
            WorldParticleSystem system;
            system.name = command.stringValue;
            system.assetPath = command.secondaryStringValue;
            system.asset = std::move(asset);
            system.model = BuildParticleSpawnModel(
                command.particleSpawnPosition);
            // console spawn 的 actor 也要能在大纲/Details 中回显同一组 TRS；
            // 位置来自命令，默认无旋转、单位缩放且立即可见。
            system.scenePosition = command.particleSpawnPosition;
            system.sceneRotation = Eigen::Vector3f::Zero();
            system.sceneScale = Eigen::Vector3f::Ones();
            system.visible = true;
            world->AddParticleSystem(
                command.stringValue,
                std::move(system));
            diagnostics.ReportInfo(
                std::string("Particle system ") +
                (replacing ? "replaced: " : "spawned: ") +
                command.stringValue + " (" +
                command.secondaryStringValue + ")");
        }
        catch (const std::exception& error)
        {
            diagnostics.ReportWarning(
                std::string("Particle spawn rejected: ") +
                error.what());
        }
        break;
    }
    case RuntimeCommandType::DestroyParticleSystem:
    {
        const std::shared_ptr<World>& world =
            worldManager.GetActiveWorld();
        if (!world)
        {
            diagnostics.ReportWarning(
                "Particle destroy ignored because no active World exists.");
            break;
        }
        if (!world->RemoveParticleSystem(command.stringValue))
        {
            diagnostics.ReportWarning(
                "Particle destroy found no system named: " +
                command.stringValue);
            break;
        }
        // 逻辑 World 立即移除；GPU 池由下一帧 Synchronize 交给 retire queue。
        diagnostics.ReportInfo(
            "Particle system destroyed: " + command.stringValue);
        break;
    }
    case RuntimeCommandType::RestartParticleSystem:
    {
        const std::shared_ptr<World>& world =
            worldManager.GetActiveWorld();
        if (!world)
        {
            diagnostics.ReportWarning(
                "Particle restart ignored because no active World exists.");
            break;
        }
        if (world->FindParticleSystem(command.stringValue) == nullptr)
        {
            diagnostics.ReportWarning(
                "Particle restart found no system named: " +
                command.stringValue);
            break;
        }
        // 只重置当前 World 实例的 CPU 侧模拟状态；不重新加载 PS 资产或纹理。
        renderSystem.RestartParticleSystems(command.stringValue);
        diagnostics.ReportInfo(
            "Particle system restarted: " + command.stringValue);
        break;
    }
    case RuntimeCommandType::KillParticle:
    {
        const std::shared_ptr<World>& world =
            worldManager.GetActiveWorld();
        if (!world)
        {
            diagnostics.ReportWarning(
                "Particle kill ignored because no active World exists.");
            break;
        }
        std::string validationError;
        if (!ValidateParticleKillRequest(
                command.stringValue,
                command.particleEmitterName,
                command.intValue,
                validationError))
        {
            diagnostics.ReportWarning(validationError);
            break;
        }
        if (world->FindParticleSystem(command.stringValue) == nullptr)
        {
            diagnostics.ReportWarning(
                "Particle kill found no system named: " +
                command.stringValue);
            break;
        }
        uint32_t stableIndex = 0;
        if (!world->FindParticleEmitterIndex(
                command.stringValue,
                command.particleEmitterName,
                stableIndex))
        {
            diagnostics.ReportWarning(
                "Particle kill found no emitter named: " +
                command.stringValue + "." +
                command.particleEmitterName);
            break;
        }
        std::string message;
        if (!renderSystem.KillParticle(
                command.stringValue,
                command.particleEmitterName,
                static_cast<uint32_t>(command.intValue),
                message))
        {
            diagnostics.ReportWarning(message);
            break;
        }
        diagnostics.ReportInfo(message);
        break;
    }
    case RuntimeCommandType::SpawnParticleAt:
    {
        const std::shared_ptr<World>& world =
            worldManager.GetActiveWorld();
        if (!world)
        {
            diagnostics.ReportWarning(
                "Particle spawn-at ignored because no active World exists.");
            break;
        }
        std::string validationError;
        if (!ValidateParticleSpawnAtRequest(
                command.stringValue,
                command.particleEmitterName,
                command.particleSpawnPosition,
                command.floatValue,
                validationError))
        {
            diagnostics.ReportWarning(validationError);
            break;
        }
        if (world->FindParticleSystem(command.stringValue) == nullptr)
        {
            diagnostics.ReportWarning(
                "Particle spawn-at found no system named: " +
                command.stringValue);
            break;
        }
        uint32_t stableIndex = 0;
        if (!world->FindParticleEmitterIndex(
                command.stringValue,
                command.particleEmitterName,
                stableIndex))
        {
            diagnostics.ReportWarning(
                "Particle spawn-at found no emitter named: " +
                command.stringValue + "." +
                command.particleEmitterName);
            break;
        }
        std::string message;
        if (!renderSystem.SpawnParticleAt(
                command.stringValue,
                command.particleEmitterName,
                command.particleSpawnPosition,
                command.floatValue,
                message))
        {
            diagnostics.ReportWarning(message);
            break;
        }
        diagnostics.ReportInfo(message);
        break;
    }
    case RuntimeCommandType::SetParticleSystemParameter:
    {
        const std::shared_ptr<World>& world =
            worldManager.GetActiveWorld();
        if (!world)
        {
            diagnostics.ReportWarning(
                "Particle parameter ignored because no active World exists.");
            break;
        }
        if (world->FindParticleSystem(command.stringValue) == nullptr)
        {
            diagnostics.ReportWarning(
                "Particle parameter found no system named: " +
                command.stringValue);
            break;
        }

        std::string validationError;
        if (!ValidateParticleParameterName(
                command.secondaryStringValue,
                validationError))
        {
            diagnostics.ReportWarning(validationError);
            break;
        }

        if (command.secondaryStringValue == "spawnRateScale")
        {
            if (!ValidateParticleSpawnRateScale(
                    command.floatValue,
                    validationError))
            {
                diagnostics.ReportWarning(validationError);
                break;
            }
            if (!world->SetParticleSystemSpawnRateScale(
                    command.stringValue,
                    command.floatValue))
            {
                diagnostics.ReportWarning(
                    "Particle spawnRateScale was not applied: " +
                    command.stringValue);
                break;
            }
            diagnostics.ReportInfo(
                "Particle spawnRateScale set: " +
                command.stringValue + " = " +
                std::to_string(command.floatValue));
        }
        else if (command.secondaryStringValue == "colorTint")
        {
            if (!ValidateParticleColorTint(
                    command.particleColorTint,
                    validationError))
            {
                diagnostics.ReportWarning(validationError);
                break;
            }
            if (!world->SetParticleSystemColorTint(
                    command.stringValue,
                    command.particleColorTint))
            {
                diagnostics.ReportWarning(
                    "Particle colorTint was not applied: " +
                    command.stringValue);
                break;
            }
            diagnostics.ReportInfo(
                "Particle colorTint set: " +
                command.stringValue + " = " +
                FormatColorTint(command.particleColorTint));
        }
        else if (command.secondaryStringValue == "emitter")
        {
            uint32_t emitterIndex = 0;
            if (command.intValue >= 0)
                emitterIndex = static_cast<uint32_t>(command.intValue);
            else if (command.particleEmitterName.empty() ||
                     !world->FindParticleEmitterIndex(
                         command.stringValue,
                         command.particleEmitterName,
                         emitterIndex))
            {
                diagnostics.ReportWarning(
                    "Particle emitter requires a valid index or name.");
                break;
            }
            if (!ValidateParticleEmitterIndex(emitterIndex, validationError))
            {
                diagnostics.ReportWarning(validationError);
                break;
            }
            if (!ValidateParticleEmitterEnabledValue(
                    command.intValue2,
                    validationError))
            {
                diagnostics.ReportWarning(validationError);
                break;
            }
            const bool enabled = command.intValue2 != 0;
            if (!world->SetParticleSystemEmitterEnabled(
                    command.stringValue,
                    emitterIndex,
                    enabled))
            {
                diagnostics.ReportWarning(
                    "Particle emitter state was not applied: " +
                    command.stringValue + " emitter " +
                    std::to_string(emitterIndex));
                break;
            }
            diagnostics.ReportInfo(
                std::string("Particle emitter ") +
                (enabled ? "enabled: " : "disabled: ") +
                command.stringValue + " emitter " +
                std::to_string(emitterIndex));
        }
        else
        {
            if (!world->ResetParticleSystemRuntimeOverrides(
                    command.stringValue))
            {
                diagnostics.ReportWarning(
                    "Particle runtime overrides were not reset: " +
                    command.stringValue);
                break;
            }
            diagnostics.ReportInfo(
                "Particle runtime overrides reset: " +
                command.stringValue);
        }
        break;
    }
    case RuntimeCommandType::SpawnParticleEmitter:
    {
        const std::shared_ptr<World>& world =
            worldManager.GetActiveWorld();
        if (!world)
        {
            diagnostics.ReportWarning(
                "Particle emitter spawn ignored because no active World exists.");
            break;
        }
        if (world->FindParticleSystem(command.stringValue) == nullptr)
        {
            diagnostics.ReportWarning(
                "Particle emitter spawn found no system named: " +
                command.stringValue);
            break;
        }
        if (command.intValue < 0)
        {
            diagnostics.ReportWarning(
                "Particle emitter source index must be non-negative.");
            break;
        }
        uint32_t stableIndex = 0;
        if (!world->SpawnParticleEmitter(
                command.stringValue,
                static_cast<uint32_t>(command.intValue),
                command.particleEmitterName,
                stableIndex))
        {
            diagnostics.ReportWarning(
                "Particle emitter spawn was rejected for system: " +
                command.stringValue);
            break;
        }
        std::string emitterName = command.particleEmitterName;
        if (emitterName.empty())
            emitterName = "runtime." + std::to_string(stableIndex);
        diagnostics.ReportInfo(
            "Particle emitter spawned: " + command.stringValue +
            " stableIndex=" + std::to_string(stableIndex) +
            " name=" + emitterName);
        break;
    }
    case RuntimeCommandType::DestroyParticleEmitter:
    case RuntimeCommandType::RestartParticleEmitter:
    {
        const std::shared_ptr<World>& world =
            worldManager.GetActiveWorld();
        if (!world)
        {
            diagnostics.ReportWarning(
                "Particle emitter command ignored because no active World exists.");
            break;
        }
        if (world->FindParticleSystem(command.stringValue) == nullptr)
        {
            diagnostics.ReportWarning(
                "Particle emitter command found no system named: " +
                command.stringValue);
            break;
        }
        uint32_t stableIndex = 0;
        if (command.intValue >= 0)
        {
            stableIndex = static_cast<uint32_t>(command.intValue);
        }
        else if (command.particleEmitterName.empty() ||
                 !world->FindParticleEmitterIndex(
                     command.stringValue,
                     command.particleEmitterName,
                     stableIndex))
        {
            diagnostics.ReportWarning(
                "Particle emitter command requires an index or emitter name.");
            break;
        }
        const bool destroy =
            command.type == RuntimeCommandType::DestroyParticleEmitter;
        const bool applied = destroy
            ? world->DestroyParticleEmitter(
                command.stringValue, stableIndex)
            : world->RestartParticleEmitter(
                command.stringValue, stableIndex);
        if (!applied)
        {
            diagnostics.ReportWarning(
                std::string("Particle emitter ") +
                (destroy ? "destroy" : "restart") +
                " was rejected for system: " + command.stringValue);
            break;
        }
        diagnostics.ReportInfo(
            std::string("Particle emitter ") +
            (destroy ? "destroyed" : "restarted") + ": " +
            command.stringValue + " stableIndex=" +
            std::to_string(stableIndex));
        break;
    }
    case RuntimeCommandType::SetParticleSystemBudget:
    {
        const std::shared_ptr<World>& world =
            worldManager.GetActiveWorld();
        if (!world)
        {
            diagnostics.ReportWarning(
                "Particle budget ignored because no active World exists.");
            break;
        }
        const uint32_t maxTotal = command.intValue < 0
            ? 0u
            : static_cast<uint32_t>(command.intValue);
        const bool applied = command.stringValue == "world"
            ? world->SetWorldParticleBudget(maxTotal, command.floatValue)
            : world->SetParticleSystemBudget(
                command.stringValue, maxTotal, command.floatValue);
        if (!applied)
        {
            diagnostics.ReportWarning(
                "Particle budget was rejected: " + command.stringValue);
            break;
        }
        diagnostics.ReportInfo(
            "Particle budget set: " + command.stringValue +
            " maxTotal=" + std::to_string(maxTotal) +
            " emitterScale=" + std::to_string(command.floatValue));
        break;
    }
    case RuntimeCommandType::SetParticleSystemUserParameter:
    {
        const std::shared_ptr<World>& world =
            worldManager.GetActiveWorld();
        if (!world)
        {
            diagnostics.ReportWarning(
                "Particle user parameter ignored because no active World exists.");
            break;
        }
        const bool clear = command.intValue2 != 0;
        const bool applied = clear
            ? world->ClearParticleSystemUserParameter(
                command.stringValue, command.particleUserParameterName)
            : world->SetParticleSystemUserParameter(
                command.stringValue,
                command.particleUserParameterName,
                command.floatValue);
        if (!applied)
        {
            diagnostics.ReportWarning(
                std::string("Particle user parameter ") +
                (clear ? "clear" : "set") + " was rejected: " +
                command.stringValue + " key=" +
                command.particleUserParameterName);
            break;
        }
        diagnostics.ReportInfo(
            std::string("Particle user parameter ") +
            (clear ? "cleared: " : "set: ") + command.stringValue +
            " key=" + command.particleUserParameterName);
        break;
    }
    case RuntimeCommandType::SetParticleDataChannel:
    {
        const std::shared_ptr<World>& world =
            worldManager.GetActiveWorld();
        if (!world)
        {
            diagnostics.ReportWarning(
                "Particle data channel ignored because no active World exists.");
            break;
        }
        const bool clear = command.particleDataChannelClear;
        std::string validationError;
        const bool applied = command.particleDataChannelTyped
            ? ExecuteParticleDataChannelCommand(
                *world,
                clear,
                command.particleDataChannelName,
                command.particleDataChannelValue,
                validationError)
            : ExecuteParticleDataChannelCommand(
                *world,
                clear,
                command.particleDataChannelName,
                command.floatValue,
                validationError);
        if (!applied)
        {
            diagnostics.ReportWarning(validationError);
            break;
        }
        diagnostics.ReportInfo(
            std::string("Particle data channel ") +
            (clear ? "cleared: " : "set: ") +
            command.particleDataChannelName +
            (clear
                ? std::string()
                : "=" + (command.particleDataChannelTyped
                    ? command.particleDataChannelValue.ToJson().dump()
                    : std::to_string(command.floatValue))));
        break;
    }
    case RuntimeCommandType::ReportParticleDataChannels:
    {
        const std::shared_ptr<World>& world =
            worldManager.GetActiveWorld();
        if (!world)
        {
            diagnostics.ReportWarning(
                "Particle data channels ignored because no active World exists.");
            break;
        }
        std::map<std::string, ParticleDataChannelValue> ordered(
            world->GetParticleDataChannels().begin(),
            world->GetParticleDataChannels().end());
        nlohmann::json channels = nlohmann::json::object();
        for (const auto& [name, value] : ordered)
            channels[name] = value.ToJson();
        const std::string line = channels.dump();
        std::cout << line << std::endl;
        diagnostics.ReportInfo(line);
        break;
    }
    case RuntimeCommandType::SetParticleSystemAttachment:
    {
        const std::shared_ptr<World>& world =
            worldManager.GetActiveWorld();
        if (!world)
        {
            diagnostics.ReportWarning(
                "Particle attachment ignored because no active World exists.");
            break;
        }
        if (command.secondaryStringValue == "detach")
        {
            if (!world->DetachParticleSystem(command.stringValue))
            {
                diagnostics.ReportWarning(
                    "Particle detach found no system named: " +
                    command.stringValue);
                break;
            }
            diagnostics.ReportInfo(
                "Particle attachment cleared: " + command.stringValue);
            break;
        }
        if (!world->AttachParticleSystem(
                command.stringValue,
                command.secondaryStringValue,
                command.particleAttachmentTarget,
                command.particleAttachmentComponent,
                command.particleAttachmentPosition,
                command.particleAttachmentRotation,
                command.particleAttachmentScale))
        {
            diagnostics.ReportWarning(
                "Particle attachment was rejected: " +
                command.stringValue);
            break;
        }
        diagnostics.ReportInfo(
            "Particle attachment set: " + command.stringValue +
            " -> " + command.secondaryStringValue + ":" +
            command.particleAttachmentTarget +
            (command.particleAttachmentComponent.empty()
                ? std::string()
                : "/" + command.particleAttachmentComponent));
        break;
    }
    case RuntimeCommandType::ReportParticleTelemetry:
    {
        const std::shared_ptr<World>& world =
            worldManager.GetActiveWorld();
        if (!world)
        {
            diagnostics.ReportWarning(
                "Particle telemetry ignored because no active World exists.");
            break;
        }
        if (!command.stringValue.empty())
        {
            const nlohmann::json telemetry =
                world->DescribeParticleSystemTelemetry(
                    command.stringValue);
            if (telemetry.is_null())
            {
                diagnostics.ReportWarning(
                    "Particle telemetry found no system named: " +
                    command.stringValue);
                break;
            }
            std::cout << telemetry.dump() << std::endl;
            diagnostics.ReportInfo(telemetry.dump());
            break;
        }
        for (const auto& entry : world->GetParticleSystems())
        {
            const nlohmann::json telemetry =
                world->DescribeParticleSystemTelemetry(entry.first);
            std::cout << telemetry.dump() << std::endl;
            diagnostics.ReportInfo(telemetry.dump());
        }
        break;
    }
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
    case RuntimeCommandType::ImpactSoftBody:
        if (destructibleRuntimes == nullptr ||
            !destructibleRuntimes->LaunchImpact(command.stringValue,
                command.floatValue,
                Physics::FractureMaterialProfile::Balloon))
        {
            diagnostics.ReportWarning(
                "softbody impact target not found: " + command.stringValue);
            break;
        }
        break;
    case RuntimeCommandType::ImpactGlassPane:
        if (destructibleRuntimes == nullptr ||
            !destructibleRuntimes->LaunchImpact(command.stringValue,
                command.floatValue,
                Physics::FractureMaterialProfile::Glass))
        {
            diagnostics.ReportWarning(
                "glass impact target not found: " + command.stringValue);
            break;
        }
        break;
    case RuntimeCommandType::FireProjectile:
    {
        const auto& world = worldManager.GetActiveWorld();
        if (projectileRuntime == nullptr || !world || !world->GetRenderCamera())
        {
            diagnostics.ReportWarning(
                "shoot command ignored because the projectile runtime is unavailable");
            break;
        }
        projectileRuntime->Fire(*world, *world->GetRenderCamera());
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
        EditorSceneActorIdentity identity;
        if (!ParseSceneActorIdentity(command.stringValue, identity))
        {
            diagnostics.ReportWarning(
                "Actor visibility rejected because the identity is not a known actor: " +
                command.stringValue);
            break;
        }
        if (!SceneActorHasVisibilityToggle(identity.type))
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
        else if (command.actorProperty.preview)
        {
            // 拖动预览只改 live World，不要求文档可编辑、不写草稿/历史；
            // 释放时同一 Transform 以 preview=false 再进来一次，才产生单笔 Undo。
            if (!ApplyEditorActorProperty(*activeWorld, command.actorProperty, error))
                diagnostics.ReportWarning("Actor preview rejected: " + error);
            break;
        }
        else if (editorHost && !editorHost->PrepareActorEdit(
            activeWorld->GetScenePath(), command.actorProperty.identity, error))
        {
            // 文档身份必须先验证，避免 World 接受了修改却没有可保存的作者目标。
        }
        else if (ApplyEditorActorProperty(*activeWorld, command.actorProperty, error))
        {
            if (command.actorProperty.property == EditorActorProperty::CastShadows)
                renderSystem.SetCsmSettings(activeWorld->GetCsmSettings());
            if (editorHost)
                editorHost->RecordActorEdit(*activeWorld, command.actorProperty);
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
    case RuntimeCommandType::ReportTerrainFeedbackStatistics:
        executionResult.terrainFeedbackStatisticsRequested = true;
        break;
    case RuntimeCommandType::ReportTerrainStatistics:
        executionResult.terrainStatisticsRequested = true;
        break;
    case RuntimeCommandType::ReportTerrainLayerStatistics:
        executionResult.terrainLayerStatisticsRequested = true;
        break;
    case RuntimeCommandType::ReportTerrainLodStatistics:
        executionResult.terrainLodStatisticsRequested = true;
        break;
    case RuntimeCommandType::ReportTerrainChunkStatistics:
        executionResult.terrainChunkStatisticsRequested = true;
        break;
    case RuntimeCommandType::ReportTerrainCollisionStatistics:
        executionResult.terrainCollisionStatisticsRequested = true;
        break;
    case RuntimeCommandType::RunTerrainPageBakeParity:
        renderSystem.RequestTerrainPageBakeParity(
            command.intValue / 32,
            command.intValue % 32);
        break;
    case RuntimeCommandType::SetTerrainResidentCapacity:
        // 只写 active streaming state 的 atomic；真正的 page manager 变更在
        // 渲染线程帧首消费，避免 GT 与渲染线程同时改驻留状态。
        renderSystem.RequestTerrainResidentCapacity(
            static_cast<uint32_t>(std::max(command.intValue, 0)));
        break;
    case RuntimeCommandType::SetTerrainLayerResidentCapacity:
        renderSystem.RequestTerrainLayerResidentCapacity(
            static_cast<uint32_t>(std::max(command.intValue, 0)));
        break;
    case RuntimeCommandType::RequestTerrainLayerPreload:
        renderSystem.RequestTerrainLayerPreload();
        break;
    case RuntimeCommandType::SetTerrainLayerForceMip:
        // 只写 streaming state 的 atomic；M_terrain/feedback 在渲染线程帧首同步。
        renderSystem.RequestTerrainLayerForceMip(command.intValue);
    case RuntimeCommandType::PrintGpuPassStats:
        renderSystem.PrintGpuPassStats();
        break;
    case RuntimeCommandType::PrintPerformanceJson:
        renderSystem.PrintPerformanceJson();
        break;
    case RuntimeCommandType::QuitApplication:
        executionResult.quitRequested = true;
        diagnostics.ReportInfo("Quit requested.");
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
    case RuntimeCommandType::SetTressfxGroupPreview:
    {
        const auto& world = worldManager.GetActiveWorld();
        if (!world || renderSystem.IsOfflineMode()) break;
        if (command.sourceText == "ui" &&
            command.actorProperty.worldGeneration != world->GetGeneration())
        {
            diagnostics.ReportWarning("TressFX group preview rejected for stale World");
            break;
        }
        TressfxGroupPreviewState preview;
        preview.selectedGroup = command.tressfxSelectedGroup;
        preview.groupColors =
            (command.tressfxGroupFlags & TressfxGroupPreviewColors) != 0u;
        preview.solo = (command.tressfxGroupFlags & TressfxGroupPreviewSolo) != 0u;
        world->SetTressfxGroupPreview(preview);
        diagnostics.ReportInfo(
            "TressFX group preview: selected=" +
            std::to_string(preview.selectedGroup) +
            " colors=" + std::to_string(preview.groupColors) +
            " solo=" + std::to_string(preview.solo));
        break;
    }
    case RuntimeCommandType::SetTressfxGroupBindingPreview:
    {
        const auto& world = worldManager.GetActiveWorld();
        if (!world || renderSystem.IsOfflineMode()) break;
        VL::Hair::TressfxGroupBindingPreviewRequest request =
            command.tressfxGroupBindingPreview;
        if (command.sourceText == "ui" &&
            request.worldGeneration != world->GetGeneration())
        {
            diagnostics.ReportWarning(
                "TressFX group binding preview rejected for stale World");
            break;
        }
        std::string rejection;
        if (!VL::Hair::ValidateTressfxGroupBindingPreviewRequest(request, rejection))
        {
            diagnostics.ReportWarning(
                "TressFX group binding preview rejected: " + rejection);
            break;
        }
        renderSystem.RequestTressfxGroupBindingPreview(request);
        diagnostics.ReportInfo(
            "TressFX group binding preview queued: object=" + request.objectName +
            " groups=" + std::to_string(request.groupNames.size()) +
            " revision=" + std::to_string(request.draftRevision));
        break;
    }
    case RuntimeCommandType::SetTressfxBrushPreview:
    {
        const auto& world = worldManager.GetActiveWorld();
        if (!world || renderSystem.IsOfflineMode()) break;
        VL::Hair::TressfxBrushPreviewRequest request = command.tressfxBrushPreview;
        if (command.sourceText == "ui" &&
            request.worldGeneration != world->GetGeneration())
        {
            diagnostics.ReportWarning("TressFX brush preview rejected for stale World");
            break;
        }
        std::string rejection;
        if (!VL::Hair::ValidateTressfxBrushPreviewRequest(request, rejection))
        {
            diagnostics.ReportWarning("TressFX brush preview rejected: " + rejection);
            break;
        }
        // 世界空间圆环属于 active World 的 GT 会话状态：固定 owner/publisher/category/key
        // 让每次 hover 覆盖上一帧，不累积 debug item。退出笔刷或 stamp 无效时显式移除。
        Debug::Style ringStyle;
        ringStyle.owner = "Editor";
        ringStyle.publisher = "TressfxBrush";
        ringStyle.category = "TressfxBrush";
        ringStyle.key = "cursor";
        ringStyle.depth = Debug::DepthMode::DepthTest;
        ringStyle.thickness = 2.0f;
        // 预览状态持续到显式清除；不由 duration 过期，避免鼠标静止时圆环闪烁。
        ringStyle.duration = -1.0f;
        ringStyle.color = request.mode == VL::Hair::GroupPaintMode::Remove
            ? Debug::Color{1.0f, 0.32f, 0.18f, 1.0f}
            : Debug::Color{0.95f, 0.83f, 0.37f, 1.0f};
        if (request.ringVisible)
        {
            const Eigen::Vector3f normal = request.ringNormal.normalized();
            const Eigen::Quaternionf orientation =
                Eigen::Quaternionf::FromTwoVectors(Eigen::Vector3f::UnitY(), normal);
            world->GetDebugDraw().DrawDebugCircle(
                request.ringCenter, request.ringRadiusWorld, orientation, ringStyle);
        }
        else
        {
            world->GetDebugDraw().RemoveKey(
                ringStyle.owner, ringStyle.publisher, ringStyle.category, ringStyle.key);
        }
        // 纯圆环清除没有对象名（切到非头发预览时），不应进入 RT 的对象查找。
        if (!request.objectName.empty())
            renderSystem.RequestTressfxBrushPreview(request);
        // hover 只更新圆环；按鼠标频率打印会淹没诊断输出，只在 ghost 变化时记录。
        if (!request.weights.empty() || !request.ringVisible)
            diagnostics.ReportInfo("TressFX brush preview queued: object=" +
                request.objectName + " target=" + std::to_string(request.targetGroupId) +
                " guides=" + std::to_string(request.weights.size()) +
                " ring=" + std::to_string(request.ringVisible));
        break;
    }
    }
}

} // namespace VL
