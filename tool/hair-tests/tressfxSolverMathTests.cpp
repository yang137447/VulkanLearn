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

TEST(TressfxSolverMath, PhysicalDragIsDownwindAndIndependentOfSegmentLength)
{
    const Eigen::Vector3f wind(0.0f, 1.0f, 0.0f);
    const float timeStep = 0.0125f;
    const float dragRate = TressfxHairDragRatePerSpeed(1.0f, 0.012f);
    ASSERT_GT(dragRate, 0.0f);

    const Eigen::Vector3f shortCurrent(0.0f, 0.0f, 0.0f);
    const Eigen::Vector3f nextShort(1.0f, 0.0f, 0.0f);
    const Eigen::Vector3f shortDisplacement = TressfxDragDisplacement(
        shortCurrent, shortCurrent, nextShort, wind, timeStep, dragRate);
    EXPECT_GT(shortDisplacement.y(), 0.0f);
    EXPECT_NEAR(shortDisplacement.x(), 0.0f, 1.0e-6f);
    EXPECT_NEAR(shortDisplacement.z(), 0.0f, 1.0e-6f);

    // 圆柱阻力和段质量都正比于段长；加速度与段长无关，同一法向风速下
    // 1 cm 与 5 cm 段得到相同的位移。
    const Eigen::Vector3f nextLong(5.0f, 0.0f, 0.0f);
    const Eigen::Vector3f longDisplacement = TressfxDragDisplacement(
        shortCurrent, shortCurrent, nextLong, wind, timeStep, dragRate);
    EXPECT_TRUE(longDisplacement.isApprox(shortDisplacement, 1.0e-6f));

    // 段与风平行时没有法向相对速度，不应产生阻力位移。
    const Eigen::Vector3f parallelWind(1.0f, 0.0f, 0.0f);
    EXPECT_TRUE(TressfxDragDisplacement(shortCurrent, shortCurrent, nextShort,
        parallelWind, timeStep, dragRate).isZero());
}

TEST(TressfxSolverMath, PhysicalDragUsesPreviousPositionForRelativeVelocity)
{
    const Eigen::Vector3f wind(0.0f, 100.0f, 0.0f);
    const float timeStep = 0.0125f;
    const float dragRate = TressfxHairDragRatePerSpeed(1.0f, 0.012f);
    const Eigen::Vector3f current(0.0f, 0.0f, 0.0f);
    const Eigen::Vector3f next(1.0f, 0.0f, 0.0f);

    const Eigen::Vector3f rest = TressfxDragDisplacement(
        current, current, next, wind, timeStep, dragRate);
    // 点已经以 50 cm/s 随空气运动时，相对速度只剩一半，阻力位移更小。
    const Eigen::Vector3f moving = TressfxDragDisplacement(
        current, current - Eigen::Vector3f(0.0f, 50.0f, 0.0f) * timeStep,
        next, wind, timeStep, dragRate);
    EXPECT_GT(rest.norm(), moving.norm());

    // windGain 为零时整条阻力链关闭。
    EXPECT_TRUE(TressfxDragDisplacement(current, current, next, wind,
        timeStep, TressfxHairDragRatePerSpeed(0.0f, 0.012f)).isZero());
}

TEST(TressfxSolverMath, DampingCoefficientIsUsedDirectlyInExponentialDecay)
{
    // damping 就是 DampingCoeff：60 Hz 一步的保留率是 exp(-damping)，没有 -ln 变换。
    for (const float damping : {0.0f, 0.02f, 0.035f, 0.05f, 0.5f})
    {
        EXPECT_NEAR(TressfxDampingDecay(damping, 1.0f / 60.0f),
            std::exp(-damping), 1.0e-6f);
    }
    // 半帧 ⇒ 指数衰减的平方根；两帧 ⇒ 平方。
    const float halfFrame = TressfxDampingDecay(0.035f, 1.0f / 120.0f);
    EXPECT_NEAR(halfFrame * halfFrame, std::exp(-0.035f), 1.0e-6f);
    const float twoFrames = TressfxDampingDecay(0.035f, 2.0f / 60.0f);
    EXPECT_NEAR(twoFrames, std::exp(-0.07f), 1.0e-6f);
    // 0.995 若仍被当作保留比例，一步衰减会是 0.995；作为系数时是 exp(-0.995)。
    EXPECT_NEAR(TressfxDampingDecay(0.995f, 1.0f / 60.0f),
        std::exp(-0.995f), 1.0e-6f);
}

