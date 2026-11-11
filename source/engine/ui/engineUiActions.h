#pragma once

#include <cstdint>
#include <filesystem>
#include <optional>
#include <vector>

#include "platform/platformEvent.h"

class Controller;

namespace VL
{

class IEditorHost;
class ModelPreviewCamera;
class PlatformApplication;
class PlatformWindow;
class ScriptBuildService;
class SubsystemCollection;
class UiSubsystem;
struct EngineScriptBuildContext;
struct EngineWindowLifecycleContext;

// EngineLoop 的 UI 动作/输入策略/平台事件状态视图。字段全部指向 EngineLoop
// 现有成员；模块不缓存 UI、输入或平台窗口状态。
struct EngineUiActionContext
{
    SubsystemCollection* subsystems = nullptr;
    IEditorHost* editorHost = nullptr;
    UiSubsystem* uiSubsystem = nullptr;
    Controller* controller = nullptr;
    ModelPreviewCamera* modelPreviewCamera = nullptr;
    ScriptBuildService* scriptBuild = nullptr;
    PlatformApplication* platformApplication = nullptr;
    PlatformWindow* window = nullptr;
    std::vector<PlatformEvent>* platformEvents = nullptr;
    bool* autoCompileScripts = nullptr;
    std::optional<std::filesystem::path>* pendingBuiltAssembly = nullptr;
    bool* shouldClose = nullptr;
    int* exitCode = nullptr;
};

// 排空 CommandBus 的 UiAction 队列：编辑器 host 优先，其后是运行时动作，
// 剩余动作翻译成 RuntimeCommand 回到队列。每帧在命令执行前调用一次。
void ApplyQueuedEngineUiActions(
    const EngineUiActionContext& context,
    const EngineScriptBuildContext& scriptBuildContext);

// 依据 UI 可见性/脚本状态仲裁键盘、指针与相对鼠标模式。
void UpdateEngineUiInputPolicy(const EngineUiActionContext& context);

// 轮询平台事件：Esc/捡选/UI 分发/窗口尺寸/退出；需要重建 swapchain 时
// 走 window lifecycle 的安全点。
void PumpEnginePlatformEvents(
    const EngineUiActionContext& context,
    const EngineWindowLifecycleContext& windowLifecycle);

} // namespace VL
