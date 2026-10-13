#pragma once

#include <cstdint>
#include <optional>
#include <string>

#include "core/runtimeResult.h"
#include "engine/runtimeCommand.h"
#include "engine/testing/runtimeValidationServices.h"
#include "ui/uiRenderSnapshot.h"
#include "world/worldManager.h"

class RenderSystem;

namespace VL
{

class DiagnosticsSubsystem;
class RuntimeConfig;
class RuntimeTestHooks;
class WorldTransitionCoordinator;
namespace Editor { class SceneDocumentService; }

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
    // 本次加载要并入的场景对象（JSON 对象数组）。模型编辑器用它把打开的模型放进中性预览
    // 环境；普通场景加载这里是空数组。命令只在执行器里**记录意图**，真正的加载发生在
    // 游戏线程的 world/graph 事务里，因此这份数据必须随结果一起携带，而不是就地展开。
    nlohmann::json loadWorldExtraSceneObjects = nlohmann::json::array();
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
    std::optional<OfflineModeCommandFeedback> offlineUiModeFeedback;
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
        const DiagnosticsSubsystem& diagnostics,
        Editor::SceneDocumentService* sceneDocuments = nullptr) const;

private:
    void ExecuteCommand(
        const RuntimeCommand& command,
        RenderSystem& renderSystem,
        WorldManager& worldManager,
        WorldTransitionCoordinator& worldTransitionCoordinator,
        RuntimeTestHooks& runtimeTestHooks,
        const RuntimeConfig& runtimeConfig,
        const DiagnosticsSubsystem& diagnostics,
        RuntimeCommandExecutionResult& executionResult,
        Editor::SceneDocumentService* sceneDocuments) const;
    void ApplyShaderReloadTest(
        const RuntimeConfig& runtimeConfig,
        RuntimeTestHooks& runtimeTestHooks,
        const DiagnosticsSubsystem& diagnostics) const;
    void ApplyShaderComputeReloadTest(
        const RuntimeConfig& runtimeConfig,
        RuntimeTestHooks& runtimeTestHooks,
        const DiagnosticsSubsystem& diagnostics) const;
    void ApplyWorldGraphTransactionTest(
        const RuntimeConfig& runtimeConfig,
        RuntimeTestHooks& runtimeTestHooks,
        const DiagnosticsSubsystem& diagnostics) const;
    void ApplyProceduralSkyParameters(
        const RuntimeCommand& command,
        WorldManager& worldManager,
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
    void ApplySaveCsmSettingsToScene(
        RenderSystem& renderSystem,
        WorldManager& worldManager,
        const DiagnosticsSubsystem& diagnostics) const;
};

} // namespace VL
