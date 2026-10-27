#include "hair/tressfxSimulationHistory.h"
#include "hair/tressfxSimulationParams.h"
#include "hair/tressfxSourceSchedule.h"

#include <gtest/gtest.h>

namespace
{
using namespace VL::Hair;
}

TEST(TressfxSimulationHistory, FirstValidPoseResetsEvenWhenPaused)
{
    GuideSimulationHistory history;
    const GuideSimulationFramePlan plan = history.Plan(true, 7, 0, 0.0f);
    EXPECT_EQ(plan.action, GuideSimulationFrameAction::Reset);
    EXPECT_EQ(plan.reason, GuideSimulationResetReason::FirstPose);
}

TEST(TressfxSimulationHistory, UnskinnedInitialStateResetsOnlyOnce)
{
    GuideSimulationHistory history;
    EXPECT_EQ(history.Plan(false, 0, 0, 0.0f).reason,
        GuideSimulationResetReason::InitialState);
    history.Commit(false, 0, 0);
    EXPECT_EQ(history.Plan(false, 0, 0, 0.0f).action,
        GuideSimulationFrameAction::Skip);
    EXPECT_EQ(history.Plan(false, 0, 0, 0.016f).action,
        GuideSimulationFrameAction::Simulate);
}

TEST(TressfxSimulationHistory, DelayedFirstPoseResetsAfterBindSpaceFrames)
{
    GuideSimulationHistory history;
    history.Commit(false, 0, 0);
    history.ObserveMissingPose();
    EXPECT_EQ(history.Plan(false, 0, 0, 0.016f).action,
        GuideSimulationFrameAction::Simulate);
    EXPECT_EQ(history.Plan(true, 9, 3, 0.0f).reason,
        GuideSimulationResetReason::FirstPose);
}

TEST(TressfxSimulationHistory, RepeatedFrozenPoseDoesNotRepeatReset)
{
    GuideSimulationHistory history;
    history.Commit(true, 7, 4);
    for (int frame = 0; frame < 8; ++frame)
    {
        EXPECT_EQ(history.Plan(true, 7, 4, 0.016f).action,
            GuideSimulationFrameAction::Simulate);
        history.Commit(true, 7, 4);
    }
}

TEST(TressfxSimulationHistory, ContinuousPoseKeepsHistoryAndPauseSkipsSolver)
{
    GuideSimulationHistory history;
    history.Commit(true, 7, 4);
    EXPECT_EQ(history.Plan(true, 7, 4, 0.0f).action,
        GuideSimulationFrameAction::Skip);
    EXPECT_EQ(history.Plan(true, 7, 4, 0.008f).action,
        GuideSimulationFrameAction::Simulate);
}

TEST(TressfxSimulationHistory, SeekEpochResetsOnceWithoutAdvancingTime)
{
    GuideSimulationHistory history;
    history.Commit(true, 7, 4);
    const GuideSimulationFramePlan plan = history.Plan(true, 7, 5, 0.0f);
    EXPECT_EQ(plan.action, GuideSimulationFrameAction::Reset);
    EXPECT_EQ(plan.reason, GuideSimulationResetReason::PoseDiscontinuity);
    // 只查询计划不能消费事件；成功录制后的 Commit 才解除复位。
    EXPECT_EQ(history.Plan(true, 7, 5, 0.0f).action,
        GuideSimulationFrameAction::Reset);
    history.Commit(true, 7, 5);
    EXPECT_EQ(history.Plan(true, 7, 5, 0.0f).action,
        GuideSimulationFrameAction::Skip);
}

TEST(TressfxSimulationHistory, NewPlayerResetsWithTheSameLocalEpoch)
{
    GuideSimulationHistory history;
    history.Commit(true, 7, 0);
    EXPECT_EQ(history.Plan(true, 8, 0, 0.016f).reason,
        GuideSimulationResetReason::PlaybackChanged);
}

