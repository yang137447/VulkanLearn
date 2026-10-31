#pragma once

#include <chrono>
#include <cstdint>

namespace VL
{

// 可见的 RmlUi 表面。运行时 HUD 与编辑器工作区共用同一个 RmlUi Context：
// `rmlContext->Update()` 会一起推进两个文档，因此不存在"只更新 HUD"的路径。
// 实际可见的永远只有其中一个（ApplyDocumentState 互斥显示），调度按当前表面统一判定。
enum class UiUpdateSurface
{
    Hidden = 0,
    RuntimeHud,
    Workspace
};

// 兜底节拍 10 Hz（与 EngineLoop 的 100 ms 视图模型窗口一致）。选择依据：
// - FPS/性能数字最快每 100 ms 换一次样本，更快的上下文更新没有新内容可显示；
// - UI 的 RCSS 没有 transition/@keyframes，不存在需要逐帧推进的过渡动画；
// - 运行 HUD 文本由脚本写入，绑定变化比较已经能逐次触发更新，100 ms 只是兜底，
//   避免任何未被事件覆盖的绑定在静止时永久停在旧值。
inline constexpr std::chrono::milliseconds UiUpdateFallbackInterval{100};

// 一次帧内更新的两个动作。绝大多数情况下两者同时发生；只有 DrawDebug 几何
// 逐帧变化时允许"只重发快照、不推进 RmlUi 上下文"。
struct UiUpdateDecision
{
    bool updateRml = false;
    bool publishSnapshot = false;
};

// 强制更新的来源。分类只为诊断/测试可读，所有来源都走同一条判定。
enum class UiMutationKind
{
    PanelBuild = 0,
    TabSwitch,
    DocumentVisibility,
    LocaleCommit,
    HotReload,
    Resize,
    WorldChange,
    ForcedRefresh
};

// 纯判定 + 少量时间状态：回答"这一帧要不要跑 rmlContext->Update()、要不要重发快照"。
// 输入事件和 DOM 突变由事件路径置脏，判定本身不读 DOM、不轮询控件状态。
class UiUpdateScheduler
{
public:
    using Clock = std::chrono::steady_clock;

    // 有任何输入交给 RmlUi（指针移动/按下/释放、滚轮、按键、文本、手柄）时调用。
    // 指针移动必须置脏：RmlUi 内部 hover 状态可能在鼠标离开控件的那一帧才清除，
    // 跳过该帧会留下错误的悬停/光标反馈。
    void MarkInputEvent() noexcept { inputPending = true; }

    // DOM 或绑定内容已变化：面板 Build、标签切换、文档 show/hide、热重载、缩放、
    // 本地化、World 换代、绑定文本变化。
    void MarkMutation(UiMutationKind kind) noexcept
    {
        lastMutationKind = kind;
        domMutationPending = true;
    }

    // 两个文档都不再显示时由 Update 的 None 档位调用。记录"隐藏"状态，使重新
    // 可见的第一帧必然 surfaceChanged，从而强制一次完整更新与完整快照发布。
    void MarkHidden() noexcept { lastSurface = UiUpdateSurface::Hidden; }

    // 纯判定，不修改内部状态；测试可直接驱动任意时间点。
    UiUpdateDecision Evaluate(
        UiUpdateSurface surface,
        bool sectionChanged,
        bool overlayAnimated,
        Clock::time_point now) const noexcept
    {
        UiUpdateDecision decision;
        if (surface == UiUpdateSurface::Hidden)
        {
            // 隐藏帧不推进 RmlUi；调用方仍按既有契约发布空快照。
            return decision;
        }

        const bool surfaceChanged = surface != lastSurface;
        const bool cadenceElapsed = !hasUpdated ||
            now - lastUpdate >= UiUpdateFallbackInterval;
        decision.updateRml = surfaceChanged || sectionChanged || inputPending ||
            domMutationPending || cadenceElapsed;
        // DrawDebug 几何不经过 RmlUi：上下文可以不动，但快照必须重发，
        // 否则过期图元会留在 RT 的上一份快照里。
        decision.publishSnapshot = decision.updateRml || overlayAnimated;
        return decision;
    }

    // 只在真正执行过 rmlContext->Update() 后调用：推进节拍窗口并消费置脏标记。
    void MarkUpdated(UiUpdateSurface surface, Clock::time_point now) noexcept
    {
        lastSurface = surface;
        lastUpdate = now;
        hasUpdated = true;
        inputPending = false;
        domMutationPending = false;
    }

    bool HasPendingInput() const noexcept { return inputPending; }
    bool HasPendingMutation() const noexcept { return domMutationPending; }
    UiMutationKind LastMutationKind() const noexcept { return lastMutationKind; }

private:
    Clock::time_point lastUpdate{};
    UiUpdateSurface lastSurface = UiUpdateSurface::Hidden;
    UiMutationKind lastMutationKind = UiMutationKind::ForcedRefresh;
    // 默认未更新：首次可见帧即使没有任何事件也必须完整更新一次。
    bool hasUpdated = false;
    bool inputPending = false;
    bool domMutationPending = false;
};

} // namespace VL
