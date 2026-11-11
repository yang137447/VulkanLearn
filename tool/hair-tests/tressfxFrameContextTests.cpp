#include <gtest/gtest.h>

#include <Eigen/Geometry>

#include "hair/tressfxFrameContext.h"
#include "hair/tressfxSkinning.h"
#include "hair/tressfxSimulationParams.h"

namespace VL::Hair
{
namespace
{
Eigen::Matrix4f Placement(
    const Eigen::Vector3f& translation, const Eigen::Vector3f& axis, float radians)
{
    Eigen::Affine3f transform = Eigen::Affine3f::Identity();
    transform.translate(translation);
    transform.rotate(Eigen::AngleAxisf(radians, axis));
    return transform.matrix();
}
}

TEST(TressfxFrameContext, RootMatchesSourceWorldPositionAcrossDifferentPlacements)
{
    const Eigen::Matrix4f hairModel = Placement(
        {12.0f, -7.0f, 3.0f}, Eigen::Vector3f::UnitY(), 0.7f);
    const Eigen::Matrix4f sourceModel = Placement(
        {-25.0f, 9.0f, 34.0f}, Eigen::Vector3f::UnitZ(), -0.4f);
    Animation::SkinPose pose;
    pose.bindToPoseMatrices.push_back(Placement(
        {1.0f, 2.0f, -3.0f}, Eigen::Vector3f::UnitX(), 0.3f));
    const Eigen::Vector4f rest(2.0f, 150.0f, -4.0f, 1.0f);

    const TressfxFrameContext frame = BuildTressfxFrameContext(
        hairModel, sourceModel, pose, 0, 0.008f);

    const Eigen::Vector4f expected = sourceModel * pose.bindToPoseMatrices[0] * rest;
    const Eigen::Vector4f actual = hairModel * frame.skinningMatrix * rest;
    EXPECT_TRUE(actual.isApprox(expected, 1.0e-5f));
}

TEST(TressfxFrameContext, RootCapsulesAndProxyShareRelativeFullSkeletonPalette)
{
    const Eigen::Matrix4f hairModel = Placement(
        {30.0f, 2.0f, -10.0f}, Eigen::Vector3f::UnitY(), -0.6f);
    const Eigen::Matrix4f sourceModel = Placement(
        {90.0f, -4.0f, 20.0f}, Eigen::Vector3f::UnitY(), 0.8f);
    Animation::SkinPose pose;
    pose.bindToPoseMatrices = {
        Placement({0.0f, 4.0f, 1.0f}, Eigen::Vector3f::UnitX(), 0.2f),
        Placement({2.0f, 1.0f, 0.0f}, Eigen::Vector3f::UnitZ(), 0.4f)};
    // section palette 故意不同，防止碰撞误用未参与该 section 的关节矩阵。
    pose.matrices.assign(2, Eigen::Matrix4f::Identity());
    const TressfxFrameContext frame = BuildTressfxFrameContext(
        hairModel, sourceModel, pose, 1, 0.016f);
    ASSERT_EQ(frame.jointMatrices.size(), 2u);
    EXPECT_TRUE(frame.skinningMatrix.isApprox(frame.jointMatrices[1]));
    TressfxCollider bound;
    bound.a = {1.0f, 20.0f, 3.0f};
    bound.b = {4.0f, 25.0f, 6.0f};
    bound.radius = 2.0f;
    TressfxCollider unbound = bound;
    const std::vector<TressfxCollider> capsules =
        TransformTressfxCapsulesByBones({bound, unbound}, {0, -1},
            frame.jointMatrices);
    const Eigen::Vector4f expectedA =
        sourceModel * pose.bindToPoseMatrices[0] * bound.a.homogeneous();
    EXPECT_TRUE((hairModel * capsules[0].a.homogeneous()).isApprox(expectedA, 1.0e-5f));
    EXPECT_TRUE(capsules[1].a.isApprox(unbound.a));
    EXPECT_FLOAT_EQ(capsules[0].radius, bound.radius);

    const Eigen::Vector4f proxyPoint(3.0f, 24.0f, -2.0f, 1.0f);
    const Eigen::Vector4f expectedProxy = sourceModel *
        (0.25f * pose.bindToPoseMatrices[0] + 0.75f * pose.bindToPoseMatrices[1]) *
        proxyPoint;
    const Eigen::Vector4f actualProxy = hairModel *
        (0.25f * frame.jointMatrices[0] + 0.75f * frame.jointMatrices[1]) *
        proxyPoint;
    EXPECT_TRUE(actualProxy.isApprox(expectedProxy, 1.0e-5f));
}

TEST(TressfxFrameContext, LegacyPoseUsesSectionPaletteAndPreservesFrozenIdentity)
{
    Animation::SkinPose pose;
    pose.matrices.push_back(Placement(
        {4.0f, 8.0f, 12.0f}, Eigen::Vector3f::UnitY(), 0.3f));
    pose.playbackIdentity = 17;
    pose.discontinuity = 6;
    const TressfxFrameContext frame = BuildTressfxFrameContext(
        Eigen::Matrix4f::Identity(), Eigen::Matrix4f::Identity(), pose, 0, 0.0f);

    EXPECT_TRUE(frame.skinningMatrix.isApprox(pose.matrices[0]));
    EXPECT_FLOAT_EQ(frame.deltaSeconds, 0.0f);
    EXPECT_EQ(frame.playbackIdentity, 17u);
    EXPECT_EQ(frame.discontinuity, 6u);
    pose.matrices[0].setIdentity();
    pose.discontinuity = 7;
    EXPECT_FALSE(frame.skinningMatrix.isApprox(pose.matrices[0]));
    EXPECT_EQ(frame.discontinuity, 6u);
}

TEST(TressfxFrameContext, SkinningOnlyConsumesNewPoseAtZeroSimulationDelta)
{
    const Eigen::Matrix4f hairModel = Placement(
        {12.0f, 3.0f, -7.0f}, Eigen::Vector3f::UnitY(), 0.4f);
    const Eigen::Matrix4f sourceModel = Placement(
        {-4.0f, 2.0f, 9.0f}, Eigen::Vector3f::UnitZ(), -0.2f);
    Animation::SkinPose pose;
    pose.bindToPoseMatrices.push_back(Eigen::Matrix4f::Identity());
    TressfxFrameContext frame;
    UpdateTressfxFrameContext(frame, hairModel, sourceModel, pose, 0, 0.0f);
    const auto before = MakeGuideSkinningParams(1, 4, frame.skinningMatrix, false);
    pose.bindToPoseMatrices[0] = Placement(
        {2.0f, 7.0f, -3.0f}, Eigen::Vector3f::UnitX(), 0.6f);
    UpdateTressfxFrameContext(frame, hairModel, sourceModel, pose, 0, 0.0f);
    const auto after = MakeGuideSkinningParams(1, 4, frame.skinningMatrix, false);
    // 同时检查根和梢；off 不能退化为只更新固定根点。
    for (const Eigen::Vector4f& rest :
        {Eigen::Vector4f(1, 150, 2, 1), Eigen::Vector4f(5, 130, -3, 1)})
    {
        EXPECT_FALSE((before.skinning * rest).isApprox(after.skinning * rest));
        EXPECT_TRUE((hairModel * after.skinning * rest).isApprox(
            sourceModel * pose.bindToPoseMatrices[0] * rest, 1.0e-5f));
    }
    EXPECT_FLOAT_EQ(after.controls.x(), 0.0f);
}

TEST(TressfxFrameContext, SkinningOnlyParamsDoNotActivateSolverOrCollision)
{
    Eigen::Matrix4f root = Eigen::Matrix4f::Identity();
    root(1, 3) = 17.0f;
    const auto params = MakeGuideSkinningParams(3, 8, root, true);
    EXPECT_EQ(params.guideCount, 3u);
    EXPECT_EQ(params.pointCount, 24u);
    EXPECT_FLOAT_EQ(params.controls.z(), 8.0f);
    EXPECT_TRUE(params.skinning.isApprox(root));
    EXPECT_FLOAT_EQ(params.motion.w(), 4.0f);
    EXPECT_FLOAT_EQ(MakeGuideSkinningParams(3, 8, root, false).motion.w(), 0.0f);
    EXPECT_EQ(params.iterations, 0u);
    EXPECT_EQ(params.colliderCount, 0u);
    EXPECT_TRUE(params.gravity.isZero());
    EXPECT_TRUE(params.wind0.isZero());
    EXPECT_TRUE(params.wind1.isZero());
    EXPECT_TRUE(params.wind2.isZero());
    EXPECT_TRUE(params.wind3.isZero());
    EXPECT_TRUE(params.shape.isZero());
    EXPECT_TRUE(params.motion.head<3>().isZero());
}

TEST(TressfxFrameClock, RepeatedRenderSceneDoesNotAdvanceSimulationTwice)
{
    TressfxFrameClock clock;
    EXPECT_FLOAT_EQ(clock.DeltaForFrame(3, 27, 0.01f), 0.01f);
    EXPECT_FLOAT_EQ(clock.DeltaForFrame(3, 27, 0.01f), 0.01f);
    EXPECT_FLOAT_EQ(clock.Consume(3, 27, 0.01f), 0.01f);
    EXPECT_FLOAT_EQ(clock.DeltaForFrame(3, 27, 0.01f), 0.0f);
    EXPECT_FLOAT_EQ(clock.Consume(3, 27, 0.01f), 0.0f);
    EXPECT_FLOAT_EQ(clock.Consume(3, 28, 0.01f), 0.01f);
}

TEST(TressfxFrameClock, PausedFramesResumeWithTheirOwnDelta)
{
    TressfxFrameClock clock;
    EXPECT_FLOAT_EQ(clock.Consume(3, 0, 0.016f), 0.016f);
    EXPECT_FLOAT_EQ(clock.Consume(3, 1, 0.0f), 0.0f);
    EXPECT_FLOAT_EQ(clock.Consume(3, 2, 0.0f), 0.0f);
    EXPECT_FLOAT_EQ(clock.Consume(3, 3, 0.004f), 0.004f);
}

TEST(TressfxFrameClock, NewWorldAndLatestFrameUseOnlyTheirFrozenDelta)
{
    TressfxFrameClock clock;
    EXPECT_FLOAT_EQ(clock.Consume(3, 0, 0.01f), 0.01f);
    EXPECT_FLOAT_EQ(clock.Consume(4, 0, 0.002f), 0.002f);
    // 未消费的 GT 帧不补算，避免使用最新姿态重复模拟已过期的输入。
    EXPECT_FLOAT_EQ(clock.Consume(4, 5, 0.008f), 0.008f);
}
}
