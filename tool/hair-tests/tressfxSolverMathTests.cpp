#include "hair/tressfxSolverMath.h"

#include <gtest/gtest.h>

#include <cmath>

namespace
{
using namespace VL::Hair;

constexpr float kTolerance = 1.0e-5f;
}

TEST(TressfxSolverMath, DistanceConstraintMultiplierCoversAllMovableCombinations)
{
    EXPECT_TRUE(TressfxConstraintMultiplier(true, true)
            .isApprox(Eigen::Vector2f(0.5f, 0.5f)));
    EXPECT_TRUE(TressfxConstraintMultiplier(true, false)
            .isApprox(Eigen::Vector2f(1.0f, 0.0f)));
    EXPECT_TRUE(TressfxConstraintMultiplier(false, true)
            .isApprox(Eigen::Vector2f(0.0f, 1.0f)));
    EXPECT_TRUE(TressfxConstraintMultiplier(false, false)
            .isApprox(Eigen::Vector2f(0.0f, 0.0f)));
}

TEST(TressfxSolverMath, DistanceConstraintSplitsCorrectionBetweenMovablePoints)
{
    Eigen::Vector3f pos0(0.0f, 0.0f, 0.0f);
    Eigen::Vector3f pos1(2.0f, 0.0f, 0.0f);
    TressfxApplyDistanceConstraint(pos0, true, pos1, true, 1.0f);
    EXPECT_TRUE(pos0.isApprox(Eigen::Vector3f(0.5f, 0.0f, 0.0f)));
    EXPECT_TRUE(pos1.isApprox(Eigen::Vector3f(1.5f, 0.0f, 0.0f)));
    EXPECT_NEAR((pos1 - pos0).norm(), 1.0f, kTolerance);
}

TEST(TressfxSolverMath, DistanceConstraintGivesFullCorrectionToOneMovablePoint)
{
    Eigen::Vector3f fixedRoot(0.0f, 0.0f, 0.0f);
    Eigen::Vector3f movable(2.0f, 0.0f, 0.0f);
    TressfxApplyDistanceConstraint(fixedRoot, false, movable, true, 1.0f);
    EXPECT_TRUE(fixedRoot.isZero());
    EXPECT_TRUE(movable.isApprox(Eigen::Vector3f(1.0f, 0.0f, 0.0f)));

    Eigen::Vector3f movableRoot(0.0f, 0.0f, 0.0f);
    Eigen::Vector3f fixedTip(2.0f, 0.0f, 0.0f);
    TressfxApplyDistanceConstraint(movableRoot, true, fixedTip, false, 1.0f);
    EXPECT_TRUE(movableRoot.isApprox(Eigen::Vector3f(1.0f, 0.0f, 0.0f)));
    EXPECT_TRUE(fixedTip.isApprox(Eigen::Vector3f(2.0f, 0.0f, 0.0f)));
}

TEST(TressfxSolverMath, DistanceConstraintLeavesFullyFixedPairUntouched)
{
    Eigen::Vector3f pos0(0.0f, 0.0f, 0.0f);
    Eigen::Vector3f pos1(2.0f, 0.0f, 0.0f);
    TressfxApplyDistanceConstraint(pos0, false, pos1, false, 1.0f);
    EXPECT_TRUE(pos0.isZero());
    EXPECT_TRUE(pos1.isApprox(Eigen::Vector3f(2.0f, 0.0f, 0.0f)));
}

TEST(TressfxSolverMath, WindForceIsPerpendicularToSegmentAndOpposesPerpendicularWind)
{
    // TressFX: Force = -cross(cross(v, w), v)，v 是 pos_i - pos_{i+1}。
    const Eigen::Vector3f segment(1.0f, 0.0f, 0.0f);
    const Eigen::Vector3f wind(0.0f, 1.0f, 0.0f);
    const Eigen::Vector3f force = TressfxWindForce(segment, wind);
    EXPECT_NEAR(force.dot(segment), 0.0f, kTolerance);
    // 段与风垂直时 cross(cross(v,w),v) 等于 w，负号使力指向 -w。
    EXPECT_TRUE(force.isApprox(Eigen::Vector3f(0.0f, -1.0f, 0.0f)));

    // 段方向与风向不垂直时，力仍在 v 的垂直平面内，只保留风在 v 上的垂直分量。
    const Eigen::Vector3f slanted(1.0f, 1.0f, 0.0f);
    const Eigen::Vector3f slantedForce = TressfxWindForce(slanted, wind);
    EXPECT_NEAR(slantedForce.dot(slanted), 0.0f, kTolerance);
    EXPECT_NEAR(slantedForce.dot(wind), -0.5f, kTolerance);
    EXPECT_NEAR(slantedForce.norm(), std::sqrt(0.5f), 1.0e-5f);

    // 单位适配：段长度不影响风力（源实现在米制下按 |v|² 缩放）。
    EXPECT_TRUE(TressfxWindForce(4.5f * slanted, wind)
            .isApprox(slantedForce, 1.0e-5f));
}

