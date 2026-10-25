#pragma once

#include <chrono>
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
class InputSubsystem;
class RuntimeConfig;
class RuntimeTestHooks;
class WorldTransitionCoordinator;
class ScriptRuntimeSubsystem;
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
        InputSubsystem* input = nullptr,
        ScriptRuntimeSubsystem* scripts = nullptr,
        Editor::SceneDocumentService* sceneDocuments = nullptr) const;

private:
    // `mesh sweep` 的每帧状态：显式命令启动，GT 每帧调用 ExecuteQueuedCommands 时
    // 推进一次，到时或 actor 消失就停。它不是每帧默认行为，只有命令能激活。
    // 用 steady_clock 而不是帧计数，避免 30/60/120 Hz 下位移速度不同。
    struct MeshSweepState
    {
        bool active = false;
        std::string identity;
        std::string secondIdentity;
        Eigen::Vector3f startPosition = Eigen::Vector3f::Zero();
        Eigen::Vector3f secondStartPosition = Eigen::Vector3f::Zero();
        Eigen::Vector3f rotation = Eigen::Vector3f::Zero();
        Eigen::Vector3f secondRotation = Eigen::Vector3f::Zero();
        Eigen::Vector3f scale = Eigen::Vector3f::Ones();
        Eigen::Vector3f secondScale = Eigen::Vector3f::Ones();
        Eigen::Vector3f delta = Eigen::Vector3f::Zero();
        float seconds = 0.0f;
        std::chrono::steady_clock::time_point started;
    };

    void ExecuteCommand(
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
    // placement 命令执行端：只有这里写 World 的 mesh 根 TRS，也只在 GT 命令边界调用。
    void ApplyMeshPlacement(
        const RuntimeCommand& command,
        WorldManager& worldManager,
        const DiagnosticsSubsystem& diagnostics) const;
    // 每个 GT 帧推进一次已激活的 sweep；未激活时零开销。
    void TickMeshSweep(
        WorldManager& worldManager,
        const DiagnosticsSubsystem& diagnostics) const;
    void CancelMeshSweep(const DiagnosticsSubsystem& diagnostics) const;

    mutable MeshSweepState meshSweep;
};

} // namespace VL
