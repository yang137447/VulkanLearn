#include "animation/poseDriver.h"

#include <cmath>
#include <fstream>
#include <gtest/gtest.h>

namespace
{
using namespace VL::Animation;

constexpr float Pi = 3.14159265358979323846f;

Eigen::Quaternionf Basis()
{
    // Eigen 构造顺序是 (w, x, y, z)：绕 X 轴 -90°。
    return Eigen::Quaternionf(0.70710678118654752440f, -0.70710678118654752440f, 0, 0);
}

// 测试里复刻 UE FRotator::Quaternion 的标量路径；求解器内部不可见，必须独立实现。
Eigen::Quaternionf UeRotator(float pitch, float yaw, float roll)
{
    const float half = Pi / 360.0f;
    const float sp = std::sin(pitch * half), cp = std::cos(pitch * half);
    const float sy = std::sin(yaw * half), cy = std::cos(yaw * half);
    const float sr = std::sin(roll * half), cr = std::cos(roll * half);
    // Euler 四参数构造是 (w,x,y,z)，这里按分量显式写入，避免顺序陷阱。
    Eigen::Quaternionf result;
    result.x() = cr * sp * sy - sr * cp * cy;
    result.y() = -cr * sp * cy - sr * cp * sy;
    result.z() = cr * cp * sy - sr * sp * cy;
    result.w() = cr * cp * cy + sr * sp * sy;
    return result.normalized();
}

Eigen::Quaternionf UeToVl(const Eigen::Quaternionf& ue)
{
    return (Basis() * ue * Basis().conjugate()).normalized();
}

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
    for (size_t joint = joints; joint < pose.size() * 4; ++joint)
    {
        WriteLane(pose[joint / 4].rotation.x, joint % 4, 0);
        WriteLane(pose[joint / 4].rotation.y, joint % 4, 0);
        WriteLane(pose[joint / 4].rotation.z, joint % 4, 0);
        WriteLane(pose[joint / 4].rotation.w, joint % 4, 1);
    }
    return pose;
}

void SetRotation(LocalPose& pose, size_t joint, const Eigen::Quaternionf& value)
{
    WriteLane(pose[joint / 4].rotation.x, joint % 4, value.x());
    WriteLane(pose[joint / 4].rotation.y, joint % 4, value.y());
    WriteLane(pose[joint / 4].rotation.z, joint % 4, value.z());
    WriteLane(pose[joint / 4].rotation.w, joint % 4, value.w());
}

void SetTranslation(LocalPose& pose, size_t joint, const Eigen::Vector3f& value)
{
    WriteLane(pose[joint / 4].translation.x, joint % 4, value.x());
    WriteLane(pose[joint / 4].translation.y, joint % 4, value.y());
    WriteLane(pose[joint / 4].translation.z, joint % 4, value.z());
}

SkeletonMesh MakeMesh(const std::vector<std::string>& names)
{
    SkeletonMesh mesh;
    for (size_t joint = 0; joint < names.size(); ++joint)
        mesh.joints.push_back({names[joint], joint == 0 ? -1 : 0,
            Eigen::Matrix4f::Identity(), Eigen::Matrix4f::Identity()});
    return mesh;
}

nlohmann::json BaseDriver(const std::string& bone, const char* solver, float radius)
{
    return {
        {"name", "driver"},
        {"sourceBones", {bone}},
        {"evalSpaceBone", ""},
        {"evalFromRefPose", false},
        {"driveSource", "Rotation"},
        {"driveOutput", "Curves"},
        {"rbf", {
            {"solverType", solver}, {"radius", radius}, {"automaticRadius", false},
            {"function", "Linear"}, {"distanceMethod", "SwingAngle"}, {"twistAxis", "X"},
            {"weightThreshold", 0.0001}, {"normalizeMethod", "OnlyNormalizeAboveOne"},
            {"medianReference", {0, 0, 0}}, {"medianMin", 45}, {"medianMax", 60}}},
        {"targets", nlohmann::json::array()}};
}

