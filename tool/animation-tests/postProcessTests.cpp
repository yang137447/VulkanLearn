#include "animation/postProcess.h"

#include <cmath>
#include <fstream>
#include <gtest/gtest.h>

namespace
{
using namespace VL::Animation;

constexpr float Pi = 3.14159265358979323846f;
constexpr float DegreesToRadians = Pi / 180.0f;

float ReadLane(ozz::math::SimdFloat4 vector, size_t lane)
{
    float values[4];
    ozz::math::StorePtrU(vector, values);
    return values[lane];
}

void WriteLane(ozz::math::SimdFloat4& vector, size_t lane, float value)
{
    float values[4];
    ozz::math::StorePtrU(vector, values);
    values[lane] = value;
    vector = ozz::math::simd_float4::LoadPtrU(values);
}

Eigen::Quaternionf ReadRotation(const LocalPose& locals, size_t joint)
{
    const auto& value = locals[joint / 4].rotation;
    const size_t lane = joint % 4;
    return Eigen::Quaternionf(ReadLane(value.w, lane), ReadLane(value.x, lane),
        ReadLane(value.y, lane), ReadLane(value.z, lane)).normalized();
}

void SetRotation(LocalPose& locals, size_t joint, const Eigen::Quaternionf& rotation)
{
    auto& value = locals[joint / 4].rotation;
    const size_t lane = joint % 4;
    WriteLane(value.x, lane, rotation.x());
    WriteLane(value.y, lane, rotation.y());
    WriteLane(value.z, lane, rotation.z());
    WriteLane(value.w, lane, rotation.w());
}

LocalPose IdentityPose(size_t joints)
{
    LocalPose pose((joints + 3) / 4);
    for (auto& value : pose)
    {
        value.translation.x = ozz::math::simd_float4::zero();
        value.translation.y = ozz::math::simd_float4::zero();
        value.translation.z = ozz::math::simd_float4::zero();
        value.rotation.x = ozz::math::simd_float4::zero();
        value.rotation.y = ozz::math::simd_float4::zero();
        value.rotation.z = ozz::math::simd_float4::zero();
        value.rotation.w = ozz::math::simd_float4::one();
        value.scale.x = ozz::math::simd_float4::one();
        value.scale.y = ozz::math::simd_float4::one();
        value.scale.z = ozz::math::simd_float4::one();
    }
    return pose;
}

Eigen::Quaternionf AxisAngle(const Eigen::Vector3f& axis, float degrees)
{
    return Eigen::Quaternionf(Eigen::AngleAxisf(degrees * DegreesToRadians, axis.normalized()));
}

float SignedAngleDegrees(const Eigen::Quaternionf& value, const Eigen::Vector3f& axis)
{
    return 2.0f * std::atan2(value.vec().dot(axis.normalized()), value.w()) * 180.0f / Pi;
}

SkeletonMesh MakeMesh(const std::vector<std::string>& names)
{
    SkeletonMesh mesh;
    for (size_t joint = 0; joint < names.size(); ++joint)
        mesh.joints.push_back({names[joint], joint == 0 ? -1 : 0,
            Eigen::Matrix4f::Identity(), Eigen::Matrix4f::Identity()});
    return mesh;
}

nlohmann::json Root(nlohmann::json body)
{
    body["format"] = "vl.anim-post-process.v1";
    body["id"] = "test";
    return body;
}

nlohmann::json TwistJson(bool invert, const char* driver, const char* first, const char* second)
{
    return {
        {"name", driver}, {"driverBone", driver}, {"twistAxis", "X"}, {"invert", invert},
        {"targets", nlohmann::json::array({
            {{"bone", first}, {"weight", 1.0}},
            {{"bone", second}, {"weight", 0.5}}})}};
}

std::shared_ptr<const PostProcessAsset> Parse(nlohmann::json root)
{
    return std::make_shared<const PostProcessAsset>(
        ParsePostProcessAsset(root, "Maps/test/EPP.json"));
}

nlohmann::json HelperEntryJson(const char* name, const char* oldParent, const char* newParent,
    float weight, bool invertProjectionOrder = false)
{
    nlohmann::json projections = nlohmann::json::array({
        {{"child", name}, {"oldParent", oldParent}, {"newParent", newParent},
         {"childInitial", true}, {"oldParentInitial", true}, {"newParentInitial", false}}});
    (void)invertProjectionOrder;
    return {
        {"name", name}, {"projections", projections}, {"blendT", 0.0},
        {"targets", nlohmann::json::array({{{"bone", name}, {"weight", weight}}})}};
}
}

