#pragma once

#include <cstdint>
#include <optional>
#include <string>

#include "core/runtimeResult.h"
#include "engine/runtimeCommand.h"
#include "engine/testing/runtimeValidationServices.h"
#include "world/worldManager.h"

class RenderSystem;

namespace VL
{

class DiagnosticsSubsystem;
class RuntimeConfig;
class RuntimeTestHooks;
class WorldTransitionCoordinator;

struct RuntimeCommandExecutionResult
{
    bool worldChanged = false;
    // The transactional World path only publishes runtime binding after every
    // prepare step succeeds, so this records entry into the no-throw live
    // ownership commit rather than candidate preparation.
    bool worldRuntimeBindingAttempted = false;
    bool worldRuntimeBindingSucceeded = false;
    bool loadWorldAttempted = false;
    bool worldLoadRequested = false;
    bool loadWorldSucceeded = false;
    std::string loadWorldCommandPath;
    std::string loadWorldResolvedPath;
    std::optional<RuntimeError> loadWorldError;
    WorldHandle loadedWorld;
    WorldHandle activeWorldBeforeCommand;
    WorldHandle activeWorldAfterCommand;
    RuntimeRendererResourceFingerprint rendererResourcesBeforeLoad;
    RuntimeRendererResourceFingerprint rendererResourcesAfterLoad;
    bool shaderReloadRequested = false;
    RuntimeShaderReloadScope shaderReloadScope =
        RuntimeShaderReloadScope::Changed;
    bool shaderCacheStatisticsRequested = false;
};

RuntimeRendererResourceFingerprint
CaptureRuntimeRendererResourceFingerprint();

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
    void ApplyShaderReloadTest(
        const RuntimeConfig& runtimeConfig,
        RuntimeTestHooks& runtimeTestHooks,
        const DiagnosticsSubsystem& diagnostics) const;
    void ApplyShaderAutoReloadTest(
        const RuntimeConfig& runtimeConfig,
        RuntimeTestHooks& runtimeTestHooks,
        const DiagnosticsSubsystem& diagnostics) const;
    void ApplyShaderComputeReloadTest(
        const RuntimeConfig& runtimeConfig,
        RuntimeTestHooks& runtimeTestHooks,
        const DiagnosticsSubsystem& diagnostics) const;
    void ApplyShaderDefinitionReloadTest(
        const RuntimeConfig& runtimeConfig,
        RuntimeTestHooks& runtimeTestHooks,
        const DiagnosticsSubsystem& diagnostics) const;
    void ApplyWorldGraphTransactionTest(
        const RuntimeConfig& runtimeConfig,
        RuntimeTestHooks& runtimeTestHooks,
        const DiagnosticsSubsystem& diagnostics) const;
    void ApplyShaderUiReloadTest(
        const RuntimeConfig& runtimeConfig,
        RuntimeTestHooks& runtimeTestHooks,
        const DiagnosticsSubsystem& diagnostics) const;
    void ApplyShaderShutdownInflightTest(
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
