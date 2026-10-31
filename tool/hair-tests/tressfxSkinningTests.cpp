#include "hair/tressfxSkinning.h"
#include "hair/tressfxSolverMath.h"

#include <gtest/gtest.h>

#include <cstdint>
#include <cstring>
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

TEST(TressfxSkinning, BlendedBoneMatrixMatchesWeightedInitialPosition)
{
    Eigen::Matrix4f head = Eigen::Matrix4f::Identity();
    Eigen::Matrix4f neck = Eigen::Matrix4f::Identity();
    head(0, 3) = 10.0f;
    neck(0, 3) = 30.0f;
    const std::vector<Eigen::Matrix4f> palette{head, neck};
    VL::Hair::TressfxStrandBoneWeights weights;
    weights.joints = {0, 1};
    weights.weights = {0.75f, 0.25f};
    const auto blended = VL::Hair::TressfxBlendedBoneMatrix(weights, palette);
    EXPECT_NEAR(blended(0, 3), 15.0f, 1.0e-6f);
    const auto position = VL::Hair::TressfxInitialPosition(
        weights, palette, Eigen::Vector3f(0.0f, 0.0f, 0.0f));
    EXPECT_NEAR(position.x(), 15.0f, 1.0e-6f);
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

TEST(TressfxSkinning, PacksTopFourStrandWeightsAndNormalizes)
{
    // 5 个影响只保留权重最大的 4 个，并按保留项之和归一化；.tfxbone 与网格派生
    // 共用这条规则，shader 的 GpuStrandBones 固定 4 槽。
    VL::Hair::TressfxStrandBoneWeights weights;
    weights.joints = {1, 2, 3, 4, 5};
    weights.weights = {0.1f, 0.4f, 0.1f, 0.3f, 0.1f};
    const auto packed = VL::Hair::TressfxPackStrandBoneWeights(weights, 9);
    EXPECT_EQ(packed.joints[0], 2u);
    EXPECT_EQ(packed.joints[1], 4u);
    // stable_sort：0.1 的三项保持输入顺序，取前两个。
    EXPECT_EQ(packed.joints[2], 1u);
    EXPECT_EQ(packed.joints[3], 3u);
    EXPECT_NEAR(packed.weights[0], 0.4f / 0.9f, 1.0e-6f);
    EXPECT_NEAR(packed.weights[1], 0.3f / 0.9f, 1.0e-6f);
    EXPECT_NEAR(packed.weights[2], 0.1f / 0.9f, 1.0e-6f);
    EXPECT_NEAR(packed.weights[3], 0.1f / 0.9f, 1.0e-6f);
}

TEST(TressfxSkinning, PackFallsBackToRootBoneWithoutPositiveWeights)
{
    const auto empty = VL::Hair::TressfxPackStrandBoneWeights({}, 7);
    EXPECT_EQ(empty.joints[0], 7u);
    EXPECT_FLOAT_EQ(empty.weights[0], 1.0f);

    VL::Hair::TressfxStrandBoneWeights zero;
    zero.joints = {3, 4};
    zero.weights = {0.0f, 0.0f};
    const auto fallback = VL::Hair::TressfxPackStrandBoneWeights(zero, 7);
    EXPECT_EQ(fallback.joints[0], 7u);
    EXPECT_FLOAT_EQ(fallback.weights[0], 1.0f);
}

TEST(TressfxSkinning, TransferConversionKeepsUnmatchedStrandsEmpty)
{
    // CSR：header 3 个 word（2 个顶点），vertex0 没有影响，vertex1 有两个。
    VL::Hair::SdfProxySkinTransfer transfer;
    transfer.vertexCount = 2;
    transfer.words = {3u, 3u, 7u};
    const float firstWeight = 0.25f;
    const float secondWeight = 0.75f;
    std::uint32_t bits = 0;
    transfer.words.push_back(5u);
    std::memcpy(&bits, &firstWeight, sizeof(bits));
    transfer.words.push_back(bits);
    transfer.words.push_back(6u);
    std::memcpy(&bits, &secondWeight, sizeof(bits));
    transfer.words.push_back(bits);

    const auto strands = VL::Hair::TressfxStrandBoneWeightsFromTransfer(transfer);
    ASSERT_EQ(strands.size(), 2u);
    EXPECT_TRUE(strands[0].joints.empty()) << "unmatched root must stay empty";
    ASSERT_EQ(strands[1].joints.size(), 2u);
    EXPECT_EQ(strands[1].joints[0], 5u);
    EXPECT_NEAR(strands[1].weights[0], firstWeight, 1.0e-6f);
    EXPECT_NEAR(strands[1].weights[1], secondWeight, 1.0e-6f);

    // 空权重不能变成零矩阵：上传路径按根回退 rootBone。
    const auto packed = VL::Hair::TressfxPackStrandBoneWeights(strands[0], 9);
    EXPECT_EQ(packed.joints[0], 9u);
    EXPECT_FLOAT_EQ(packed.weights[0], 1.0f);
}
