#include <gtest/gtest.h>

#include <cmath>
#include <limits>

#include <Eigen/Dense>

#include "render/shadow/shadowCasterCull.h"

namespace
{

// 与 Camera::SetOrthographic / ndcMatrix 的 Vulkan 约定一致的 light view-projection：
// 光源空间 x/y 在 [-halfSize, halfSize]，z 在 [-zFar, 0]（相机朝 -z），
// 映射到 clip space x/y ∈ [-w, w]、z ∈ [0, w]。
Eigen::Matrix4f MakeLightViewProjection(
    float halfSize = 50.0f,
    float zFar = 200.0f)
{
    Eigen::Matrix4f matrix = Eigen::Matrix4f::Identity();
    matrix(0, 0) = 1.0f / halfSize;
    matrix(1, 1) = 1.0f / halfSize;
    matrix(2, 2) = -1.0f / zFar;
    return matrix;
}

VL::ShadowCasterCullFrustum MakeFrustum()
{
    VL::ShadowCasterCullFrustum frustum;
    EXPECT_TRUE(frustum.Build(MakeLightViewProjection()));
    return frustum;
}

const Eigen::Vector3f kInsideMin(-40.0f, -40.0f, -100.0f);
const Eigen::Vector3f kInsideMax(40.0f, 40.0f, -10.0f);

} // namespace

TEST(ShadowCasterCullTest, BuildsValidFrustumFromLightViewProjection)
{
    VL::ShadowCasterCullFrustum frustum;
    EXPECT_TRUE(frustum.Build(MakeLightViewProjection()));
    EXPECT_TRUE(frustum.IsValid());
}

TEST(ShadowCasterCullTest, KeepsBoundsInsideFrustum)
{
    const VL::ShadowCasterCullFrustum frustum = MakeFrustum();
    EXPECT_TRUE(frustum.IntersectsWorldBounds(kInsideMin, kInsideMax));
}

TEST(ShadowCasterCullTest, CullsBoundsOutsideLeftPlane)
{
    const VL::ShadowCasterCullFrustum frustum = MakeFrustum();
    EXPECT_FALSE(frustum.IntersectsWorldBounds(
        Eigen::Vector3f(-100.0f, -40.0f, -100.0f),
        Eigen::Vector3f(-60.0f, 40.0f, -10.0f)));
}

TEST(ShadowCasterCullTest, CullsBoundsOutsideRightPlane)
{
    const VL::ShadowCasterCullFrustum frustum = MakeFrustum();
    EXPECT_FALSE(frustum.IntersectsWorldBounds(
        Eigen::Vector3f(60.0f, -40.0f, -100.0f),
        Eigen::Vector3f(100.0f, 40.0f, -10.0f)));
}

TEST(ShadowCasterCullTest, CullsBoundsOutsideBottomPlane)
{
    const VL::ShadowCasterCullFrustum frustum = MakeFrustum();
    EXPECT_FALSE(frustum.IntersectsWorldBounds(
        Eigen::Vector3f(-40.0f, -100.0f, -100.0f),
        Eigen::Vector3f(40.0f, -60.0f, -10.0f)));
}

TEST(ShadowCasterCullTest, CullsBoundsOutsideTopPlane)
{
    const VL::ShadowCasterCullFrustum frustum = MakeFrustum();
    EXPECT_FALSE(frustum.IntersectsWorldBounds(
        Eigen::Vector3f(-40.0f, 60.0f, -100.0f),
        Eigen::Vector3f(40.0f, 100.0f, -10.0f)));
}

TEST(ShadowCasterCullTest, CullsBoundsBehindLightNearPlane)
{
    const VL::ShadowCasterCullFrustum frustum = MakeFrustum();
    EXPECT_FALSE(frustum.IntersectsWorldBounds(
        Eigen::Vector3f(-40.0f, -40.0f, 10.0f),
        Eigen::Vector3f(40.0f, 40.0f, 20.0f)));
}

TEST(ShadowCasterCullTest, CullsBoundsBeyondLightFarPlane)
{
    const VL::ShadowCasterCullFrustum frustum = MakeFrustum();
    EXPECT_FALSE(frustum.IntersectsWorldBounds(
        Eigen::Vector3f(-40.0f, -40.0f, -300.0f),
        Eigen::Vector3f(40.0f, 40.0f, -250.0f)));
}

