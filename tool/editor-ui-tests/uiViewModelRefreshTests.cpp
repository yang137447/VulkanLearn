#include <gtest/gtest.h>

#include <chrono>

#include "ui/uiViewModelRefresh.h"

namespace
{

using VL::DecideUiViewModelBuildMode;
using VL::ShouldBuildEditorViewModel;
using VL::UiViewModelBuildMode;
using VL::UiViewModelRefreshSchedule;

using Clock = UiViewModelRefreshSchedule::Clock;
using std::chrono::milliseconds;

// 可见性决策是隐藏/节流修复的核心：编辑器不可见时不允许构建完整快照。
TEST(UiViewModelVisibilityTest, UninitializedSubsystemBuildsNothing)
{
    EXPECT_EQ(DecideUiViewModelBuildMode(false, false, false), UiViewModelBuildMode::None);
    EXPECT_EQ(DecideUiViewModelBuildMode(false, true, true), UiViewModelBuildMode::None);
}

TEST(UiViewModelVisibilityTest, ImmersiveModeSkipsEditorAndRuntimeHud)
{
    // 沉浸模式：开发者 UI 与 HUD 都隐藏，连 HUD 视图都不构建。
    EXPECT_EQ(DecideUiViewModelBuildMode(true, false, false), UiViewModelBuildMode::None);
}

TEST(UiViewModelVisibilityTest, NoDeveloperUiBuildsRuntimeHudOnly)
{
    // `--no-dev-ui`：只有运行时 HUD 可见，不构建编辑器快照。
    EXPECT_EQ(DecideUiViewModelBuildMode(true, false, true), UiViewModelBuildMode::RuntimeHud);
}

TEST(UiViewModelVisibilityTest, VisibleWorkspaceBuildsFullEditorSnapshot)
{
    EXPECT_EQ(DecideUiViewModelBuildMode(true, true, false), UiViewModelBuildMode::FullEditor);
}

TEST(UiViewModelVisibilityTest, VisibleWorkspaceNeverBuildsHudOnlyMode)
{
    // HUD 在编辑器可见时由 ApplyDocumentState 隐藏；档位必须是完整编辑器。
    EXPECT_EQ(DecideUiViewModelBuildMode(true, true, true), UiViewModelBuildMode::FullEditor);
}

TEST(UiViewModelRefreshTest, FirstFrameIsDue)
{
    UiViewModelRefreshSchedule schedule;
    EXPECT_TRUE(schedule.IsDue(Clock::time_point{}, false));
}

TEST(UiViewModelRefreshTest, WaitsUntilIntervalElapses)
{
    UiViewModelRefreshSchedule schedule(milliseconds(100));
    const Clock::time_point start = Clock::time_point{} + milliseconds(1000);
    schedule.MarkRefreshed(start);
    EXPECT_FALSE(schedule.IsDue(start + milliseconds(99), false));
    EXPECT_TRUE(schedule.IsDue(start + milliseconds(100), false));
}

TEST(UiViewModelRefreshTest, EventDirtyFlagForcesRefreshBeforeInterval)
{
    UiViewModelRefreshSchedule schedule(milliseconds(100));
    const Clock::time_point start = Clock::time_point{} + milliseconds(1000);
    schedule.MarkRefreshed(start);
    // 选中/模式切换等显式脏路径不受剩余等待时间限制。
    EXPECT_TRUE(schedule.IsDue(start, true));
    EXPECT_TRUE(schedule.IsDue(start + milliseconds(1), true));
}

TEST(UiViewModelRefreshTest, MarkRefreshedRestartsTheWholeWindow)
{
    UiViewModelRefreshSchedule schedule(milliseconds(100));
    const Clock::time_point start = Clock::time_point{} + milliseconds(1000);
    schedule.MarkRefreshed(start);
    EXPECT_TRUE(schedule.IsDue(start + milliseconds(100), false));
    schedule.MarkRefreshed(start + milliseconds(100));
    EXPECT_FALSE(schedule.IsDue(start + milliseconds(150), false));
    EXPECT_TRUE(schedule.IsDue(start + milliseconds(200), false));
}

TEST(UiViewModelRefreshTest, CustomIntervalControlsCadence)
{
    // 15 Hz 也是允许的节拍：66 ms 内不刷新，67 ms 起刷新。
    UiViewModelRefreshSchedule schedule(milliseconds(67));
    const Clock::time_point start = Clock::time_point{} + milliseconds(1000);
    schedule.MarkRefreshed(start);
    EXPECT_FALSE(schedule.IsDue(start + milliseconds(66), false));
    EXPECT_TRUE(schedule.IsDue(start + milliseconds(67), false));
}

TEST(UiViewModelRefreshTest, HiddenUiNeverBuildsEvenWhenForced)
{
    UiViewModelRefreshSchedule schedule;
    const Clock::time_point now = Clock::time_point{} + milliseconds(1000);
    // 隐藏状态即使收到脏事件也不能构建完整编辑器快照。
    EXPECT_FALSE(ShouldBuildEditorViewModel(
        UiViewModelBuildMode::None, schedule, now, true));
    EXPECT_FALSE(ShouldBuildEditorViewModel(
        UiViewModelBuildMode::RuntimeHud, schedule, now, true));
    // 只有工作区可见时才考虑节流/强制刷新。
    EXPECT_TRUE(ShouldBuildEditorViewModel(
        UiViewModelBuildMode::FullEditor, schedule, now, true));
}

} // namespace