nlohmann::json Target(const std::string& name, float pitch, float yaw, float roll)
{
    return {
        {"drivenName", name}, {"scale", 1.0},
        {"distanceMethod", "Default"}, {"function", "Default"},
        {"applyCustomCurve", false},
        {"transforms", {{{"translation", {0, 0, 0}},
            {"rotation", {{"pitch", pitch}, {"yaw", yaw}, {"roll", roll}}}}}}};
}

std::shared_ptr<const PoseDriverSet> MakeSet(nlohmann::json driver)
{
    nlohmann::json root = {{"format", "vl.anim-pose-driver.v1"}, {"id", "test"},
        {"drivers", nlohmann::json::array({driver})}};
    return std::make_shared<const PoseDriverSet>(ParsePoseDriverSet(root, "Maps/test.json"));
}

float Curve(const PoseDriverFrame& frame, const std::string& name)
{
    for (const auto& curve : frame.curves)
        if (curve.name == name) return curve.value;
    return 0.0f;
}

std::string Describe(const PoseDriverFrame& frame)
{
    std::string text = "active=" + std::to_string(frame.active) +
        " solved=" + std::to_string(frame.solvedDrivers) +
        " curves=" + std::to_string(frame.curves.size());
    for (const auto& diagnostic : frame.diagnostics)
        text += " [" + diagnostic.driver + "/" + diagnostic.code + ":" + diagnostic.detail + "]";
    for (const auto& curve : frame.curves)
        text += " " + curve.name + "=" + std::to_string(curve.value);
    return text;
}
}

TEST(PoseDriver, ParseRejectsDrivePoses)
{
    auto driver = BaseDriver("bone", "Interpolative", 90);
    driver["driveOutput"] = "Poses";
    EXPECT_THROW(MakeSet(driver), std::invalid_argument);
}

TEST(PoseDriver, ParseRejectsCustomCurve)
{
    auto driver = BaseDriver("bone", "Interpolative", 90);
    auto target = Target("curve", 0, 0, 0);
    target["applyCustomCurve"] = true;
    driver["targets"].push_back(target);
    EXPECT_THROW(MakeSet(driver), std::invalid_argument);
}

TEST(PoseDriver, ParseRejectsTransformCountMismatch)
{
    auto driver = BaseDriver("bone", "Interpolative", 90);
    driver["sourceBones"] = {"a", "b"};
    driver["targets"].push_back(Target("curve", 0, 0, 0));
    EXPECT_THROW(MakeSet(driver), std::invalid_argument);
}

TEST(PoseDriver, ParseRejectsUnknownField)
{
    auto driver = BaseDriver("bone", "Interpolative", 90);
    driver["PoseTargets"] = nlohmann::json::array();
    EXPECT_THROW(MakeSet(driver), std::invalid_argument);
}

TEST(PoseDriver, InterpolativeReproducesEveryTarget)
{
    const auto mesh = MakeMesh({"root", "bone"});
    auto driver = BaseDriver("bone", "Interpolative", 90);
    driver["targets"].push_back(Target("a", 0, 0, 0));
    driver["targets"].push_back(Target("b", 35, 0, 0));
    driver["targets"].push_back(Target("c", 0, 40, 0));
    auto solver = PoseDriverSolver(mesh, MakeSet(driver));
    ASSERT_EQ(solver.Set()->drivers.size(), 1u);
    ASSERT_EQ(solver.Set()->drivers[0].targets.size(), 3u);
    ASSERT_EQ(solver.Set()->drivers[0].sourceBones.size(), 1u);
    ASSERT_EQ(solver.Set()->drivers[0].targets[0].rotations.size(), 1u);

    const std::pair<const char*, nlohmann::json> targets[] = {
        {"a", driver["targets"][0]}, {"b", driver["targets"][1]},
        {"c", driver["targets"][2]}};
    for (const auto& entry : targets)
    {
        const auto& rotation = entry.second["transforms"][0]["rotation"];
        auto pose = IdentityPose(mesh.joints.size());
        SetRotation(pose, 1, UeToVl(UeRotator(rotation["pitch"].get<float>(),
            rotation["yaw"].get<float>(), rotation["roll"].get<float>())));
        const auto frame = solver.Solve(pose);
        ASSERT_TRUE(frame.active) << entry.first << " " << Describe(frame);
        EXPECT_NEAR(Curve(frame, entry.first), 1.0f, 0.02f) << entry.first;
        for (const auto& other : targets)
            if (std::string(other.first) != entry.first)
                EXPECT_LT(Curve(frame, other.first), 0.02f) << entry.first << " -> " << other.first;
    }
}