TEST(PostProcess, ParseRejectsBadWeight)
{
    auto entry = TwistJson(false, "hand_l", "lowerarm_twist_01_l", "lowerarm_twist_02_l");
    entry["targets"][0]["weight"] = 1.5;
    EXPECT_THROW(Parse(Root({{"twist", nlohmann::json::array({entry})}})),
        std::invalid_argument);
}

TEST(PostProcess, ParseRejectsDuplicateTargetBone)
{
    auto entry = TwistJson(false, "hand_l", "lowerarm_twist_01_l", "lowerarm_twist_01_l");
    EXPECT_THROW(Parse(Root({{"twist", nlohmann::json::array({entry})}})),
        std::invalid_argument);
}

TEST(PostProcess, ParseRejectsUnknownField)
{
    auto entry = TwistJson(false, "hand_l", "lowerarm_twist_01_l", "lowerarm_twist_02_l");
    entry["driver"] = "hand_l";
    EXPECT_THROW(Parse(Root({{"twist", nlohmann::json::array({entry})}})),
        std::invalid_argument);
}

TEST(PostProcess, ParseRejectsEmptyAsset)
{
    EXPECT_THROW(Parse(Root({{"twist", nlohmann::json::array()}})), std::invalid_argument);
}

TEST(PostProcess, FullWeightWritesTwistAndHalfWeightSlerps)
{
    const auto mesh = MakeMesh({"root", "hand_l", "lowerarm_twist_01_l", "lowerarm_twist_02_l"});
    auto solver = PostProcessSolver(mesh, Parse(Root({{"twist", nlohmann::json::array({
        TwistJson(false, "hand_l", "lowerarm_twist_01_l", "lowerarm_twist_02_l")})}})));
    ASSERT_TRUE(solver.Valid());

    auto pose = IdentityPose(mesh.joints.size());
    SetRotation(pose, 1, AxisAngle(Eigen::Vector3f::UnitX(), 40.0f));
    PostProcessApplyStats stats;
    ASSERT_TRUE(solver.Apply(pose, &stats));
    EXPECT_EQ(stats.rotatedBones, 2);
    EXPECT_NEAR(SignedAngleDegrees(ReadRotation(pose, 2), Eigen::Vector3f::UnitX()), 40.0f, 0.2f);
    EXPECT_NEAR(SignedAngleDegrees(ReadRotation(pose, 3), Eigen::Vector3f::UnitX()), 20.0f, 0.2f);
}

TEST(PostProcess, InvertFlipsTwistDirection)
{
    const auto mesh = MakeMesh({"root", "upperarm_l", "upperarm_twist_01_l", "upperarm_twist_02_l"});
    auto solver = PostProcessSolver(mesh, Parse(Root({{"twist", nlohmann::json::array({
        TwistJson(true, "upperarm_l", "upperarm_twist_01_l", "upperarm_twist_02_l")})}})));
    auto pose = IdentityPose(mesh.joints.size());
    SetRotation(pose, 1, AxisAngle(Eigen::Vector3f::UnitX(), 40.0f));
    ASSERT_TRUE(solver.Apply(pose));
    EXPECT_NEAR(SignedAngleDegrees(ReadRotation(pose, 2), Eigen::Vector3f::UnitX()), -40.0f, 0.2f);
    EXPECT_NEAR(SignedAngleDegrees(ReadRotation(pose, 3), Eigen::Vector3f::UnitX()), -20.0f, 0.2f);
}