TEST(ShadowCasterCullTest, KeepsBoundsStraddlingEachPlane)
{
    const VL::ShadowCasterCullFrustum frustum = MakeFrustum();
    const Eigen::Vector3f straddling[][2] = {
        { Eigen::Vector3f(-60.0f, -40.0f, -100.0f), Eigen::Vector3f(-40.0f, 40.0f, -10.0f) },
        { Eigen::Vector3f(40.0f, -40.0f, -100.0f), Eigen::Vector3f(60.0f, 40.0f, -10.0f) },
        { Eigen::Vector3f(-40.0f, -60.0f, -100.0f), Eigen::Vector3f(40.0f, -40.0f, -10.0f) },
        { Eigen::Vector3f(-40.0f, 40.0f, -100.0f), Eigen::Vector3f(40.0f, 60.0f, -10.0f) },
        { Eigen::Vector3f(-40.0f, -40.0f, -10.0f), Eigen::Vector3f(40.0f, 40.0f, 10.0f) },
        { Eigen::Vector3f(-40.0f, -40.0f, -250.0f), Eigen::Vector3f(40.0f, 40.0f, -190.0f) }
    };

    for (const auto& bounds : straddling)
    {
        EXPECT_TRUE(frustum.IntersectsWorldBounds(bounds[0], bounds[1]));
    }
}

TEST(ShadowCasterCullTest, KeepsBoundsTouchingPlane)
{
    const VL::ShadowCasterCullFrustum frustum = MakeFrustum();
    // 恰好贴住 left 平面（x = -50）：相切不允许剔除。
    EXPECT_TRUE(frustum.IntersectsWorldBounds(
        Eigen::Vector3f(-50.0f, -40.0f, -100.0f),
        Eigen::Vector3f(0.0f, 40.0f, -10.0f)));
}

TEST(ShadowCasterCullTest, KeepsDegenerateAndInvertedBounds)
{
    const VL::ShadowCasterCullFrustum frustum = MakeFrustum();
    EXPECT_TRUE(frustum.IntersectsWorldBounds(
        Eigen::Vector3f(0.0f, 0.0f, -10.0f),
        Eigen::Vector3f(0.0f, 0.0f, -10.0f)));
    // min > max 的空范围没有可信几何，必须保留。
    EXPECT_TRUE(frustum.IntersectsWorldBounds(
        Eigen::Vector3f(60.0f, 40.0f, -10.0f),
        Eigen::Vector3f(-60.0f, -40.0f, -100.0f)));
    // 退化点完全在视锥外仍可按正常 AABB 剔除。
    EXPECT_FALSE(frustum.IntersectsWorldBounds(
        Eigen::Vector3f(100.0f, 0.0f, -10.0f),
        Eigen::Vector3f(100.0f, 0.0f, -10.0f)));
}

TEST(ShadowCasterCullTest, KeepsBoundsWhenFrustumIsInvalid)
{
    VL::ShadowCasterCullFrustum frustum;
    EXPECT_FALSE(frustum.Build(Eigen::Matrix4f::Zero()));
    EXPECT_FALSE(frustum.IsValid());
    EXPECT_TRUE(frustum.IntersectsWorldBounds(kInsideMin, kInsideMax));

    Eigen::Matrix4f nonFinite = MakeLightViewProjection();
    nonFinite(0, 3) = std::numeric_limits<float>::quiet_NaN();
    VL::ShadowCasterCullFrustum nanFrustum;
    EXPECT_FALSE(nanFrustum.Build(nonFinite));
    EXPECT_TRUE(nanFrustum.IntersectsWorldBounds(kInsideMin, kInsideMax));
}

TEST(ShadowCasterCullTest, KeepsNonFiniteBounds)
{
    const VL::ShadowCasterCullFrustum frustum = MakeFrustum();
    const float nan = std::numeric_limits<float>::quiet_NaN();
    const float infinity = std::numeric_limits<float>::infinity();
    EXPECT_TRUE(frustum.IntersectsWorldBounds(
        Eigen::Vector3f(nan, -40.0f, -100.0f), kInsideMax));
    EXPECT_TRUE(frustum.IntersectsWorldBounds(
        kInsideMin, Eigen::Vector3f(infinity, 40.0f, -10.0f)));
}

TEST(ShadowCasterCullTest, PaddingKeepsBoundsJustOutsidePlane)
{
    const VL::ShadowCasterCullFrustum frustum = MakeFrustum();
    const Eigen::Vector3f outsideMin(52.0f, -40.0f, -100.0f);
    const Eigen::Vector3f outsideMax(60.0f, 40.0f, -10.0f);
    EXPECT_FALSE(frustum.IntersectsWorldBounds(outsideMin, outsideMax));
    // 沿世界轴外扩 3 cm 后重新与 right 平面相交。
    EXPECT_TRUE(frustum.IntersectsWorldBounds(outsideMin, outsideMax, 3.0f));
}