TEST(PoseDriver, SwingAngleDistanceIgnoresPureTwist)
{
    const auto mesh = MakeMesh({"root", "bone"});
    // 半径 30° 让第二个目标完全落在衰减区外，便于单看“纯 twist 是否被忽略”。
    auto driver = BaseDriver("bone", "Additive", 30);
    driver["targets"].push_back(Target("identity", 0, 0, 0));
    driver["targets"].push_back(Target("pitch", 60, 0, 0));
    auto solver = PoseDriverSolver(mesh, MakeSet(driver));

    auto pose = IdentityPose(mesh.joints.size());
    // 绕 UE X 轴 40° 的纯 twist：SwingAngle 距离应为 0。
    SetRotation(pose, 1, UeToVl(UeRotator(0, 0, 40)));
    const auto frame = solver.Solve(pose);
    EXPECT_NEAR(Curve(frame, "identity"), 1.0f, 0.02f) << Describe(frame);
    EXPECT_LT(Curve(frame, "pitch"), 0.02f);
}

TEST(PoseDriver, AdditiveNormalizesAboveOne)
{
    const auto mesh = MakeMesh({"root", "bone"});
    auto driver = BaseDriver("bone", "Additive", 90);
    driver["targets"].push_back(Target("a", 0, 0, 0));
    driver["targets"].push_back(Target("b", 0, 0, 0));
    auto solver = PoseDriverSolver(mesh, MakeSet(driver));
    const auto frame = solver.Solve(IdentityPose(mesh.joints.size()));
    EXPECT_NEAR(Curve(frame, "a"), 0.5f, 0.01f);
    EXPECT_NEAR(Curve(frame, "b"), 0.5f, 0.01f);
}

TEST(PoseDriver, MissingSourceBoneIsDiagnosedAndProducesNoCurve)
{
    const auto mesh = MakeMesh({"root"});
    auto driver = BaseDriver("absent", "Interpolative", 90);
    driver["targets"].push_back(Target("a", 0, 0, 0));
    auto solver = PoseDriverSolver(mesh, MakeSet(driver));
    ASSERT_FALSE(solver.BindDiagnostics().empty());
    EXPECT_EQ(solver.BindDiagnostics()[0].code, "missing-source-bone");
    const auto frame = solver.Solve(IdentityPose(mesh.joints.size()));
    EXPECT_FALSE(frame.active);
    EXPECT_TRUE(frame.curves.empty());
}

TEST(PoseDriver, TranslationSourceUsesLocalTranslation)
{
    const auto mesh = MakeMesh({"root", "bone"});
    auto driver = BaseDriver("bone", "Additive", 10);
    driver["driveSource"] = "Translation";
    auto target = Target("near", 0, 0, 0);
    target["transforms"][0]["translation"] = {5, 0, 0};
    driver["targets"].push_back(target);
    auto solver = PoseDriverSolver(mesh, MakeSet(driver));

    auto pose = IdentityPose(mesh.joints.size());
    SetTranslation(pose, 1, {5, 0, 0});
    EXPECT_NEAR(Curve(solver.Solve(pose), "near"), 1.0f, 0.02f);
}