TEST(TressfxSolverMath, ExponentialDampingMatchesSixtyHertzRetention)
{
    // 作者 damping 是 60 Hz 下每步保留的比例；映射后在该 dt 上必须复现。
    for (const float damping : {1.0f, 0.995f, 0.9f})
    {
        const float coeff = TressfxDampingCoeffFromRetention(damping);
        EXPECT_NEAR(TressfxDampingDecay(coeff, 1.0f / 60.0f), damping, 1.0e-6f);
    }
    // 半帧 ⇒ 指数衰减的平方根；两帧 ⇒ 平方。
    const float coeff = TressfxDampingCoeffFromRetention(0.995f);
    const float halfFrame = TressfxDampingDecay(coeff, 1.0f / 120.0f);
    EXPECT_NEAR(halfFrame * halfFrame, 0.995f, 1.0e-6f);
    const float twoFrames = TressfxDampingDecay(coeff, 2.0f / 60.0f);
    EXPECT_NEAR(twoFrames, 0.995f * 0.995f, 1.0e-6f);
}

TEST(TressfxSolverMath, GlobalShapeGateUsesRangeFractionAndStiffness)
{
    EXPECT_TRUE(TressfxGlobalShapeApplies(3, 10, 0.5f, 0.5f));
    EXPECT_FALSE(TressfxGlobalShapeApplies(5, 10, 0.5f, 0.5f));
    EXPECT_FALSE(TressfxGlobalShapeApplies(3, 10, 0.0f, 0.5f));
    EXPECT_FALSE(TressfxGlobalShapeApplies(3, 10, 0.5f, 0.0f));

    const Eigen::Vector3f initial(0.0f, 0.0f, 0.0f);
    const Eigen::Vector3f temp(4.0f, 0.0f, 0.0f);
    EXPECT_TRUE(TressfxGlobalShapePull(initial, temp, 0.25f)
            .isApprox(Eigen::Vector3f(3.0f, 0.0f, 0.0f)));
    EXPECT_TRUE(TressfxGlobalShapePull(initial, temp, 1.0f).isZero());
}

TEST(TressfxSolverMath, VspBlendEndpointsAreUnchangedAndFullyTransformed)
{
    const Eigen::Vector3f position(1.0f, 2.0f, 3.0f);
    const Eigen::Vector3f translation(10.0f, 0.0f, 0.0f);
    const Eigen::Vector4f identity(0.0f, 0.0f, 0.0f, 1.0f);
    EXPECT_TRUE(TressfxVspBlend(position, identity, translation, 0.0f)
            .isApprox(position));
    EXPECT_TRUE(TressfxVspBlend(position, identity, translation, 1.0f)
            .isApprox(position + translation));

    // 绕 Z 轴 +90°：x 轴转到 y 轴。
    const float half = 0.25f * 3.14159265358979323846f;
    const Eigen::Vector4f quarterTurn(0.0f, 0.0f, std::sin(half), std::cos(half));
    const Eigen::Vector4f unitX(1.0f, 0.0f, 0.0f, 1.0f);
    EXPECT_TRUE(TressfxVspBlend(Eigen::Vector3f(1.0f, 0.0f, 0.0f), quarterTurn,
                    Eigen::Vector3f::Zero(), 1.0f)
            .isApprox(Eigen::Vector3f(0.0f, 1.0f, 0.0f), 1.0e-5f));
    EXPECT_TRUE(TressfxQuatFromTwoUnitVectors(
                    Eigen::Vector3f::UnitX(), Eigen::Vector3f::UnitY())
            .isApprox(quarterTurn, 1.0e-5f));
}

