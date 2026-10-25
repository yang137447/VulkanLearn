#include "hair/tressfxFrameContext.h"
#include "hair/tressfxSimulationHistory.h"
#include "hair/tressfxSimulationOwner.h"
#include "hair/tressfxSimulationParams.h"

#include <gtest/gtest.h>

#include <Eigen/Geometry>

#include <cmath>
#include <vector>

namespace
{
using Eigen::Matrix4f;
using Eigen::Vector3f;
using VL::Hair::TressfxModelHistory;

Matrix4f Translation(float x, float y, float z)
{
    Matrix4f matrix = Matrix4f::Identity();
    matrix.block<3, 1>(0, 3) = Vector3f(x, y, z);
    return matrix;
}

Matrix4f QuarterTurnZ()
{
    Matrix4f matrix = Matrix4f::Identity();
    // 整数矩阵与解析坐标作 oracle，不从被测 inverse/AngleAxis 反推期望。
    matrix(0, 0) = 0.0f;
    matrix(0, 1) = -1.0f;
    matrix(1, 0) = 1.0f;
    matrix(1, 1) = 0.0f;
    return matrix;
}

Vector3f TransformPoint(const Matrix4f& matrix, const Vector3f& point)
{
    return (matrix * point.homogeneous()).head<3>();
}

Vector3f TransformVector(const Matrix4f& matrix, const Vector3f& vector)
{
    return matrix.block<3, 3>(0, 0) * vector;
}

void ExpectWorldHistoryPreserved(
    const Matrix4f& oldModel, const Matrix4f& newModel,
    const Vector3f& current, const Vector3f& previous,
    const Vector3f& previousPrevious)
{
    TressfxModelHistory history;
    history.Commit(oldModel);
    ASSERT_TRUE(history.HasPlacementChange(newModel));

    const Matrix4f historyToCurrent = history.TransformToCurrentLocal(newModel);
    const Vector3f rebasedCurrent = TransformPoint(historyToCurrent, current);
    const Vector3f rebasedPrevious = TransformPoint(historyToCurrent, previous);
    const Vector3f rebasedPreviousPrevious =
        TransformPoint(historyToCurrent, previousPrevious);

    EXPECT_TRUE((newModel * rebasedCurrent.homogeneous()).head<3>().isApprox(
        (oldModel * current.homogeneous()).head<3>(), 1.0e-4f));
    EXPECT_TRUE((newModel * rebasedPrevious.homogeneous()).head<3>().isApprox(
        (oldModel * previous.homogeneous()).head<3>(), 1.0e-4f));
    EXPECT_TRUE((newModel * rebasedPreviousPrevious.homogeneous()).head<3>().isApprox(
        (oldModel * previousPrevious.homogeneous()).head<3>(), 1.0e-4f));
}

// 绕任意 pivot 的刚体 placement：world = R * (p - pivot) + pivot + translation。
// 只供脚本化轨迹使用，不参与“世界空间 reference solver”的期望值生成。
Matrix4f RotateAboutPivot(const Vector3f& translation, const Vector3f& pivot,
    float radians)
{
    const Eigen::Matrix3f rotation =
        Eigen::AngleAxisf(radians, Vector3f::UnitZ()).toRotationMatrix();
    Matrix4f model = Matrix4f::Identity();
    model.block<3, 3>(0, 0) = rotation;
    model.block<3, 1>(0, 3) = pivot - rotation * pivot + translation;
    return model;
}

// 四段脚本轨迹：平移 -> 绕非原点旋转 -> 平移与旋转组合。
// 模型矩阵同时喂给被测的 TressfxModelHistory 和独立世界坐标 oracle。
Matrix4f ScriptedPlacement(int frame)
{
    if (frame < 20)
        return Translation(4.0f * static_cast<float>(frame),
            0.5f * static_cast<float>(frame), -0.25f * static_cast<float>(frame));
    if (frame < 40)
        return RotateAboutPivot(Vector3f(76.0f, 9.5f, -4.75f),
            Vector3f(10.0f, 5.0f, -3.0f),
            0.035f * static_cast<float>(frame - 19));
    return RotateAboutPivot(Vector3f(76.0f + 3.0f * static_cast<float>(frame - 40),
            9.5f - 0.75f * static_cast<float>(frame - 40), -4.75f),
        Vector3f(-6.0f, 2.0f, 4.0f),
        0.7f + 0.02f * static_cast<float>(frame - 40));
}

// 两点（根 + 自由发梢）局部 Verlet 夹具。独立重写 shader 的换基/积分/global
// shape 顺序，用来给“快速平移”和“dt=0 换基”提供可解析的期望坐标。
struct TwoPointLocalSolver
{
    Vector3f rootRest = Vector3f(0.0f, 0.0f, 0.0f);
    Vector3f tipRest = Vector3f(0.0f, -10.0f, 0.0f);
    Vector3f rootCurrent = rootRest;
    Vector3f rootPrevious = rootRest;
    Vector3f tipCurrent = tipRest;
    Vector3f tipPrevious = tipRest;
    TressfxModelHistory history;

