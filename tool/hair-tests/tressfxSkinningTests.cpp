#include "hair/tressfxSkinning.h"
#include "hair/tressfxSolverMath.h"

#include <gtest/gtest.h>

#include <stdexcept>
#include <string>
#include <vector>

namespace
{
const std::vector<std::string> JointNames = {
    "FemaleRoot", "Spine1_M", "Spine2_M", "Neck_M", "Head_M", "HeadEnd_M"};
}

TEST(TressfxSkinning, FindsRootJointByName)
{
    EXPECT_EQ(VL::Hair::ResolveTressfxRootJoint(
                  JointNames, "FemaleCharacter", "Head_M", "SM_hair.json"),
        4u);
    EXPECT_EQ(VL::Hair::ResolveTressfxRootJoint(
                  JointNames, "FemaleCharacter", "FemaleRoot", "SM_hair.json"),
        0u);
}

TEST(TressfxSkinning, MissingBoneErrorNamesBoneAndAsset)
{
    try
    {
        VL::Hair::ResolveTressfxRootJoint(
            JointNames, "FemaleCharacter", "Head_NotFound", "Maps/SC_x/Meshes/SM_hair_skinned.json");
        FAIL() << "Missing root bone was accepted";
    }
    catch (const std::runtime_error& error)
    {
        const std::string message = error.what();
        EXPECT_NE(message.find("Head_NotFound"), std::string::npos);
        EXPECT_NE(message.find("FemaleCharacter"), std::string::npos);
        EXPECT_NE(message.find("SM_hair_skinned.json"), std::string::npos);
    }
}

TEST(TressfxSkinning, MissingSkeletonErrorNamesObjectAndAsset)
{
    try
    {
        VL::Hair::ResolveTressfxRootJoint(
            {}, "FemaleCharacter", "Head_M", "Maps/SC_x/Meshes/SM_hair_skinned.json");
        FAIL() << "Source object without a skeleton was accepted";
    }
    catch (const std::runtime_error& error)
    {
        const std::string message = error.what();
        EXPECT_NE(message.find("FemaleCharacter"), std::string::npos);
        EXPECT_NE(message.find("skeleton"), std::string::npos);
        EXPECT_NE(message.find("SM_hair_skinned.json"), std::string::npos);
    }
}

TEST(TressfxSkinning, ImmovablePointsUseSkinningMatrix)
{
    Eigen::Matrix4f matrix = Eigen::Matrix4f::Identity();
    matrix.block<3, 3>(0, 0) =
        Eigen::AngleAxisf(0.5f, Eigen::Vector3f::UnitY()).toRotationMatrix();
    matrix.block<3, 1>(0, 3) = Eigen::Vector3f(11.0f, 22.0f, 33.0f);
    const Eigen::Vector3f restRoot0(1.0f, 2.0f, 3.0f);
    const Eigen::Vector3f restRoot1(4.0f, 5.0f, 6.0f);
    const Eigen::Vector3f restPinnedBeyondPrefix(10.0f, 11.0f, 12.0f);

    // TressFX 的不可移动点（根前缀 ∪ 逆质量 <= 0）整步写 InitialPos = M * rest。
    EXPECT_TRUE(VL::Hair::TressfxInitialPosition(matrix, restRoot0)
            .isApprox((matrix * restRoot0.homogeneous()).head<3>()));
    EXPECT_TRUE(VL::Hair::TressfxInitialPosition(matrix, restRoot1)
            .isApprox((matrix * restRoot1.homogeneous()).head<3>()));
    EXPECT_TRUE(VL::Hair::TressfxInitialPosition(matrix, restPinnedBeyondPrefix)
            .isApprox((matrix * restPinnedBeyondPrefix.homogeneous()).head<3>()));

    // 可移动点不消费上面这条 pin 规则；不可移动判定是根前缀或逆质量 <= 0。
    EXPECT_FALSE(VL::Hair::TressfxIsImmovable(2, 2, 1.0f));
    EXPECT_TRUE(VL::Hair::TressfxIsImmovable(1, 2, 1.0f));
    EXPECT_TRUE(VL::Hair::TressfxIsImmovable(3, 2, 0.0f));
}

TEST(TressfxSkinning, CapsuleJointResolvesByNameAndMissingBoneNamesCapsule)
{
    EXPECT_EQ(VL::Hair::ResolveTressfxJointByName(
                  JointNames, "FemaleCharacter", "Neck_M",
                  "collision.capsules[1].bone", "SM_hair_skinned.json"),
        3u);
    try
    {
        VL::Hair::ResolveTressfxJointByName(
            JointNames, "FemaleCharacter", "Head_NotFound",
            "collision.capsules[2].bone", "Maps/SC_x/Meshes/SM_hair_skinned.json");
        FAIL() << "Unknown capsule bone was accepted";
    }
    catch (const std::runtime_error& error)
    {
        const std::string message = error.what();
        EXPECT_NE(message.find("collision.capsules[2].bone"), std::string::npos);
        EXPECT_NE(message.find("Head_NotFound"), std::string::npos);
        EXPECT_NE(message.find("FemaleCharacter"), std::string::npos);
        EXPECT_NE(message.find("SM_hair_skinned.json"), std::string::npos);
    }
}

