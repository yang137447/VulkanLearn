#pragma once

#include <cstdint>
#include <string>
#include <unordered_map>

#include "engine/runtimeCommand.h"
#include "world/worldManager.h"

class RenderSystem;

namespace VL
{

class DiagnosticsSubsystem;
class RuntimeConfig;
class RuntimeTestHooks;
class WorldTransitionCoordinator;

// Lightweight identity snapshot used by runtime rollback tests. It records the
// renderer binding tables without holding resources alive or exposing backend
// objects to the command layer.
struct RuntimeRendererResourceFingerprint
{
    bool captured = false;
    uint64_t worldOwnerGeneration = 0;
    std::unordered_map<std::string, std::uintptr_t> worldTextures;
    std::unordered_map<std::string, std::uintptr_t> renderableObjects;
    std::unordered_map<std::string, std::uintptr_t> materials;
    std::unordered_map<std::string, std::uintptr_t> materialInstances;
    std::unordered_map<std::string, std::uintptr_t> objectResources;
    std::unordered_map<std::string, std::uintptr_t> sceneObjects;
    std::unordered_map<std::string, std::uintptr_t> textures;
    std::unordered_map<std::string, std::uintptr_t> passMaterialInstances;
};

struct RuntimeCommandExecutionResult
{
    bool worldChanged = false;
    bool worldRuntimeBindingAttempted = false;
    bool worldRuntimeBindingSucceeded = false;
    bool loadWorldAttempted = false;
    bool loadWorldSucceeded = false;
    std::string loadWorldCommandPath;
    std::string loadWorldResolvedPath;
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
        WorldTransitionCoordinator& worldTransitionCoordinator,
        RuntimeTestHooks& runtimeTestHooks,
        const RuntimeConfig& runtimeConfig,
        const DiagnosticsSubsystem& diagnostics) const;

private:
    void ExecuteCommand(
        const RuntimeCommand& command,
        RenderSystem& renderSystem,
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
    void ApplyGeneratedHighLightReloadStress(
        const RuntimeCommand& command,
        const RuntimeConfig& runtimeConfig,
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