TEST(PostProcess, SwingOnlyRotationProducesNoTwist)
{
    const auto mesh = MakeMesh({"root", "hand_l", "lowerarm_twist_01_l", "lowerarm_twist_02_l"});
    auto solver = PostProcessSolver(mesh, Parse(Root({{"twist", nlohmann::json::array({
        TwistJson(false, "hand_l", "lowerarm_twist_01_l", "lowerarm_twist_02_l")})}})));
    auto pose = IdentityPose(mesh.joints.size());
    SetRotation(pose, 1, AxisAngle(Eigen::Vector3f::UnitY(), 35.0f));
    solver.Apply(pose);
    EXPECT_NEAR(ReadRotation(pose, 2).w(), 1.0f, 0.001f);
    EXPECT_NEAR(ReadRotation(pose, 3).w(), 1.0f, 0.001f);
}

TEST(PostProcess, MissingBoneIsDiagnosedAndEntrySkipped)
{
    const auto mesh = MakeMesh({"root", "hand_l"});
    auto solver = PostProcessSolver(mesh, Parse(Root({{"twist", nlohmann::json::array({
        TwistJson(false, "hand_l", "lowerarm_twist_01_l", "lowerarm_twist_02_l")})}})));
    ASSERT_FALSE(solver.BindDiagnostics().empty());
    EXPECT_EQ(solver.BindDiagnostics()[0].code, "missing-target-bone");
    EXPECT_FALSE(solver.Valid());
}

TEST(PostProcess, HelperReparentsChildToMovingParent)
{
    auto mesh = MakeMesh({"root", "pelvis", "helper"});
    mesh.joints[2].parent = 1;
    // helper 的绑定局部旋转是 20°，当前姿态被写成 45°（模拟动画漂移）。
    mesh.joints[2].restLocal.block<3, 3>(0, 0) = AxisAngle(Eigen::Vector3f::UnitY(), 20.0f)
        .toRotationMatrix();
    auto solver = PostProcessSolver(mesh, Parse(Root({{"helpers", nlohmann::json::array({
        HelperEntryJson("helper", "pelvis", "pelvis", 1.0f)})}})));
    ASSERT_TRUE(solver.Valid()) << solver.BindDiagnostics()[0].detail;

    auto pose = IdentityPose(mesh.joints.size());
    SetRotation(pose, 1, AxisAngle(Eigen::Vector3f::UnitZ(), 30.0f));
    SetRotation(pose, 2, AxisAngle(Eigen::Vector3f::UnitX(), 45.0f));
    ASSERT_TRUE(solver.Apply(pose));
    // 结果：父骨当前全局 30°Z × (父绑定^-1 × 子绑定) = 30°Z × 20°Y；
    // 子骨局部恢复为绑定的 20°Y。
    EXPECT_NEAR(ReadRotation(pose, 2).angularDistance(
        AxisAngle(Eigen::Vector3f::UnitY(), 20.0f)), 0.0f, 0.001f);
}

TEST(PostProcess, HelperBlendsTwoProjectionsAtHalf)
{
    auto mesh = MakeMesh({"root", "pelvis", "parent_l", "parent_r", "helper"});
    mesh.joints[2].restLocal.block<3, 3>(0, 0) = AxisAngle(Eigen::Vector3f::UnitZ(), 20.0f)
        .toRotationMatrix();
    mesh.joints[3].restLocal.block<3, 3>(0, 0) = AxisAngle(Eigen::Vector3f::UnitZ(), -20.0f)
        .toRotationMatrix();
    nlohmann::json projections = nlohmann::json::array({
        {{"child", "helper"}, {"oldParent", "parent_l"}, {"newParent", "parent_l"},
         {"childInitial", true}, {"oldParentInitial", true}, {"newParentInitial", false}},
        {{"child", "helper"}, {"oldParent", "parent_r"}, {"newParent", "parent_r"},
         {"childInitial", true}, {"oldParentInitial", true}, {"newParentInitial", false}}});
    nlohmann::json helper = {
        {"name", "helper"}, {"projections", projections}, {"blendT", 0.5},
        {"targets", nlohmann::json::array({{{"bone", "helper"}, {"weight", 1.0}}})}};
    auto solver = PostProcessSolver(mesh, Parse(Root({{"helpers", nlohmann::json::array({helper})}})));
    ASSERT_TRUE(solver.Valid());
    auto pose = IdentityPose(mesh.joints.size());
    solver.Apply(pose);
    // 两投影相对结果都是 0°（父骨未动），插值后仍是 0°：
    // 目标就是「把 helper 的全局旋转恢复成绑定关系」，在父骨静止时局部回到 0。
    EXPECT_NEAR(ReadRotation(pose, 4).w(), 1.0f, 0.001f);
}