    // damping = 0、重力 = 0、rootFixedPoints = 1；shapeStiffness 为 0 时不拉回。
    void Step(const Matrix4f& model, float shapeStiffness)
    {
        const Matrix4f historyToCurrent =
            history.TransformToCurrentLocal(model);
        // 与 tressfxSimIntegration.comp 一样，历史点始终用齐次 w=1 换基。
        rootCurrent = TransformPoint(historyToCurrent, rootCurrent);
        rootPrevious = TransformPoint(historyToCurrent, rootPrevious);
        tipCurrent = TransformPoint(historyToCurrent, tipCurrent);
        tipPrevious = TransformPoint(historyToCurrent, tipPrevious);

        const Vector3f tipBefore = tipCurrent;
        const Vector3f tipOld = tipPrevious;
        tipPrevious = tipBefore;
        // damping 0 -> decay 1，重力 0：局部速度完全由换基后的历史差构成。
        Vector3f integrated = tipBefore + (tipBefore - tipOld);
        if (shapeStiffness > 0.0f)
            integrated += shapeStiffness * (tipRest - integrated);
        tipCurrent = integrated;

        rootPrevious = rootCurrent;
        rootCurrent = rootRest;
        history.Commit(model);
    }

    Vector3f RootWorld(const Matrix4f& model) const
    {
        return TransformPoint(model, rootCurrent);
    }
    Vector3f TipWorld(const Matrix4f& model) const
    {
        return TransformPoint(model, tipCurrent);
    }
};
} // namespace

TEST(TressfxWorldHistory, TranslationRebasesAllThreePositionHistories)
{
    const Matrix4f oldModel = Translation(10.0f, -4.0f, 3.0f);
    const Matrix4f newModel = Translation(22.0f, -4.0f, 3.0f);
    ExpectWorldHistoryPreserved(oldModel, newModel,
        Vector3f(2.0f, 5.0f, -1.0f),
        Vector3f(1.0f, 5.0f, -1.0f),
        Vector3f(0.0f, 5.0f, -1.0f));
    TressfxModelHistory history;
    history.Commit(oldModel);
    const Matrix4f rebase = history.TransformToCurrentLocal(newModel);
    EXPECT_TRUE(TransformPoint(rebase, Vector3f(2, 5, -1))
        .isApprox(Vector3f(-10, 5, -1)));
    EXPECT_TRUE(TransformPoint(rebase, Vector3f(1, 5, -1))
        .isApprox(Vector3f(-11, 5, -1)));
    EXPECT_TRUE(TransformPoint(rebase, Vector3f(0, 5, -1))
        .isApprox(Vector3f(-12, 5, -1)));
}