TEST(PoseDriver, RefPoseRelativeEvaluationRemovesBindRotation)
{
    auto mesh = MakeMesh({"root", "bone"});
    const Eigen::Quaternionf bind = UeToVl(UeRotator(0, 0, 25));
    mesh.joints[1].restLocal.block<3, 3>(0, 0) = bind.toRotationMatrix();

    auto driver = BaseDriver("bone", "Additive", 90);
    driver["evalFromRefPose"] = true;
    driver["targets"].push_back(Target("identity", 0, 0, 0));
    auto solver = PoseDriverSolver(mesh, MakeSet(driver));

    auto pose = IdentityPose(mesh.joints.size());
    SetRotation(pose, 1, bind);
    const auto frame = solver.Solve(pose);
    EXPECT_NEAR(Curve(frame, "identity"), 1.0f, 0.02f) << Describe(frame);
}

class EchoPoseDriverAsset : public ::testing::Test
{
protected:
    void SetUp() override
    {
        std::ifstream input(VL_ANIMATION_POSE_DRIVER_FIXTURE, std::ios::binary);
        ASSERT_TRUE(input.good());
        const std::string bytes((std::istreambuf_iterator<char>(input)),
            std::istreambuf_iterator<char>());
        asset = std::make_shared<const PoseDriverSet>(ParsePoseDriverSet(
            nlohmann::json::parse(bytes), "Maps/SC_valley_echo/Rig/EPD_EchoPostProcess.json"));
    }

    std::shared_ptr<const PoseDriverSet> asset;
};

TEST_F(EchoPoseDriverAsset, MatchesT3DStructure)
{
    EXPECT_EQ(asset->id, "EPD_EchoPostProcess");
    ASSERT_EQ(asset->drivers.size(), 9u);
    const char* bones[] = {"neck_02", "lowerarm_l", "lowerarm_r", "calf_l", "calf_r",
        "upperarm_r", "hand_l", "hand_r", "upperarm_l"};
    const float radii[] = {90, 30, 30, 40, 40, 50, 50, 50, 50};
    const size_t counts[] = {5, 5, 5, 6, 6, 13, 6, 6, 13};
    size_t total = 0;
    for (size_t index = 0; index < asset->drivers.size(); ++index)
    {
        const auto& driver = asset->drivers[index];
        EXPECT_EQ(driver.sourceBones, std::vector<std::string>{bones[index]});
        EXPECT_EQ(driver.params.solverType, RBFSolverType::Interpolative);
        EXPECT_EQ(driver.params.function, RBFFunctionType::Linear);
        EXPECT_EQ(driver.params.distanceMethod, RBFDistanceMethod::SwingAngle);
        EXPECT_EQ(driver.params.twistAxis, PoseDriverAxis::X);
        EXPECT_FLOAT_EQ(driver.params.radius, radii[index]);
        EXPECT_EQ(driver.targets.size(), counts[index]);
        total += driver.targets.size();
    }
    EXPECT_EQ(total, 65u);
}

TEST_F(EchoPoseDriverAsset, BindsAndSolvesOnEchoSkeleton)
{
    const auto mesh = MakeMesh({"root", "pelvis", "neck_02", "lowerarm_l", "lowerarm_r",
        "calf_l", "calf_r", "upperarm_r", "hand_l", "hand_r", "upperarm_l"});
    auto solver = PoseDriverSolver(mesh, asset);
    EXPECT_TRUE(solver.BindDiagnostics().empty());
    const auto frame = solver.Solve(IdentityPose(mesh.joints.size()));
    EXPECT_EQ(frame.solvedDrivers, 9);
    EXPECT_TRUE(frame.active);
    EXPECT_FALSE(frame.curves.empty());
    for (const auto& curve : frame.curves)
    {
        EXPECT_TRUE(std::isfinite(curve.value)) << curve.name;
        EXPECT_GE(curve.value, 0.0f) << curve.name;
        EXPECT_LE(curve.value, 1.0f) << curve.name;
    }
}
