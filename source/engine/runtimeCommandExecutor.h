#pragma once

#include <cstdint>
#include <optional>
#include <string>

#include "core/runtimeResult.h"
#include "engine/runtimeCommand.h"
#include "engine/runtimeTestHooks.h"
#include "world/worldManager.h"

class RenderSystem;

namespace VL
{

class DiagnosticsSubsystem;
class RuntimeConfig;
class WorldTransitionCoordinator;

struct RuntimeCommandExecutionResult
{
    bool worldChanged = false;
    bool worldRuntimeBindingAttempted = false;
    bool worldRuntimeBindingSucceeded = false;
    bool loadWorldAttempted = false;
    bool loadWorldSucceeded = false;
    std::string loadWorldCommandPath;
    std::string loadWorldResolvedPath;
    std::optional<RuntimeError> loadWorldError;
    WorldHandle loadedWorld;
    WorldHandle activeWorldBeforeCommand;
    WorldHandle activeWorldAfterCommand;
    RuntimeRendererResourceFingerprint rendererResourcesBeforeLoad;
    RuntimeRendererResourceFingerprint rendererResourcesAfterLoad;
};

// Applies queued runtime commands at the owner side of the target systems. This
// keeps DebugConsole and the command layer from directly editing RenderGraph or
// pass material internals.
class RuntimeCommandExecutor
{
public:
    RuntimeCommandExecutionResult ExecuteQueuedCommands(
        CommandBus& commandBus,
        RenderSystem& renderSystem,
        WorldManager& worldManager,
        WorldTransitionCoordinator& worldTransitionCoordinator,
        RuntimeTestHooks& runtimeTestHooks,
        const RuntimeConfig& runtimeConfig,
        const DiagnosticsSubsystem& diagnostics) const;

private:
    void ExecuteCommand(
        const RuntimeCommand& command,
        RenderSystem& renderSystem,
        WorldManager& worldManager,
        WorldTransitionCoordinator& worldTransitionCoordinator,
        RuntimeTestHooks& runtimeTestHooks,
        const RuntimeConfig& runtimeConfig,
        const DiagnosticsSubsystem& diagnostics,
        RuntimeCommandExecutionResult& executionResult) const;
    void ApplyWorldReloadStress(
        const RuntimeCommand& command,
        RuntimeTestHooks& runtimeTestHooks,
        const DiagnosticsSubsystem& diagnostics) const;
    void ApplyWorldReloadFailureRollbackTest(
        const RuntimeCommand& command,
        RuntimeTestHooks& runtimeTestHooks,
        const DiagnosticsSubsystem& diagnostics) const;
    void ApplyGeneratedMaterialFailureRollbackTest(
        const RuntimeConfig& runtimeConfig,
        RuntimeTestHooks& runtimeTestHooks,
        const DiagnosticsSubsystem& diagnostics) const;
    void ApplyGeneratedMeshFailureRollbackTest(
        const RuntimeConfig& runtimeConfig,
        RuntimeTestHooks& runtimeTestHooks,
        const DiagnosticsSubsystem& diagnostics) const;
    void ApplyGeneratedTextureFailureRollbackTest(
        const RuntimeConfig& runtimeConfig,
        RuntimeTestHooks& runtimeTestHooks,
        const DiagnosticsSubsystem& diagnostics) const;
    void ApplyGeneratedHighLightReloadStress(
        const RuntimeCommand& command,
        const RuntimeConfig& runtimeConfig,
        RuntimeTestHooks& runtimeTestHooks,
        const DiagnosticsSubsystem& diagnostics) const;
    void ApplyResizeStress(
        const RuntimeCommand& command,
        RuntimeTestHooks& runtimeTestHooks,
        const DiagnosticsSubsystem& diagnostics) const;
    void ApplyRenderGraphReloadStress(
        const RuntimeCommand& command,
        RuntimeTestHooks& runtimeTestHooks,
        const DiagnosticsSubsystem& diagnostics) const;
    void ApplyFrameSmokeTest(
        const RuntimeCommand& command,
        RuntimeTestHooks& runtimeTestHooks,
        const DiagnosticsSubsystem& diagnostics) const;
    void ApplyEnvironmentUpdateStress(
        const RuntimeCommand& command,
        RuntimeTestHooks& runtimeTestHooks,
        const DiagnosticsSubsystem& diagnostics) const;
    void ApplyProceduralSkyParameters(
        const RuntimeCommand& command,
        WorldManager& worldManager,
        RuntimeTestHooks& runtimeTestHooks,
        const DiagnosticsSubsystem& diagnostics) const;
    void ApplyLoadWorld(
        const RuntimeCommand& command,
        WorldTransitionCoordinator& worldTransitionCoordinator,
        const RuntimeConfig& runtimeConfig,
        const DiagnosticsSubsystem& diagnostics,
        RuntimeCommandExecutionResult& executionResult) const;
    void ApplyToneMappingMode(
        int mode,
        RenderSystem& renderSystem,
        const DiagnosticsSubsystem& diagnostics) const;
    void ApplyBloomParameter(
        BloomParameter parameter,
        float value,
        RenderSystem& renderSystem,
        const DiagnosticsSubsystem& diagnostics) const;
};

} // namespace VL