TEST(TressfxSimulationHistory, MissingPosePreservesIdentityAndRecoveryResets)
{
    GuideSimulationHistory history;
    history.Commit(true, 7, 4);
    history.ObserveMissingPose();
    history.Commit(false, 0, 0);
    EXPECT_EQ(history.Plan(false, 0, 0, 0.016f).action,
        GuideSimulationFrameAction::Simulate);
    EXPECT_EQ(history.Plan(true, 7, 4, 0.0f).reason,
        GuideSimulationResetReason::PoseRecovered);
    history.Commit(true, 7, 4);
    EXPECT_EQ(history.Plan(true, 7, 4, 0.016f).action,
        GuideSimulationFrameAction::Simulate);
}

TEST(TressfxSimulationHistory, EpochChangedWhileMissingStillReportsDiscontinuity)
{
    GuideSimulationHistory history;
    history.Commit(true, 7, 4);
    history.ObserveMissingPose();
    EXPECT_EQ(history.Plan(true, 7, 5, 0.0f).reason,
        GuideSimulationResetReason::PoseDiscontinuity);
}

TEST(TressfxSimulationHistory, ResetEventSurvivesZeroDeltaFramesUntilSourceSubstepsRun)
{
    // 源 while 在 delta <= 0 时不执行。history.Plan 是非消费查询，reset 原因要
    // 一直保留到下一个正 delta 帧；VL 的原生调度计划必须同样不消费它。
    GuideSimulationHistory history;
    GuideSimulationFramePlan plan = history.Plan(true, 7, 0, 0.0f);
    ASSERT_EQ(plan.action, GuideSimulationFrameAction::Reset);
    GuideSubstepPlan frame = PlanGuideSubsteps(0.0f,
        plan.action == GuideSimulationFrameAction::Reset, true);
    EXPECT_FALSE(frame.HasWork());
    // 计划函数不消费 reset：下一次查询仍返回同一原因。
    plan = history.Plan(true, 7, 0, 0.0f);
    EXPECT_EQ(plan.reason, GuideSimulationResetReason::FirstPose);
    // 正 delta 帧：首轮 Integration 用 reset 参数，同轮后续 pass 继续，第二轮正常。
    frame = PlanGuideSubsteps(1.0f / 30.0f,
        plan.action == GuideSimulationFrameAction::Reset, true);
    ASSERT_EQ(frame.schedule.substeps, 2u);
    EXPECT_TRUE(frame.resetFirstSubstep);
    EXPECT_FALSE(frame.rebaseFirstSubstep);
    EXPECT_TRUE(frame.FirstSubstepUsesSeparateParams());
    EXPECT_TRUE(frame.RunsConstraintChain(0));
    EXPECT_TRUE(frame.RunsConstraintChain(1));
    // 只有 Integration 已录制后才 Commit；Commit 后同一帧/后续帧都是正常步进。
    history.Commit(true, 7, 0);
    plan = history.Plan(true, 7, 0, frame.schedule.framePhysicalSeconds);
    EXPECT_EQ(plan.action, GuideSimulationFrameAction::Simulate);
    const GuideSubstepPlan nextFrame =
        PlanGuideSubsteps(1.0f / 30.0f, false, false);
    EXPECT_TRUE(nextFrame.HasWork());
    EXPECT_FALSE(nextFrame.resetFirstSubstep);
    EXPECT_FALSE(nextFrame.FirstSubstepUsesSeparateParams());
}

TEST(TressfxSimulationHistory, ResetFlagPreservesStd140LayoutAndPoseTarget)
{
    TressfxSimulationConfig config;
    config.bendStiffness = 0.45f;
    Eigen::Matrix4f skinning = Eigen::Matrix4f::Identity();
    skinning(0, 3) = 123.0f;
    const GuideSimulationParams params =
        MakeGuideSimulationParams(config, Eigen::Vector3f::Zero(), 1, 4, 4,
            0.0f, skinning, true);
    EXPECT_EQ(sizeof(params), 800u);
    EXPECT_FLOAT_EQ(params.motion.x(), 0.45f);
    EXPECT_FLOAT_EQ(params.motion.y(), 1.0f);
    EXPECT_FLOAT_EQ(params.controls.x(), 0.0f);
    EXPECT_TRUE(params.skinning.isApprox(skinning));
    EXPECT_FLOAT_EQ(MakeGuideSimulationParams(config, Eigen::Vector3f::Zero(),
                        1, 4, 4, 0.016f)
                        .motion.y(),
        0.0f);
}
