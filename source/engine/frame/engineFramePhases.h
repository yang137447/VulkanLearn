#pragma once

#include <cstdint>
#include <functional>
#include <vector>

#include "engine/runtimeCommand.h"
#include "engine/script/engineScriptBuild.h"
#include "engine/ui/engineUiActions.h"
#include "engine/window/engineWindowLifecycle.h"
#include "platform/platformEvent.h"

class Controller;

namespace VL
{

class AudioRuntime;
class CpuFrameProfiler;
class DestructibleRuntimeRegistry;
class IEditorHost;
class ModelPreviewCamera;
class PlatformWindow;
class ProjectileRuntime;
class RenderThread;
class RendererBackendVulkan;
class RuntimeCommandExecutor;
class RuntimeConfig;
class RuntimeValidationServices;
class SubsystemCollection;
class TerrainChunkStreamer;
class UiSubsystem;
namespace Physics { class FractureImpactWorld; }
namespace Physics { namespace Fracture { class FractureBudget; } }
struct RuntimeCommandExecutionResult;

// 仍留在 EngineLoop 上的帧级动作；用回调注入，避免 frame 模块反向依赖
// EngineLoop 的私有成员。
struct EngineFrameCallbacks
{
    std::function<void(RuntimeCommandExecutionResult&)>
        processRequestedWorldTransition;
    std::function<void(const RuntimeCommandExecutionResult&)>
        processShaderRuntimeRequests;
    std::function<void()> processAutomaticShaderReloads;
    std::function<void(float, const RuntimeCommandExecutionResult&)>
        updateUiViewModel;
    std::function<void()> synchronizeSceneViewport;
    std::function<void(RuntimeCommand)> queueRuntimeCommand;
    std::function<void()> tickPerformanceJsonl;
    std::function<void()> tickUiSectionStats;
};

// GT 帧循环的显式状态视图。数据字段全部指向 EngineLoop 现有成员；
// uiActions/scriptBuild/windowLifecycle 是同一份状态的模块视图，按帧重建。
struct EngineFrameContext
{
    SubsystemCollection* subsystems = nullptr;
    RuntimeCommandExecutor* runtimeCommandExecutor = nullptr;
    RuntimeValidationServices* runtimeValidationServices = nullptr;
    IEditorHost* editorHost = nullptr;
    UiSubsystem* uiSubsystem = nullptr;
    ModelPreviewCamera* modelPreviewCamera = nullptr;
    RenderThread* renderThread = nullptr;
    TerrainChunkStreamer* terrainChunkStreamer = nullptr;
    RendererBackendVulkan* rendererBackend = nullptr;
    PlatformWindow* window = nullptr;
    AudioRuntime* audioRuntime = nullptr;
    DestructibleRuntimeRegistry* destructibleRuntimes = nullptr;
    Physics::FractureImpactWorld* fractureWorld = nullptr;
    Physics::Fracture::FractureBudget* fractureBudget = nullptr;
    ProjectileRuntime* projectileRuntime = nullptr;
    Controller* controller = nullptr;
    CpuFrameProfiler* cpuFrameProfiler = nullptr;
    const RuntimeConfig* runtimeConfig = nullptr;
    std::vector<PlatformEvent>* platformEvents = nullptr;
    uint64_t* autoPlayWorldGeneration = nullptr;
    bool* destructibleSessionActive = nullptr;
    bool* shouldClose = nullptr;
    int* exitCode = nullptr;
    const bool* exitAfterRuntimeTests = nullptr;

    EngineUiActionContext uiActions;
    EngineScriptBuildContext scriptBuild;
    EngineWindowLifecycleContext windowLifecycle;
    EngineFrameCallbacks callbacks;
};

// 一帧 GT：UI 动作 -> 命令 -> 世界/着色器事务 -> 平台事件 -> Update ->
// UI 视图模型 -> 地形流 -> 发布/录制/提交 -> CPU 阶段快照 -> FPS/调试帧边界。
// 帧顺序与各安全点位置与拆分前逐行一致。
void TickEngineFrame(EngineFrameContext& context);

} // namespace VL