TEST(TressfxWorldHistory, QuarterTurnPreservesWorldVelocityVector)
{
    const Matrix4f oldModel = Matrix4f::Identity();
    const Matrix4f newModel = QuarterTurnZ();
    TressfxModelHistory history;
    history.Commit(oldModel);

    const Vector3f current(2.0f, 0.0f, 0.0f);
    const Vector3f previous(1.0f, 0.0f, 0.0f);
    const Vector3f rebasedCurrent =
        TransformPoint(history.TransformToCurrentLocal(newModel), current);
    const Vector3f rebasedPrevious =
        TransformPoint(history.TransformToCurrentLocal(newModel), previous);
    const Vector3f currentWorld =
        TransformPoint(newModel, rebasedCurrent);
    const Vector3f previousWorld =
        TransformPoint(newModel, rebasedPrevious);

    EXPECT_TRUE(currentWorld.isApprox(current, 1.0e-4f));
    EXPECT_TRUE(previousWorld.isApprox(previous, 1.0e-4f));
    EXPECT_TRUE(TransformVector(newModel, rebasedCurrent - rebasedPrevious)
        .isApprox(current - previous, 1.0e-4f));
    EXPECT_TRUE(rebasedCurrent.isApprox(Vector3f(0.0f, -2.0f, 0.0f), 1.0e-4f));
    EXPECT_TRUE(rebasedPrevious.isApprox(Vector3f(0.0f, -1.0f, 0.0f), 1.0e-4f));
}

TEST(TressfxWorldHistory, CombinedPlacementUsesCurrentInverseThenOldModel)
{
    const Matrix4f oldModel = Translation(5.0f, 7.0f, -2.0f) * QuarterTurnZ();
    Matrix4f newModel = Translation(-11.0f, 3.0f, 9.0f);
    newModel(0, 0) = -1;
    newModel(1, 1) = -1;
    ExpectWorldHistoryPreserved(oldModel, newModel,
        Vector3f(4.0f, -2.0f, 1.0f),
        Vector3f(3.5f, -2.0f, 1.0f),
        Vector3f(3.0f, -2.0f, 1.0f));
    TressfxModelHistory history;
    history.Commit(oldModel);
    // old local (4,-2,1) -> world (7,11,-1) -> new local (-18,-8,-10)。
    EXPECT_TRUE(TransformPoint(history.TransformToCurrentLocal(newModel),
        Vector3f(4, -2, 1)).isApprox(Vector3f(-18, -8, -10)));
}

TEST(TressfxWorldHistory, QueryDoesNotConsumeChangeAndCommitMakesItStable)
{
    const Matrix4f oldModel = Matrix4f::Identity();
    const Matrix4f newModel = Translation(4.0f, 0.0f, 0.0f);
    TressfxModelHistory history;

    EXPECT_FALSE(history.HasPlacementChange(newModel));
    EXPECT_TRUE(history.TransformToCurrentLocal(newModel)
        .isApprox(Matrix4f::Identity()));

    history.Commit(oldModel);
    const Matrix4f first = history.TransformToCurrentLocal(newModel);
    const Matrix4f second = history.TransformToCurrentLocal(newModel);
    EXPECT_TRUE(first.isApprox(second));
    EXPECT_TRUE(history.HasPlacementChange(newModel));

    history.Commit(newModel);
    EXPECT_FALSE(history.HasPlacementChange(newModel));
    EXPECT_TRUE(history.TransformToCurrentLocal(newModel)
        .isApprox(Matrix4f::Identity()));
}

TEST(TressfxWorldHistory, RootTargetFollowsCurrentPlacementWhileFreeTipKeepsWorld)
{
    const Matrix4f oldModel = Translation(10.0f, 0.0f, 0.0f);
    const Matrix4f newModel = Translation(20.0f, 0.0f, 0.0f);
    const Vector3f rootRest(0.0f, 2.0f, 0.0f);
    const Vector3f freeTip(3.0f, 5.0f, 0.0f);
    TressfxModelHistory history;
    history.Commit(oldModel);

    const Matrix4f historyToCurrent = history.TransformToCurrentLocal(newModel);
    const Vector3f rebasedTip = TransformPoint(historyToCurrent, freeTip);
    const Vector3f rootTarget = TransformPoint(newModel, rootRest);

    EXPECT_TRUE(TransformPoint(newModel, rebasedTip)
        .isApprox(TransformPoint(oldModel, freeTip), 1.0e-4f));
    EXPECT_TRUE(rootTarget.isApprox(Vector3f(20.0f, 2.0f, 0.0f), 1.0e-4f));
    EXPECT_FALSE(rootTarget.isApprox(
        TransformPoint(oldModel, rootRest), 1.0e-4f));
}