TEST(ShadowCasterCullTest, SkinnedPoseBoundsReplaceBindPoseBounds)
{
    const VL::ShadowCasterCullFrustum frustum = MakeFrustum();
    // bind pose 的 AABB 完全在 left 平面外；当前 skinPose 把它拉到视锥内。
    // culler 只消费调用方给的当前世界包围盒，因此必须跟随 skinPose。
    const Eigen::Vector3f bindPoseMin(-200.0f, -40.0f, -100.0f);
    const Eigen::Vector3f bindPoseMax(-150.0f, 40.0f, -10.0f);
    const Eigen::Vector3f skinnedPoseMin(-20.0f, -40.0f, -100.0f);
    const Eigen::Vector3f skinnedPoseMax(20.0f, 40.0f, -10.0f);

    EXPECT_FALSE(frustum.IntersectsWorldBounds(bindPoseMin, bindPoseMax));
    EXPECT_TRUE(frustum.IntersectsWorldBounds(skinnedPoseMin, skinnedPoseMax));
}

namespace
{

VL::ShadowCasterReceiverBounds MakeReceiverBounds()
{
    VL::ShadowCasterReceiverBounds receiver;
    // 光源空间与世界空间一致，接收面 XY 范围 [0,0]..[100,100]。
    receiver.lightSpaceXyMinMax = Eigen::Vector4f(0.0f, 0.0f, 100.0f, 100.0f);
    receiver.valid = true;
    return receiver;
}

} // namespace

TEST(ShadowCasterCullTest, ReceiverXyKeepsOverlappingBounds)
{
    const VL::ShadowCasterReceiverBounds receiver = MakeReceiverBounds();
    EXPECT_TRUE(VL::ShadowCasterBoundsTouchReceiverLightSpaceXy(
        Eigen::Vector3f(10.0f, 10.0f, -50.0f),
        Eigen::Vector3f(90.0f, 90.0f, 50.0f),
        receiver));
}

TEST(ShadowCasterCullTest, ReceiverXyCullsDisjointBounds)
{
    const VL::ShadowCasterReceiverBounds receiver = MakeReceiverBounds();
    EXPECT_FALSE(VL::ShadowCasterBoundsTouchReceiverLightSpaceXy(
        Eigen::Vector3f(-50.0f, 10.0f, -50.0f),
        Eigen::Vector3f(-10.0f, 90.0f, 50.0f),
        receiver));
    EXPECT_FALSE(VL::ShadowCasterBoundsTouchReceiverLightSpaceXy(
        Eigen::Vector3f(10.0f, 150.0f, -50.0f),
        Eigen::Vector3f(90.0f, 200.0f, 50.0f),
        receiver));
}

TEST(ShadowCasterCullTest, ReceiverXyKeepsStraddlingBounds)
{
    const VL::ShadowCasterReceiverBounds receiver = MakeReceiverBounds();
    EXPECT_TRUE(VL::ShadowCasterBoundsTouchReceiverLightSpaceXy(
        Eigen::Vector3f(-10.0f, 40.0f, -50.0f),
        Eigen::Vector3f(10.0f, 60.0f, 50.0f),
        receiver));
}

TEST(ShadowCasterCullTest, ReceiverXyPaddingKeepsBoundsJustOutside)
{
    const VL::ShadowCasterReceiverBounds receiver = MakeReceiverBounds();
    const Eigen::Vector3f min(-8.0f, 40.0f, -50.0f);
    const Eigen::Vector3f max(-4.0f, 60.0f, 50.0f);
    EXPECT_FALSE(VL::ShadowCasterBoundsTouchReceiverLightSpaceXy(min, max, receiver));
    EXPECT_TRUE(VL::ShadowCasterBoundsTouchReceiverLightSpaceXy(
        min, max, receiver, 5.0f));
}

TEST(ShadowCasterCullTest, ReceiverXyKeepsDegenerateOrNonFiniteInput)
{
    const VL::ShadowCasterReceiverBounds receiver = MakeReceiverBounds();
    const float nan = std::numeric_limits<float>::quiet_NaN();
    EXPECT_TRUE(VL::ShadowCasterBoundsTouchReceiverLightSpaceXy(
        Eigen::Vector3f(60.0f, 60.0f, -10.0f),
        Eigen::Vector3f(-60.0f, -60.0f, -100.0f),
        receiver));
    EXPECT_TRUE(VL::ShadowCasterBoundsTouchReceiverLightSpaceXy(
        Eigen::Vector3f(nan, 0.0f, 0.0f),
        Eigen::Vector3f(10.0f, 10.0f, 10.0f),
        receiver));
}