TEST(TressfxSkinning, CapsuleEndpointsFollowBoneRotation)
{
    VL::Hair::TressfxCollider capsule;
    capsule.a = Eigen::Vector3f(0.0f, 0.0f, 0.0f);
    capsule.b = Eigen::Vector3f(0.0f, 0.0f, 2.0f);
    capsule.radius = 3.0f;
    capsule.bone = "Head_M";

    Eigen::Matrix4f rotate = Eigen::Matrix4f::Identity();
    // 绕 Y 轴 90°：+Z 端点转到 +X；胶囊应整体随骨骼旋转。
    rotate.block<3, 3>(0, 0) =
        Eigen::AngleAxisf(3.14159265358979323846f * 0.5f,
            Eigen::Vector3f::UnitY()).toRotationMatrix();
    const VL::Hair::TressfxCollider moved =
        VL::Hair::TransformTressfxCapsuleByBone(capsule, rotate);
    EXPECT_TRUE(moved.a.isApprox(Eigen::Vector3f(0.0f, 0.0f, 0.0f)));
    EXPECT_TRUE(moved.b.isApprox(Eigen::Vector3f(2.0f, 0.0f, 0.0f), 1.0e-5f));
    // 没有骨骼缩放提取：radius 原样保留。
    EXPECT_FLOAT_EQ(moved.radius, 3.0f);

    Eigen::Matrix4f translate = Eigen::Matrix4f::Identity();
    translate.block<3, 1>(0, 3) = Eigen::Vector3f(5.0f, 6.0f, 7.0f);
    const VL::Hair::TressfxCollider shifted =
        VL::Hair::TransformTressfxCapsuleByBone(capsule, translate);
    EXPECT_TRUE(shifted.a.isApprox(capsule.a + Eigen::Vector3f(5.0f, 6.0f, 7.0f)));
    EXPECT_TRUE(shifted.b.isApprox(capsule.b + Eigen::Vector3f(5.0f, 6.0f, 7.0f)));
}

TEST(TressfxSkinning, CapsulesWithoutBoneStayInBindSpace)
{
    VL::Hair::TressfxCollider bound;
    bound.a = Eigen::Vector3f(1.0f, 2.0f, 3.0f);
    bound.b = Eigen::Vector3f(4.0f, 5.0f, 6.0f);
    bound.radius = 2.0f;
    bound.bone = "Chest_M";
    VL::Hair::TressfxCollider unbound = bound;
    unbound.bone.clear();
    const std::vector<VL::Hair::TressfxCollider> capsules{bound, unbound};
    const std::vector<int32_t> joints{0, -1};

    std::vector<Eigen::Matrix4f> matrices(1, Eigen::Matrix4f::Identity());
    matrices[0].block<3, 1>(0, 3) = Eigen::Vector3f(10.0f, 0.0f, 0.0f);
    const std::vector<VL::Hair::TressfxCollider> transformed =
        VL::Hair::TransformTressfxCapsulesByBones(capsules, joints, matrices);
    ASSERT_EQ(transformed.size(), 2u);
    EXPECT_TRUE(transformed[0].a.isApprox(bound.a + Eigen::Vector3f(10.0f, 0.0f, 0.0f)));
    EXPECT_TRUE(transformed[1].a.isApprox(unbound.a));
    EXPECT_TRUE(transformed[1].b.isApprox(unbound.b));
    EXPECT_FLOAT_EQ(transformed[1].radius, unbound.radius);
}

TEST(TressfxSkinning, OutOfRangeCapsuleJointKeepsBindSpace)
{
    VL::Hair::TressfxCollider capsule;
    capsule.a = Eigen::Vector3f(1.0f, 2.0f, 3.0f);
    capsule.b = Eigen::Vector3f(4.0f, 5.0f, 6.0f);
    capsule.radius = 1.0f;
    capsule.bone = "Head_M";
    const std::vector<VL::Hair::TressfxCollider> capsules{capsule};
    // pose 缺该关节时不能静默读越界矩阵，也不能把胶囊写坏。
    const auto transformed = VL::Hair::TransformTressfxCapsulesByBones(
        capsules, {7}, std::vector<Eigen::Matrix4f>());
    EXPECT_TRUE(transformed[0].a.isApprox(capsule.a));
    EXPECT_TRUE(transformed[0].b.isApprox(capsule.b));
}