TEST(TressfxWorldHistory, NonUniformScaleProducesFinitePointRebaseOnly)
{
    Matrix4f oldModel = Translation(2.0f, 3.0f, 4.0f);
    oldModel(0, 0) = 1.0f;
    oldModel(1, 1) = 2.0f;
    oldModel(2, 2) = 1.5f;
    Matrix4f newModel = Translation(-5.0f, 8.0f, 1.0f);
    newModel(0, 0) = 0.5f;
    newModel(1, 1) = 3.0f;
    newModel(2, 2) = 0.75f;

    TressfxModelHistory history;
    history.Commit(oldModel);
    const Vector3f local(4.0f, -2.0f, 7.0f);
    const Vector3f rebased = TransformPoint(
        history.TransformToCurrentLocal(newModel), local);

    EXPECT_TRUE(rebased.allFinite());
    EXPECT_TRUE(TransformPoint(newModel, rebased)
        .isApprox(TransformPoint(oldModel, local), 1.0e-4f));
}

TEST(TressfxWorldHistory, SequentialCommittedPlacementsKeepStationaryTipWorld)
{
    TressfxModelHistory history;
    history.Commit(Matrix4f::Identity());
    const Matrix4f firstPlacement = Translation(10, 0, 0);
    const Vector3f firstLocal = TransformPoint(
        history.TransformToCurrentLocal(firstPlacement), Vector3f(3, 2, 1));
    history.Commit(firstPlacement);
    const Matrix4f secondPlacement = Translation(20, 0, 0) * QuarterTurnZ();
    const Vector3f secondLocal = TransformPoint(
        history.TransformToCurrentLocal(secondPlacement), firstLocal);
    EXPECT_TRUE(secondLocal.isApprox(Vector3f(2, 17, 1)));
    EXPECT_TRUE(TransformPoint(secondPlacement, secondLocal)
        .isApprox(Vector3f(3, 2, 1)));
}

TEST(TressfxWorldHistory, ZeroDeltaPlacementRebaseDoesNotConsumeSolverClock)
{
    VL::Hair::GuideSimulationHistory poseHistory;
    poseHistory.Commit(true, 7, 1);
    TressfxModelHistory modelHistory;
    modelHistory.Commit(Matrix4f::Identity());
    VL::Hair::TressfxFrameClock clock;
    clock.Consume(1, 10, 0.01f);
    const Matrix4f placement = Translation(10, 0, 0);
    EXPECT_EQ(poseHistory.Plan(true, 7, 1, 0).action,
        VL::Hair::GuideSimulationFrameAction::Skip);
    EXPECT_TRUE(modelHistory.HasPlacementChange(placement));
    EXPECT_TRUE(TransformPoint(modelHistory.TransformToCurrentLocal(placement),
        Vector3f(2, 0, 0)).isApprox(Vector3f(-8, 0, 0)));
    modelHistory.Commit(placement);
    EXPECT_FALSE(modelHistory.HasPlacementChange(placement));
    EXPECT_FLOAT_EQ(clock.DeltaForFrame(1, 11, 0.01f), 0.01f);
}

TEST(TressfxWorldHistory, ResetOverridesOldInertiaAfterMissingPose)
{
    VL::Hair::GuideSimulationHistory poseHistory;
    poseHistory.Commit(true, 7, 1);
    poseHistory.ObserveMissingPose();
    const auto plan = poseHistory.Plan(true, 7, 1, 0);
    EXPECT_EQ(plan.action, VL::Hair::GuideSimulationFrameAction::Reset);
    EXPECT_EQ(plan.reason, VL::Hair::GuideSimulationResetReason::PoseRecovered);
    // Reset 的计划优先于时间；GPU 三 history 写 current rest 仍由 shader 路径验证。
    poseHistory.Commit(true, 7, 1);
    EXPECT_EQ(poseHistory.Plan(true, 7, 1, 0).action,
        VL::Hair::GuideSimulationFrameAction::Skip);
}

TEST(TressfxWorldHistory, OwnerAllowsRepeatedDrawOfOnePlacement)
{
    VL::Hair::TressfxSimulationOwner owner;
    owner.Observe("actor/body/hair");
    owner.Observe("actor/body/hair");
    EXPECT_FALSE(owner.HasConflict());
    EXPECT_EQ(owner.OwnerIdentity(), "actor/body/hair");
}

