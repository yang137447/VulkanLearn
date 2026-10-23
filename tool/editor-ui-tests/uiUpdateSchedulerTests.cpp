#include <gtest/gtest.h>

#include <chrono>
#include <memory>

#include "ui/uiRenderSnapshotQueue.h"
#include "ui/uiUpdateScheduler.h"

namespace
{

using VL::UiMutationKind;
using VL::UiRenderSnapshot;
using VL::UiRenderSnapshotPublisher;
using VL::UiUpdateDecision;
using VL::UiUpdateFallbackInterval;
using VL::UiUpdateScheduler;
using VL::UiUpdateSurface;

using Clock = UiUpdateScheduler::Clock;
using std::chrono::milliseconds;

const Clock::time_point Start = Clock::time_point{} + milliseconds(1000);

// 可见、无输入、无分节变化、未到兜底节拍：这一帧必须什么都不做。
TEST(UiUpdateSchedulerTest, IdleFrameSkipsUpdateAndPublish)
{
    UiUpdateScheduler scheduler;
    const UiUpdateDecision decision =
        scheduler.Evaluate(UiUpdateSurface::Workspace, false, false, Start);
    ASSERT_TRUE(decision.updateRml);
    scheduler.MarkUpdated(UiUpdateSurface::Workspace, Start);

    const UiUpdateDecision idle =
        scheduler.Evaluate(UiUpdateSurface::Workspace, false, false, Start + milliseconds(50));
    EXPECT_FALSE(idle.updateRml);
    EXPECT_FALSE(idle.publishSnapshot);
}

// 分节版本变化意味着绑定/面板数据换了，必须在同一帧重跑上下文更新。
TEST(UiUpdateSchedulerTest, SectionChangeForcesUpdate)
{
    UiUpdateScheduler scheduler;
    scheduler.MarkUpdated(UiUpdateSurface::Workspace, Start);
    const UiUpdateDecision decision =
        scheduler.Evaluate(UiUpdateSurface::Workspace, true, false, Start + milliseconds(1));
    EXPECT_TRUE(decision.updateRml);
    EXPECT_TRUE(decision.publishSnapshot);
}

// 指针/按键/滚轮/文本事件可能改变 hover/pressed/focus：必须立即更新，
// 否则跳过帧会留下过期的悬停或光标反馈。
TEST(UiUpdateSchedulerTest, InputEventForcesUpdate)
{
    UiUpdateScheduler scheduler;
    scheduler.MarkUpdated(UiUpdateSurface::Workspace, Start);
    scheduler.MarkInputEvent();
    const UiUpdateDecision decision =
        scheduler.Evaluate(UiUpdateSurface::Workspace, false, false, Start + milliseconds(1));
    EXPECT_TRUE(decision.updateRml);
    EXPECT_TRUE(decision.publishSnapshot);
}

// 面板构建、文档 show/hide、热重载、缩放、本地化都归类为 DOM 突变。
TEST(UiUpdateSchedulerTest, DomMutationForcesUpdate)
{
    for (const UiMutationKind kind : {UiMutationKind::PanelBuild,
             UiMutationKind::DocumentVisibility, UiMutationKind::HotReload,
             UiMutationKind::Resize, UiMutationKind::WorldChange})
    {
        UiUpdateScheduler scheduler;
        scheduler.MarkUpdated(UiUpdateSurface::Workspace, Start);
        scheduler.MarkMutation(kind);
        const UiUpdateDecision decision =
            scheduler.Evaluate(UiUpdateSurface::Workspace, false, false, Start + milliseconds(1));
        EXPECT_TRUE(decision.updateRml) << "kind=" << static_cast<int>(kind);
        EXPECT_TRUE(decision.publishSnapshot) << "kind=" << static_cast<int>(kind);
    }
}

// 标签切换是面板本地 DOM 突变：即使数据分节没变也要更新。
TEST(UiUpdateSchedulerTest, TabSwitchForcesUpdate)
{
    UiUpdateScheduler scheduler;
    scheduler.MarkUpdated(UiUpdateSurface::Workspace, Start);
    scheduler.MarkMutation(UiMutationKind::TabSwitch);
    const UiUpdateDecision decision =
        scheduler.Evaluate(UiUpdateSurface::Workspace, false, false, Start + milliseconds(1));
    EXPECT_TRUE(decision.updateRml);
    EXPECT_TRUE(decision.publishSnapshot);
}

// 语言提交重写全部静态文本。
TEST(UiUpdateSchedulerTest, LocaleCommitForcesUpdate)
{
    UiUpdateScheduler scheduler;
    scheduler.MarkUpdated(UiUpdateSurface::Workspace, Start);
    scheduler.MarkMutation(UiMutationKind::LocaleCommit);
    const UiUpdateDecision decision =
        scheduler.Evaluate(UiUpdateSurface::Workspace, false, false, Start + milliseconds(1));
    EXPECT_TRUE(decision.updateRml);
    EXPECT_TRUE(decision.publishSnapshot);
}

// 兜底节拍：10 Hz。FPS/性能数字每 100 ms 才换样本，UI 也没有 RCSS 过渡动画。
TEST(UiUpdateSchedulerTest, CadenceElapsedForcesUpdate)
{
    EXPECT_EQ(UiUpdateFallbackInterval, milliseconds(100));
    UiUpdateScheduler scheduler;
    scheduler.MarkUpdated(UiUpdateSurface::Workspace, Start);

    const UiUpdateDecision before =
        scheduler.Evaluate(UiUpdateSurface::Workspace, false, false,
            Start + UiUpdateFallbackInterval - milliseconds(1));
    EXPECT_FALSE(before.updateRml);

    const UiUpdateDecision due =
        scheduler.Evaluate(UiUpdateSurface::Workspace, false, false,
            Start + UiUpdateFallbackInterval);
    EXPECT_TRUE(due.updateRml);
    EXPECT_TRUE(due.publishSnapshot);
}

// 两个文档都隐藏时不推进 RmlUi；脏标记保留到重新可见的第一帧。
TEST(UiUpdateSchedulerTest, HiddenNeverUpdatesRegardlessOfInputs)
{
    UiUpdateScheduler scheduler;
    scheduler.MarkUpdated(UiUpdateSurface::Workspace, Start);
    scheduler.MarkHidden();
    scheduler.MarkInputEvent();
    scheduler.MarkMutation(UiMutationKind::Resize);
    const UiUpdateDecision decision = scheduler.Evaluate(
        UiUpdateSurface::Hidden, true, true, Start + milliseconds(5000));
    EXPECT_FALSE(decision.updateRml);
    EXPECT_FALSE(decision.publishSnapshot);
    // 隐藏不消费标记：重新可见时仍会强制更新。
    EXPECT_TRUE(scheduler.HasPendingInput());
    EXPECT_TRUE(scheduler.HasPendingMutation());
}

// 隐藏→可见的第一帧必须完整更新并发布完整快照，即使没有任何事件。
TEST(UiUpdateSchedulerTest, FirstVisibleFrameAfterHiddenForcesFullUpdate)
{
    UiUpdateScheduler scheduler;
    scheduler.MarkHidden();
    const UiUpdateDecision decision =
        scheduler.Evaluate(UiUpdateSurface::Workspace, false, false, Start);
    EXPECT_TRUE(decision.updateRml);
    EXPECT_TRUE(decision.publishSnapshot);
}

// 工作区与 HUD 共用同一个 RmlUi Context，表面切换也要强制更新。
TEST(UiUpdateSchedulerTest, SurfaceSwitchForcesUpdate)
{
    UiUpdateScheduler scheduler;
    scheduler.MarkUpdated(UiUpdateSurface::RuntimeHud, Start);
    const UiUpdateDecision decision =
        scheduler.Evaluate(UiUpdateSurface::Workspace, false, false, Start + milliseconds(1));
    EXPECT_TRUE(decision.updateRml);
    EXPECT_TRUE(decision.publishSnapshot);
}

// DrawDebug 几何逐帧变化：允许只重发快照而不推进 RmlUi 上下文。
TEST(UiUpdateSchedulerTest, AnimatedOverlayPublishesWithoutRmlUpdate)
{
    UiUpdateScheduler scheduler;
    scheduler.MarkUpdated(UiUpdateSurface::Workspace, Start);
    const UiUpdateDecision decision =
        scheduler.Evaluate(UiUpdateSurface::Workspace, false, true, Start + milliseconds(1));
    EXPECT_FALSE(decision.updateRml);
    EXPECT_TRUE(decision.publishSnapshot);
}

// MarkUpdated 消费置脏标记：一次更新后，下一帧立即回到空闲。
TEST(UiUpdateSchedulerTest, MarkUpdatedConsumesPendingFlags)
{
    UiUpdateScheduler scheduler;
    scheduler.MarkInputEvent();
    scheduler.MarkMutation(UiMutationKind::PanelBuild);
    EXPECT_TRUE(scheduler.Evaluate(
        UiUpdateSurface::Workspace, false, false, Start).updateRml);
    scheduler.MarkUpdated(UiUpdateSurface::Workspace, Start);
    EXPECT_FALSE(scheduler.HasPendingInput());
    EXPECT_FALSE(scheduler.HasPendingMutation());
    EXPECT_FALSE(scheduler.Evaluate(
        UiUpdateSurface::Workspace, false, false, Start + milliseconds(1)).updateRml);
}

// 跳过发布必须保留上一份不可变快照的同一身份：RT 继续绘制同样的几何。
TEST(UiRenderSnapshotPublicationTest, SkippedUpdateRetainsLastPublishedIdentity)
{
    UiRenderSnapshotPublisher publisher;
    auto full = std::make_shared<UiRenderSnapshot>();
    full->vertices.resize(4);
    full->indices = {0, 1, 2, 0, 2, 3};
    publisher.Publish(full);

    // RT 消费后队列为空，但 RT 与 publisher 都仍持有同一份不可变快照。
    const std::shared_ptr<const UiRenderSnapshot> consumed = publisher.Queue().ConsumeLatest();
    ASSERT_NE(consumed, nullptr);
    EXPECT_EQ(consumed.get(), full.get());

    publisher.Skip();
    EXPECT_EQ(publisher.Queue().ConsumeLatest(), nullptr);
    EXPECT_EQ(publisher.LastPublished().get(), full.get());
    EXPECT_EQ(publisher.PublishedCount(), 1u);
    EXPECT_EQ(publisher.SkippedCount(), 1u);
}

// 隐藏→可见的第一帧：调度器要求发布，发布出的就是完整快照。
TEST(UiRenderSnapshotPublicationTest, FirstVisibleFrameAfterHiddenPublishesFullSnapshot)
{
    UiUpdateScheduler scheduler;
    UiRenderSnapshotPublisher publisher;

    scheduler.MarkHidden();
    const UiUpdateDecision decision =
        scheduler.Evaluate(UiUpdateSurface::Workspace, false, false, Start);
    ASSERT_TRUE(decision.publishSnapshot);

    auto full = std::make_shared<UiRenderSnapshot>();
    full->vertices.resize(8);
    full->sceneViewportEnabled = true;
    if (decision.publishSnapshot)
    {
        publisher.Publish(full);
    }
    else
    {
        publisher.Skip();
    }
    EXPECT_EQ(publisher.LastPublished().get(), full.get());
    const std::shared_ptr<const UiRenderSnapshot> consumed = publisher.Queue().ConsumeLatest();
    ASSERT_NE(consumed, nullptr);
    EXPECT_EQ(consumed->vertices.size(), 8u);
    EXPECT_TRUE(consumed->sceneViewportEnabled);
    scheduler.MarkUpdated(UiUpdateSurface::Workspace, Start);
}

} // namespace