TEST(TressfxSolverMath, VspStiffnessFallsLinearlyFromRootPrefixToTip)
{
    // rootFixedPoints = 2, 10 points: local 2 -> 1, local 9 -> 0。
    EXPECT_NEAR(TressfxVspStiffness(2, 10, 2), 1.0f, kTolerance);
    EXPECT_NEAR(TressfxVspStiffness(9, 10, 2), 0.0f, kTolerance);
    EXPECT_NEAR(TressfxVspStiffness(5, 10, 2), 1.0f - 3.0f / 7.0f, kTolerance);
    // 只有根前缀时无法形成衰减区间，返回 0（对应 tip）。
    EXPECT_NEAR(TressfxVspStiffness(2, 3, 2), 0.0f, kTolerance);
}

TEST(TressfxSolverMath, StrandTransformBuildsRotationFromFirstTwoPoints)
{
    // 第一段从 +Y 转到 +X，根点不动。
    const TressfxStrandTransform transform = TressfxComputeStrandTransform(
        Eigen::Vector3f(0, 0, 0), Eigen::Vector3f(0, 1, 0),
        Eigen::Vector3f(0, 0, 0), Eigen::Vector3f(1, 0, 0),
        Eigen::Vector3f(0, 1, 0), 0.5f, 5000.0f);
    EXPECT_NEAR(transform.coefficient, 0.5f, kTolerance);
    EXPECT_TRUE(transform.translation.isZero());
    EXPECT_TRUE(TressfxRotateByQuat(transform.quat, Eigen::Vector3f(0, 1, 0))
            .isApprox(Eigen::Vector3f(1, 0, 0), 1.0e-5f));

    // 伪加速度超过阈值时强制系数 1。
    const TressfxStrandTransform overThreshold = TressfxComputeStrandTransform(
        Eigen::Vector3f(0, 0, 0), Eigen::Vector3f(0, 1, 0),
        Eigen::Vector3f(0, 0, 0), Eigen::Vector3f(0, 1, 0),
        Eigen::Vector3f(6000, 1, 0), 0.0f, 5000.0f);
    EXPECT_NEAR(overThreshold.coefficient, 1.0f, kTolerance);
}

TEST(TressfxSolverMath, LocalShapeIsZeroForStraightRestAndCurrentStrand)
{
    // 直 rest + 直 current：rotGlobal 是单位四元数，target 正好是当前 i+1。
    const Eigen::Vector4f identity(0.0f, 0.0f, 0.0f, 1.0f);
    const Eigen::Vector3f delta = TressfxLocalShapeDelta(identity,
        Eigen::Vector3f(0, 0, 0), Eigen::Vector3f(1, 0, 0),
        Eigen::Vector3f(2, 0, 0), Eigen::Vector3f(0, 0, 0),
        Eigen::Vector3f(1, 0, 0), Eigen::Vector3f(2, 0, 0), 1.0f);
    EXPECT_TRUE(delta.isZero());
}

TEST(TressfxSolverMath, LocalShapeRestoresBentRestDirectionOnStraightStrand)
{
    // rest 在 i+1 处弯 +Y，current 拉直：del 把 i+1 拉向 rest 方向。
    const Eigen::Vector4f identity(0.0f, 0.0f, 0.0f, 1.0f);
    const Eigen::Vector3f delta = TressfxLocalShapeDelta(identity,
        Eigen::Vector3f(0, 0, 0), Eigen::Vector3f(1, 0, 0),
        Eigen::Vector3f(1, 1, 0), Eigen::Vector3f(0, 0, 0),
        Eigen::Vector3f(1, 0, 0), Eigen::Vector3f(2, 0, 0), 1.0f);
    // target = pos + vecBindPose = (1,1,0)，0.5*(target - (2,0,0)) = (-0.5,0.5,0)。
    EXPECT_TRUE(delta.isApprox(Eigen::Vector3f(-0.5f, 0.5f, 0.0f), 1.0e-5f));

    // 把 del 应用回 i/i+1 后，i->i+1 段正好转到 bind-pose 方向 (0,1,0)。
    Eigen::Vector3f pos = Eigen::Vector3f(1, 0, 0) - delta;
    Eigen::Vector3f posPlusOne = Eigen::Vector3f(2, 0, 0) + delta;
    EXPECT_TRUE((posPlusOne - pos).normalized()
            .isApprox(Eigen::Vector3f(0, 1, 0), 1.0e-5f));
}