TEST(TressfxWorldHistory, SecondPlacementMakesSharedMutableBufferConflictSticky)
{
    VL::Hair::TressfxSimulationOwner owner;
    owner.Observe("first");
    owner.Observe("second");
    owner.Observe("first");
    EXPECT_TRUE(owner.HasConflict());
    EXPECT_EQ(owner.OwnerIdentity(), "first");
}

TEST(TressfxWorldForces, RotatedPlacementKeepsWorldGravityDown)
{
    TressfxSimulationConfig config;
    config.gravity = Vector3f(0, -980, 0);
    const Matrix4f model = Translation(10, 20, 30) * QuarterTurnZ();
    const auto params = VL::Hair::MakeGuideSimulationParams(config, 1, 4, 4,
        0.01f, 0, Matrix4f::Identity(), false, Matrix4f::Identity(), false, model);
    EXPECT_TRUE(params.gravity.head<3>().isApprox(Vector3f(-980, 0, 0)));
    const Vector3f worldDisplacement = TransformVector(model,
        params.gravity.head<3>() * 0.01f * 0.01f);
    EXPECT_TRUE(worldDisplacement.isApprox(Vector3f(0, -0.098f, 0), 1.0e-5f));
}

TEST(TressfxWorldForces, ConeExpandsInWorldBeforeQuarterTurnToHairLocal)
{
    TressfxSimulationConfig config;
    config.wind = Vector3f(100, 0, 0);
    const Matrix4f model = QuarterTurnZ();
    const auto params = VL::Hair::MakeGuideSimulationParams(config, 1, 4, 4,
        0.01f, 0, Matrix4f::Identity(), false, Matrix4f::Identity(), false, model);
    const float c = 50 * std::cos(40.0f * 3.14159265358979323846f / 180.0f);
    const float s = 50 * std::sin(40.0f * 3.14159265358979323846f / 180.0f);
    EXPECT_TRUE(params.wind0.head<3>().isApprox(Vector3f(0, -c, -s)));
    EXPECT_TRUE(params.wind1.head<3>().isApprox(Vector3f(0, -c, s)));
    EXPECT_TRUE(params.wind2.head<3>().isApprox(Vector3f(s, -c, 0)));
    EXPECT_TRUE(params.wind3.head<3>().isApprox(Vector3f(-s, -c, 0)));
}

TEST(TressfxWorldForces, AuthoredScaleOnlyGuaranteesFiniteVectorConversion)
{
    TressfxSimulationConfig config;
    config.gravity = Vector3f(20, -980, 30);
    config.wind = Vector3f(100, 0, 0);
    Matrix4f model = Matrix4f::Identity();
    model(0, 0) = 2;
    model(1, 1) = 4;
    model(2, 2) = 0.5f;
    const auto params = VL::Hair::MakeGuideSimulationParams(config, 1, 4, 4,
        0.01f, 0, Matrix4f::Identity(), false, Matrix4f::Identity(), false, model);
    EXPECT_TRUE(params.gravity.head<3>().isApprox(Vector3f(10, -245, 60)));
    EXPECT_TRUE(params.wind0.allFinite());
    EXPECT_TRUE(params.wind1.allFinite());
    EXPECT_TRUE(params.wind2.allFinite());
    EXPECT_TRUE(params.wind3.allFinite());
    // 不据向量换基推出长度、三叉积风力或碰撞 metric 在 scale 下等价。
}

TEST(TressfxWorldHistory, FirstFrameNeverRebasesActorInitialPlacement)
{
    TressfxModelHistory history;
    const Matrix4f initialPlacement = Translation(37.0f, -12.0f, 5.0f) * QuarterTurnZ();

    // 首帧尚未提交任何 model：SSBO 初值本来就是 hair 局部 rest，
    // 不能把 actor 的初始 placement 当成一次“从 Identity 到当前”的移动。
    EXPECT_FALSE(history.HasPlacementChange(initialPlacement));
    EXPECT_TRUE(history.TransformToCurrentLocal(initialPlacement)
        .isApprox(Matrix4f::Identity()));

    history.Commit(initialPlacement);
    EXPECT_FALSE(history.HasPlacementChange(initialPlacement));

    // 第二帧真实移动才换基，并保持世界位置。
    const Matrix4f moved = Translation(50.0f, 0.0f, 0.0f) * initialPlacement;
    EXPECT_TRUE(history.HasPlacementChange(moved));
    const Vector3f local = TransformPoint(
        history.TransformToCurrentLocal(moved), Vector3f(2.0f, 3.0f, 4.0f));
    EXPECT_TRUE(TransformPoint(moved, local).isApprox(
        TransformPoint(initialPlacement, Vector3f(2.0f, 3.0f, 4.0f)), 1.0e-4f));
}