TEST(TressfxSolverMath, WindCornersAreFourDistinctFortyDegreeConeDirections)
{
    // +X 风、frame 0：wM = 1000 * (sin(0)² + 0.5) = 500。
    const Eigen::Vector3f direction = Eigen::Vector3f::UnitX();
    const float magnitude = 1000.0f;
    const TressfxWindCorners corners =
        TressfxBuildWindCorners(direction, magnitude, 0);
    const float wM = 500.0f;
    const float cosine = std::cos(TressfxWindConeAngleRadians);
    const Eigen::Vector3f* winds[4] = {&corners.wind0, &corners.wind1,
        &corners.wind2, &corners.wind3};
    for (int i = 0; i < 4; ++i)
    {
        EXPECT_NEAR(winds[i]->norm(), wM, 1.0e-3f);
        // 每个角都在以 +X 为轴、40° 的锥面上。
        EXPECT_NEAR(winds[i]->normalized().dot(direction), cosine, 1.0e-6f);
        for (int j = i + 1; j < 4; ++j)
            EXPECT_GT((*winds[i] - *winds[j]).norm(), 1.0e-3f);
    }
    // 绕 Y 的一对只差 Z 分量符号；绕 Z 的一对只差 Y 分量符号。
    EXPECT_NEAR(corners.wind0.z(), -corners.wind1.z(), 1.0e-5f);
    EXPECT_NEAR(corners.wind0.y(), corners.wind1.y(), 1.0e-5f);
    EXPECT_NEAR(corners.wind2.y(), -corners.wind3.y(), 1.0e-5f);
    EXPECT_NEAR(corners.wind2.z(), corners.wind3.z(), 1.0e-5f);
}

TEST(TressfxSolverMath, WindCornerMagnitudeFollowsTressFxFrameModulation)
{
    // frame = 1000 ⇒ sin(1)² + 0.5；四路幅值都按 wM 缩放。
    const float modulation = std::sin(1.0f) * std::sin(1.0f) + 0.5f;
    const TressfxWindCorners corners =
        TressfxBuildWindCorners(Eigen::Vector3f::UnitX(), 2000.0f, 1000);
    EXPECT_NEAR(corners.wind0.norm(), 2000.0f * modulation, 1.0e-2f);
    EXPECT_NEAR(corners.wind3.norm(), 2000.0f * modulation, 1.0e-2f);
    EXPECT_NEAR(TressfxWindFrameModulation(0), 0.5f, 1.0e-6f);
    // 零幅值返回零向量，Host 不会把 NaN 方向写进 UBO。
    const TressfxWindCorners zero =
        TressfxBuildWindCorners(Eigen::Vector3f::Zero(), 0.0f, 7);
    EXPECT_TRUE(zero.wind0.isZero());
    EXPECT_TRUE(zero.wind3.isZero());
}

