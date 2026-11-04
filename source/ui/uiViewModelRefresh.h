#pragma once

#include <chrono>

namespace VL
{

// UI 视图模型的构建档位。档位只由可见性决定，与刷新节拍无关：
// - None：编辑器与运行时 HUD 都不可见（沉浸模式），或 UI 子系统未初始化。
//   此时不构建编辑器快照，只保留 DrawDebug 叠层所需的冻结值。
// - RuntimeHud：只有运行时 HUD 可见（`--no-dev-ui`），只构建场景名与游戏文本。
// - FullEditor：开发者工作区可见，构建完整编辑器快照（仍受下面的节拍限制）。
enum class UiViewModelBuildMode
{
    None,
    RuntimeHud,
    FullEditor
};

// 纯可见性判定，便于在不启动 RmlUi/Vulkan 的测试里验证“隐藏即不构建”。
// developerUiVisible 已经蕴含 developerUiEnabled；runtimeHudVisible 由
// UiSubsystem 按“开发者 UI 与沉浸模式都不可见”计算。
inline UiViewModelBuildMode DecideUiViewModelBuildMode(
    bool initialized,
    bool developerUiVisible,
    bool runtimeHudVisible)
{
    if (!initialized)
    {
        return UiViewModelBuildMode::None;
    }
    if (developerUiVisible)
    {
        return UiViewModelBuildMode::FullEditor;
    }
    return runtimeHudVisible ? UiViewModelBuildMode::RuntimeHud
                             : UiViewModelBuildMode::None;
}

// 可见编辑器快照的定期刷新节拍：100 ms = 10 Hz。
// 输入/选择/世界换代等事件走显式强制刷新（force），不在面板内部比较整份快照。
class UiViewModelRefreshSchedule
{
public:
    using Clock = std::chrono::steady_clock;
    static constexpr std::chrono::milliseconds DefaultInterval{100};

    UiViewModelRefreshSchedule() = default;
    explicit UiViewModelRefreshSchedule(Clock::duration interval)
        : interval(interval)
    {
    }

    // 首次调用即到期；force 来自显式脏路径，绕过剩余等待时间。
    bool IsDue(Clock::time_point now, bool force) const
    {
        return force || now >= nextRefresh;
    }

    // 只在真正构建了一帧完整快照后调用，保证失败/跳过不会提前消耗窗口。
    void MarkRefreshed(Clock::time_point now)
    {
        nextRefresh = now + interval;
    }

    Clock::duration Interval() const { return interval; }

private:
    Clock::duration interval = DefaultInterval;
    // 默认构造的 time_point 是纪元起点，因此第一帧必然到期。
    Clock::time_point nextRefresh = Clock::time_point{};
};

// “隐藏时即使置脏也不构建”：把可见性与节流合成一个纯判定，测试可直接覆盖。
inline bool ShouldBuildEditorViewModel(
    UiViewModelBuildMode mode,
    const UiViewModelRefreshSchedule& schedule,
    std::chrono::steady_clock::time_point now,
    bool force)
{
    return mode == UiViewModelBuildMode::FullEditor && schedule.IsDue(now, force);
}

} // namespace VL