TEST(TressfxWorldHistory, ZeroDeltaRebaseKeepsWorldVelocityAndClock)
{
    VL::Hair::TressfxFrameClock clock;
    clock.Consume(3, 10, 0.01f);

    TressfxModelHistory history;
    history.Commit(Translation(5.0f, 0.0f, 0.0f));
    // M1 = T(5) * Rz90：世界 (7,1) 在 M1 局部为 (1,-2)；
    // 世界 (6,1) 为 (1,-1)，世界 (5,1) 为 (1,0)，均由解析求逆得到。
    const Matrix4f moved = Translation(5.0f, 0.0f, 0.0f) * QuarterTurnZ();
    const Matrix4f historyToCurrent = history.TransformToCurrentLocal(moved);

    const Vector3f localCurrent = TransformPoint(
        historyToCurrent, Vector3f(2.0f, 1.0f, 0.0f));
    const Vector3f localPrevious = TransformPoint(
        historyToCurrent, Vector3f(1.0f, 1.0f, 0.0f));
    const Vector3f localPreviousPrevious = TransformPoint(
        historyToCurrent, Vector3f(0.0f, 1.0f, 0.0f));
    EXPECT_TRUE(localCurrent.isApprox(Vector3f(1.0f, -2.0f, 0.0f), 1.0e-5f));
    EXPECT_TRUE(localPrevious.isApprox(Vector3f(1.0f, -1.0f, 0.0f), 1.0e-5f));
    EXPECT_TRUE(localPreviousPrevious.isApprox(Vector3f(1.0f, 0.0f, 0.0f), 1.0e-5f));

    // 换基只改表示坐标：三份世界位置和世界速度都不变。
    const Vector3f worldCurrent = TransformPoint(moved, localCurrent);
    const Vector3f worldPrevious = TransformPoint(moved, localPrevious);
    const Vector3f worldPreviousPrevious =
        TransformPoint(moved, localPreviousPrevious);
    EXPECT_TRUE(worldCurrent.isApprox(Vector3f(7.0f, 1.0f, 0.0f), 1.0e-5f));
    EXPECT_TRUE(worldPrevious.isApprox(Vector3f(6.0f, 1.0f, 0.0f), 1.0e-5f));
    EXPECT_TRUE(worldPreviousPrevious.isApprox(
        Vector3f(5.0f, 1.0f, 0.0f), 1.0e-5f));
    EXPECT_TRUE((worldCurrent - worldPrevious).isApprox(
        Vector3f(1.0f, 0.0f, 0.0f), 1.0e-5f));

    // 0 dt 换基不 Consume 时钟：同一帧仍算已消费，下一帧仍拿到自己的 dt。
    EXPECT_FLOAT_EQ(clock.DeltaForFrame(3, 10, 0.01f), 0.0f);
    EXPECT_FLOAT_EQ(clock.DeltaForFrame(3, 11, 0.01f), 0.01f);
    history.Commit(moved);
    EXPECT_FALSE(history.HasPlacementChange(moved));
}