nlohmann::json ConstraintJson(const char* bone, nlohmann::json targets)
{
    return {{"name", bone}, {"bone", bone}, {"transformType", "Parent"},
        {"offsetOption", "RefPose"}, {"targets", targets}};
}

TEST(PostProcess, ConstraintBlendsTargetsByWeight)
{
    const auto mesh = MakeMesh({"root", "a", "b", "bone"});
    auto solver = PostProcessSolver(mesh, Parse(Root({{"constraints", nlohmann::json::array({
        ConstraintJson("bone", nlohmann::json::array({
            {{"bone", "a"}, {"weight", 0.5}}, {{"bone", "b"}, {"weight", 0.5}}}))})}})));
    ASSERT_TRUE(solver.Valid());
    auto pose = IdentityPose(mesh.joints.size());
    SetRotation(pose, 1, AxisAngle(Eigen::Vector3f::UnitZ(), 20.0f));
    SetRotation(pose, 2, AxisAngle(Eigen::Vector3f::UnitZ(), 40.0f));
    ASSERT_TRUE(solver.Apply(pose));
    // Parent 约束是组件空间加权平均：0.5*20 + 0.5*40 = 30 度。
    EXPECT_NEAR(SignedAngleDegrees(ReadRotation(pose, 3), Eigen::Vector3f::UnitZ()), 30.0f, 0.2f);
}

TEST(PostProcess, ConstraintKeepsRefPoseOffset)
{
    auto mesh = MakeMesh({"root", "a", "b", "bone"});
    // 绑定姿态下 bone 相对 a 有 20 度偏移。
    mesh.joints[3].restLocal.block<3, 3>(0, 0) = AxisAngle(Eigen::Vector3f::UnitY(), 20.0f)
        .toRotationMatrix();
    auto solver = PostProcessSolver(mesh, Parse(Root({{"constraints", nlohmann::json::array({
        ConstraintJson("bone", nlohmann::json::array({{{"bone", "a"}, {"weight", 1.0}}}))})}})));
    auto pose = IdentityPose(mesh.joints.size());
    SetRotation(pose, 1, AxisAngle(Eigen::Vector3f::UnitZ(), 30.0f));
    ASSERT_TRUE(solver.Apply(pose));
    // 结果 = offset(20Y) * aCur(30Z)，与 UE 的 (TargetRef^-1 * SourceRef) * TargetCur 一致。
    const Eigen::Quaternionf expected = AxisAngle(Eigen::Vector3f::UnitY(), 20.0f) *
        AxisAngle(Eigen::Vector3f::UnitZ(), 30.0f);
    EXPECT_NEAR(ReadRotation(pose, 3).angularDistance(expected), 0.0f, 0.001f);
}

TEST(PostProcess, ConstraintNormalizesWeightsAboveOne)
{
    const auto mesh = MakeMesh({"root", "a", "b", "bone"});
    auto solver = PostProcessSolver(mesh, Parse(Root({{"constraints", nlohmann::json::array({
        ConstraintJson("bone", nlohmann::json::array({
            {{"bone", "a"}, {"weight", 1.0}}, {{"bone", "b"}, {"weight", 1.0}}}))})}})));
    auto pose = IdentityPose(mesh.joints.size());
    SetRotation(pose, 1, AxisAngle(Eigen::Vector3f::UnitZ(), 10.0f));
    SetRotation(pose, 2, AxisAngle(Eigen::Vector3f::UnitZ(), 30.0f));
    ASSERT_TRUE(solver.Apply(pose));
    EXPECT_NEAR(SignedAngleDegrees(ReadRotation(pose, 3), Eigen::Vector3f::UnitZ()), 20.0f, 0.2f);
}

TEST(PostProcess, ConstraintRejectsNonParentTransformType)
{
    auto entry = ConstraintJson("bone", nlohmann::json::array(
        {{{"bone", "a"}, {"weight", 1.0}}}));
    entry["transformType"] = "Rotation";
    EXPECT_THROW(Parse(Root({{"constraints", nlohmann::json::array({entry})}})),
        std::invalid_argument);
}