TEST(TressfxSolverMath, RuntimeWindCornersSkipTressFxFrameModulation)
{
    // 运行时重载（无 frame 参数）直接使用传入幅值：场景阵风已经负责时间调制。
    const TressfxWindCorners runtime =
        TressfxBuildWindCorners(Eigen::Vector3f::UnitX(), 2000.0f);
    EXPECT_NEAR(runtime.wind0.norm(), 2000.0f, 1.0e-3f);
    EXPECT_NEAR(runtime.wind3.norm(), 2000.0f, 1.0e-3f);
    // legacy/SDK 对照重载才乘 sin(frame*0.001)²+0.5：frame 0 即 0.5 倍。
    const TressfxWindCorners legacy =
        TressfxBuildWindCorners(Eigen::Vector3f::UnitX(), 2000.0f, 0);
    EXPECT_NEAR(legacy.wind0.norm(), 1000.0f, 1.0e-3f);
    // 零幅值在两种入口下都返回零向量，Host 不会把零向量归一化成 NaN。
    const TressfxWindCorners zero =
        TressfxBuildWindCorners(Eigen::Vector3f::Zero(), 0.0f);
    EXPECT_TRUE(zero.wind0.isZero());
    EXPECT_TRUE(zero.wind3.isZero());
}

TEST(TressfxSolverMath, WindMixerUsesAlternatingWeightsAndStrandPhase)
{
    // w = (a*W0 + (1-a)*W1 + a*W2 + (1-a)*W3) / (2*cos40°)。
    // 权重和是 2，纵向分量 2*cos40°，归一化后场景风速幅值保持不变。
    const Eigen::Vector3f w0(1.0f, 0.0f, 0.0f);
    const Eigen::Vector3f w1(0.0f, 2.0f, 0.0f);
    const Eigen::Vector3f w2(0.0f, 0.0f, 4.0f);
    const Eigen::Vector3f w3(8.0f, 0.0f, 0.0f);
    const float scale = TressfxWindConeMixScale;
    // guide 0 ⇒ a = 0：只保留 W1 与 W3。
    EXPECT_TRUE(TressfxCombineWind(w0, w1, w2, w3, 0)
            .isApprox(scale * (w1 + w3)));
    // guide 10 ⇒ a = 0.5：W0/W2 各一半，W1/W3 各一半。
    EXPECT_TRUE(TressfxCombineWind(w0, w1, w2, w3, 10)
            .isApprox(scale * 0.5f * (w0 + w2 + w1 + w3)));
    // guide 19 ⇒ a = 0.95。
    EXPECT_TRUE(TressfxCombineWind(w0, w1, w2, w3, 19)
            .isApprox(scale * (0.95f * (w0 + w2) + 0.05f * (w1 + w3))));
    // 四路相同向量时权重和是 2；物理解释只对真实锥形方向成立。
    EXPECT_TRUE(TressfxCombineWind(w0, w0, w0, w0, 7).isApprox(2.0f * scale * w0));
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

TEST(TressfxSolverMath, VspStiffnessUsesAuthoredArrayAtEveryPoint)
{
    // 源第 i 项 = 曲线在 i/N 处的采样值；作者 5 项覆盖 local 0..4。
    const std::vector<float> authored = {0.0f, 0.2f, 0.5f, 0.75f, 1.0f};
    EXPECT_FLOAT_EQ(TressfxVspStiffness(0, 5, 2, &authored), 0.0f);
    EXPECT_FLOAT_EQ(TressfxVspStiffness(2, 5, 2, &authored), 0.5f);
    EXPECT_FLOAT_EQ(TressfxVspStiffness(4, 5, 2, &authored), 1.0f);
    // 数组不覆盖的 local 仍回退到旧的线性近似。
    EXPECT_NEAR(TressfxVspStiffness(5, 10, 2, &authored),
        1.0f - 3.0f / 7.0f, kTolerance);
}

TEST(TressfxSolverMath, VspCoefficientOnlyMultipliesCurveBelowPointNine)
{
    const std::vector<float> authored = {0.0f, 0.5f, 0.0f, 0.0f};
    // 源只在 VspCoeff < 0.9 时乘逐顶点 stiffness（严格小于）。
    EXPECT_FLOAT_EQ(TressfxVspCoefficient(0.8f, 1, 4, 2, &authored), 0.4f);
    EXPECT_FLOAT_EQ(TressfxVspCoefficient(0.9f, 1, 4, 2, &authored), 0.9f);
    EXPECT_FLOAT_EQ(TressfxVspCoefficient(1.0f, 1, 4, 2, &authored), 1.0f);
    // 未作者时用线性 fallback；local 3 / 4 points / root 2 → span = 1 → 0。
    EXPECT_FLOAT_EQ(TressfxVspCoefficient(0.5f, 3, 4, 2), 0.0f);
}

TEST(TressfxSolverMath, LocalShapeStiffnessUsesAuthoredArrayPerPoint)
{
    const std::vector<float> authored = {0.0f, 0.25f, 0.5f, 0.95f};
    for (uint32_t local = 0; local < authored.size(); ++local)
        EXPECT_FLOAT_EQ(TressfxLocalShapeStiffness(local, 0.3f, &authored),
            authored[local]);
    // 数组不覆盖时回退到作者标量 bendStiffness。
    EXPECT_FLOAT_EQ(TressfxLocalShapeStiffness(9, 0.3f, &authored), 0.3f);
    EXPECT_FLOAT_EQ(TressfxLocalShapeStiffness(1, 0.3f), 0.3f);
}

TEST(TressfxSolverMath, LocalShapeDeltaScalesLinearlyWithPerPointStiffness)
{
    // TressfxLocalShapeDelta 内部固定乘 0.5；曲线值只线性缩放这个 del。
    const Eigen::Vector4f identity(0.0f, 0.0f, 0.0f, 1.0f);
    const Eigen::Vector3f full = TressfxLocalShapeDelta(identity,
        Eigen::Vector3f(0, 0, 0), Eigen::Vector3f(1, 0, 0),
        Eigen::Vector3f(1, 1, 0), Eigen::Vector3f(0, 0, 0),
        Eigen::Vector3f(1, 0, 0), Eigen::Vector3f(2, 0, 0), 1.0f);
    const Eigen::Vector3f half = TressfxLocalShapeDelta(identity,
        Eigen::Vector3f(0, 0, 0), Eigen::Vector3f(1, 0, 0),
        Eigen::Vector3f(1, 1, 0), Eigen::Vector3f(0, 0, 0),
        Eigen::Vector3f(1, 0, 0), Eigen::Vector3f(2, 0, 0), 0.5f);
    EXPECT_TRUE(half.isApprox(0.5f * full, 1.0e-6f));
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

TEST(TressfxSolverMath, VspThresholdPreservesMeterComparisonInCentimeters)
{
    constexpr float sourceThresholdMeters = 1.4f;
    constexpr float baseCoefficient = 0.35f;
    // 源 float4 的 w 在三份历史中相同，二阶差分的 w 为零；这里只比较长度分量。
    // 用阈值两侧及精确边界校验严格 >，不能误成 >=、平方长度或加速度/dt²。
    for (const float displacementMeters : {1.399f, 1.4f, 1.401f})
    {
        SCOPED_TRACE(displacementMeters);
        const Eigen::Vector3f oldRootCm = Eigen::Vector3f::Zero();
        const Eigen::Vector3f oldPointCm(0.0f, 100.0f, 0.0f);
        const Eigen::Vector3f oldOldPointCm(
            100.0f * displacementMeters, 100.0f, 0.0f);
        const TressfxStrandTransform transform = TressfxComputeStrandTransform(
            oldRootCm, oldPointCm, oldRootCm, oldPointCm, oldOldPointCm,
            baseCoefficient, TressfxDefaultVspAccelThresholdMaxCm);
        const float sourceCoefficient = displacementMeters > sourceThresholdMeters
            ? 1.0f : baseCoefficient;
        EXPECT_FLOAT_EQ(transform.coefficient, sourceCoefficient);
    }
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
        30.0f * 30.0f / 40.0f, 1.0e-2f);

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
            .isApprox(matrix.block<3, 3>(0, 0) * Eigen::Vector3f::UnitX(), 1.0e-2f));
}