TEST(TressfxWorldHistory, PivotRotationPreservesWorldVelocityDirection)
{
    TressfxModelHistory history;
    history.Commit(Matrix4f::Identity());
    // 绕 (10,0,0) 转 +90°：pivot 带来的平移进入换基矩阵，不能被当成纯平移。
    const Matrix4f rotated =
        Translation(10.0f, 0.0f, 0.0f) * QuarterTurnZ() *
        Translation(-10.0f, 0.0f, 0.0f);
    const Matrix4f historyToCurrent = history.TransformToCurrentLocal(rotated);

    // 解析局部坐标：世界 (10,2)->(12,0)，(10,1)->(11,0)，(10,0.5)->(10.5,0)。
    const Vector3f localCurrent =
        TransformPoint(historyToCurrent, Vector3f(10.0f, 2.0f, 0.0f));
    const Vector3f localPrevious =
        TransformPoint(historyToCurrent, Vector3f(10.0f, 1.0f, 0.0f));
    const Vector3f localPreviousPrevious =
        TransformPoint(historyToCurrent, Vector3f(10.0f, 0.5f, 0.0f));
    EXPECT_TRUE(localCurrent.isApprox(Vector3f(12.0f, 0.0f, 0.0f), 1.0e-5f));
    EXPECT_TRUE(localPrevious.isApprox(Vector3f(11.0f, 0.0f, 0.0f), 1.0e-5f));
    EXPECT_TRUE(localPreviousPrevious.isApprox(
        Vector3f(10.5f, 0.0f, 0.0f), 1.0e-5f));

    // 换基矩阵同时含旋转与 pivot 平移；只做平移会给出不同的局部坐标。
    EXPECT_FALSE((historyToCurrent.block<3, 3>(0, 0)
        .isApprox(Eigen::Matrix3f::Identity())));
    EXPECT_TRUE((historyToCurrent.block<3, 1>(0, 3)
        .isApprox(Vector3f(10.0f, 10.0f, 0.0f), 1.0e-5f)));

    // 世界位置不变，世界速度仍沿旧 Y 方向；不会把旋转误当平移而改写速度。
    EXPECT_TRUE(TransformPoint(rotated, localCurrent)
        .isApprox(Vector3f(10.0f, 2.0f, 0.0f), 1.0e-5f));
    EXPECT_TRUE(TransformPoint(rotated, localPrevious)
        .isApprox(Vector3f(10.0f, 1.0f, 0.0f), 1.0e-5f));
    const Vector3f worldVelocity =
        TransformPoint(rotated, localCurrent) -
        TransformPoint(rotated, localPrevious);
    EXPECT_TRUE(worldVelocity.isApprox(Vector3f(0.0f, 1.0f, 0.0f), 1.0e-5f));
}

TEST(TressfxWorldHistory, FastTranslationRootFollowsAndTipHoldsThenPullsBack)
{
    TwoPointLocalSolver solver;
    const Matrix4f restModel = Matrix4f::Identity();
    solver.Step(restModel, 0.0f);

    // 快速平移：根点当帧跟随，自由发梢在换基后仍留在旧世界位置。
    const Matrix4f moved = Translation(100.0f, 0.0f, 0.0f);
    solver.Step(moved, 0.0f);
    EXPECT_TRUE(solver.RootWorld(moved).isApprox(Vector3f(100.0f, 0.0f, 0.0f), 1.0e-5f));
    EXPECT_TRUE(solver.TipWorld(moved).isApprox(Vector3f(0.0f, -10.0f, 0.0f), 1.0e-5f));

    // 没有速度、没有 shape 时发梢继续留在旧世界位置（不是被 placement 带走）。
    solver.Step(moved, 0.0f);
    EXPECT_TRUE(solver.TipWorld(moved).isApprox(Vector3f(0.0f, -10.0f, 0.0f), 1.0e-5f));

    // 打开 global shape（stiffness 0.5）：局部 -100 被拉向 0，世界坐标 50。
    solver.Step(moved, 0.5f);
    EXPECT_TRUE(solver.TipWorld(moved).isApprox(Vector3f(50.0f, -10.0f, 0.0f), 1.0e-5f));
    // 第二次 shape step 到达目标；Verlet 仍保留上一步速度，第三次会越过目标，
    // 说明 global shape 是软约束，不是把发梢硬钉在 rest。
    solver.Step(moved, 0.5f);
    EXPECT_TRUE(solver.TipWorld(moved).isApprox(Vector3f(100.0f, -10.0f, 0.0f), 1.0e-5f));
    solver.Step(moved, 0.5f);
    EXPECT_TRUE(solver.TipWorld(moved).isApprox(Vector3f(125.0f, -10.0f, 0.0f), 1.0e-4f));
}