TEST(TressfxSolverMath, LocalShapeUsesSkinningQuatToRotateBindPose)
{
    // 骨骼绕 Z 轴 +90°：rest 的 +X 段在绑定空间变成 +Y，因此直 current 会被拉向 +Y。
    const float half = 0.25f * 3.14159265358979323846f;
    const Eigen::Vector4f skinningQuat(0.0f, 0.0f, std::sin(half), std::cos(half));
    const Eigen::Vector3f delta = TressfxLocalShapeDelta(skinningQuat,
        Eigen::Vector3f(0, 0, 0), Eigen::Vector3f(1, 0, 0),
        Eigen::Vector3f(2, 0, 0), Eigen::Vector3f(0, 0, 0),
        Eigen::Vector3f(1, 0, 0), Eigen::Vector3f(2, 0, 0), 1.0f);
    // bindPos = (0,1,0)，bindPosPlusOne = (0,2,0)，vecBindPose 也是 (0,1,0)；
    // rotGlobal 把 bind 段 (0,1,0) 转到当前段 (1,0,0)，target = pos + (1,0,0)。
    // del = 0.5*((2,0,0)-(2,0,0)) = 0：当前段方向与绑定段旋转后一致。
    EXPECT_TRUE(delta.isZero());
}

TEST(TressfxSolverMath, VelocityClampRewritesHistoryWithSourceScale)
{
    // 源实现：delta *= Clamp² / |delta|²，故 |new delta| = Clamp² / |old delta|。
    Eigen::Vector3f previous(0.0f, 0.0f, 0.0f);
    EXPECT_TRUE(TressfxClampPositionDelta(previous, Eigen::Vector3f(40.0f, 0.0f, 0.0f)));
    EXPECT_TRUE(previous.isApprox(Eigen::Vector3f(17.5f, 0.0f, 0.0f), 1.0e-5f));
    EXPECT_NEAR((Eigen::Vector3f(40.0f, 0.0f, 0.0f) - previous).norm(),
        30.0f * 30.0f / 40.0f, 1.0e-4f);

    // 恰好等于 Clamp 时不重写。
    Eigen::Vector3f inside(0.0f, 0.0f, 0.0f);
    EXPECT_FALSE(TressfxClampPositionDelta(inside, Eigen::Vector3f(30.0f, 0.0f, 0.0f)));
    EXPECT_TRUE(inside.isZero());
}

TEST(TressfxSolverMath, QuaternionFromMatrixExtractsBoneRotation)
{
    Eigen::Matrix4f matrix = Eigen::Matrix4f::Identity();
    matrix.block<3, 3>(0, 0) =
        Eigen::AngleAxisf(0.7f, Eigen::Vector3f::UnitY()).toRotationMatrix();
    matrix(0, 3) = 12.0f;
    const Eigen::Vector4f quat = TressfxQuaternionFromMatrix(matrix);
    EXPECT_NEAR(quat.norm(), 1.0f, 1.0e-5f);
    EXPECT_TRUE(TressfxRotateByQuat(quat, Eigen::Vector3f::UnitX())
            .isApprox(matrix.block<3, 3>(0, 0) * Eigen::Vector3f::UnitX(), 1.0e-4f));
}

TEST(TressfxSolverMath, WindGustIsBoundedPeriodicAndStrandPhased)
{
    const float gust = TressfxWindGustFactor(0.7f, 0.4f, 5, 0.37f);
    EXPECT_GE(gust, 1.0f - 0.7f - 1.0e-4f);
    EXPECT_LE(gust, 1.0f + 0.7f + 1.0e-4f);
    EXPECT_NEAR(gust, TressfxWindGustFactor(0.7f, 0.4f, 5, 0.37f + 2.5f), 2.0e-4f);
    EXPECT_NE(TressfxWindGustFactor(0.7f, 0.4f, 5, 0.37f),
        TressfxWindGustFactor(0.7f, 0.4f, 6, 0.37f));
}