class EchoPostProcessAsset : public ::testing::Test
{
protected:
    void SetUp() override
    {
        std::ifstream input(VL_ANIMATION_POST_PROCESS_FIXTURE, std::ios::binary);
        ASSERT_TRUE(input.good());
        const std::string bytes((std::istreambuf_iterator<char>(input)),
            std::istreambuf_iterator<char>());
        asset = std::make_shared<const PostProcessAsset>(ParsePostProcessAsset(
            nlohmann::json::parse(bytes), "Maps/SC_valley_echo/Rig/EPP_EchoPostProcess.json"));
    }

    std::shared_ptr<const PostProcessAsset> asset;
};

TEST_F(EchoPostProcessAsset, MatchesControlRigs)
{
    ASSERT_EQ(asset->twist.size(), 8u);
    size_t twistTargets = 0;
    for (const auto& entry : asset->twist)
    {
        EXPECT_EQ(entry.twistAxis, PoseDriverAxis::X);
        EXPECT_EQ(entry.targets.size(), 2u);
        twistTargets += entry.targets.size();
        EXPECT_FLOAT_EQ(entry.targets[0].weight, 1.0f);
        EXPECT_FLOAT_EQ(entry.targets[1].weight, 0.5f);
        const bool parentSide = entry.driverBone.rfind("upperarm", 0) == 0 ||
            entry.driverBone.rfind("thigh", 0) == 0;
        EXPECT_EQ(entry.invert, parentSide) << entry.driverBone;
    }
    EXPECT_EQ(twistTargets, 16u);

    ASSERT_EQ(asset->helpers.size(), 7u);
    for (const auto& entry : asset->helpers)
    {
        EXPECT_EQ(entry.targets.size(), 1u);
        EXPECT_EQ(entry.projections.size(), entry.name == "C_Skirt_A_FrontRoot_Jnt" ? 2u : 1u);
        EXPECT_EQ(entry.projections[0].oldParent, entry.projections[0].newParent);
        EXPECT_TRUE(entry.projections[0].childInitial);
        EXPECT_TRUE(entry.projections[0].oldParentInitial);
        EXPECT_FALSE(entry.projections[0].newParentInitial);
    }

    ASSERT_EQ(asset->constraints.size(), 3u);
    for (const auto& entry : asset->constraints)
    {
        EXPECT_EQ(entry.bone, entry.name);
        float total = 0.0f;
        for (const auto& target : entry.targets) total += target.weight;
        EXPECT_NEAR(total, 1.0f, 0.001f) << entry.name;
    }
}

TEST_F(EchoPostProcessAsset, BindsOnEchoSkeleton)
{
    const auto mesh = MakeMesh({"root", "pelvis", "clavicle_l", "spine_04",
        "upperarm_l", "upperarm_twist_01_l", "upperarm_twist_02_l",
        "upperarm_r", "upperarm_twist_01_r", "upperarm_twist_02_r",
        "thigh_l", "thigh_twist_01_l", "thigh_twist_02_l", "thigh_r", "thigh_twist_01_r",
        "thigh_twist_02_r", "hand_l", "lowerarm_twist_01_l", "lowerarm_twist_02_l",
        "hand_r", "lowerarm_twist_01_r", "lowerarm_twist_02_r", "foot_l",
        "calf_twist_01_l", "calf_twist_02_l", "foot_r", "calf_twist_01_r", "calf_twist_02_r",
        "C_Scarf_A_Root_Jnt", "C_Scarf_A_Joint2_Jnt", "C_Scarf_A_Joint3_Jnt",
        "L_Skirt_A_BackRoot_Jnt", "R_Skirt_A_BackRoot_Jnt", "C_Skirt_A_BackRoot_Jnt",
        "C_Skirt_A_FrontRoot_Jnt", "L_Skirt_A_FrontRoot_Jnt", "R_Skirt_A_FrontRoot_Jnt",
        "spine_01", "spine_02", "spine_05", "R_Canister_A_Joint1_Jnt",
        "R_LargePouch_A_Joint1_Jnt"});
    auto solver = PostProcessSolver(mesh, asset);
    EXPECT_TRUE(solver.BindDiagnostics().empty());
    EXPECT_TRUE(solver.Valid());
}