TEST(TressfxWorldHistory, ScriptedPlacementMatchesIndependentWorldVerletOracle)
{
    // 独立 oracle：只在世界坐标推进 Verlet，不调用 TressfxModelHistory 生成期望。
    // 根点每步钉到当前 placement 的 rest；3 个自由点只受重力与阻尼。
    const std::vector<Vector3f> rest{
        Vector3f(0.0f, 0.0f, 0.0f),
        Vector3f(0.0f, -8.0f, 0.0f),
        Vector3f(0.0f, -15.0f, 0.0f),
        Vector3f(0.0f, -21.0f, 0.0f)};
    constexpr size_t root = 0;
    constexpr float dt = 1.0f / 60.0f;
    constexpr float damping = 0.05f;
    const float decay = std::exp(-damping * dt * 60.0f);
    const Vector3f gravity(0.0f, -980.0f, 0.0f);

    std::vector<Vector3f> worldCurrent = rest;
    std::vector<Vector3f> worldPrevious = rest;
    std::vector<Vector3f> worldPreviousPrevious = rest;
    const Matrix4f firstModel = ScriptedPlacement(0);
    for (size_t point = 0; point < rest.size(); ++point)
        worldCurrent[point] = worldPrevious[point] = worldPreviousPrevious[point] =
            TransformPoint(firstModel, rest[point]);

    // 被测表示：hair 局部三份历史 + production TressfxModelHistory 换基矩阵。
    std::vector<Vector3f> localCurrent = rest;
    std::vector<Vector3f> localPrevious = rest;
    std::vector<Vector3f> localPreviousPrevious = rest;
    TressfxModelHistory history;

    for (int frame = 0; frame < 90; ++frame)
    {
        const Matrix4f model = ScriptedPlacement(frame);
        const Matrix4f historyToCurrent = history.TransformToCurrentLocal(model);
        for (size_t point = 0; point < rest.size(); ++point)
        {
            localCurrent[point] = TransformPoint(historyToCurrent, localCurrent[point]);
            localPrevious[point] = TransformPoint(historyToCurrent, localPrevious[point]);
            localPreviousPrevious[point] =
                TransformPoint(historyToCurrent, localPreviousPrevious[point]);
        }

        const Vector3f localGravity = model.block<3, 3>(0, 0).inverse() * gravity;
        std::vector<Vector3f> nextLocal = localCurrent;
        std::vector<Vector3f> nextWorld = worldCurrent;
        for (size_t point = 0; point < rest.size(); ++point)
        {
            if (point == root)
            {
                // 无蒙皮夹具：InitialPos = identity * rest。
                nextLocal[point] = rest[point];
                nextWorld[point] = TransformPoint(model, rest[point]);
                continue;
            }
            const Vector3f current = localCurrent[point];
            const Vector3f old = localPrevious[point];
            nextLocal[point] = current + decay * (current - old) +
                localGravity * dt * dt;
            nextWorld[point] = worldCurrent[point] +
                decay * (worldCurrent[point] - worldPrevious[point]) +
                gravity * dt * dt;
        }

        localPreviousPrevious = localPrevious;
        localPrevious = localCurrent;
        localCurrent = nextLocal;
        worldPreviousPrevious = worldPrevious;
        worldPrevious = worldCurrent;
        worldCurrent = nextWorld;

        for (size_t point = 0; point < rest.size(); ++point)
        {
            const Vector3f actualWorld = TransformPoint(model, localCurrent[point]);
            const Vector3f actualPreviousWorld =
                TransformPoint(model, localPrevious[point]);
            EXPECT_LT((actualWorld - worldCurrent[point]).norm(), 2.0e-2f)
                << "frame " << frame << " point " << point;
            EXPECT_LT((actualPreviousWorld - worldPrevious[point]).norm(), 2.0e-2f)
                << "frame " << frame << " point " << point;
            EXPECT_LT((actualWorld - actualPreviousWorld -
                    (worldCurrent[point] - worldPrevious[point])).norm(), 5.0e-2f)
                << "world velocity mismatch frame " << frame << " point " << point;
        }
        history.Commit(model);
    }
}
