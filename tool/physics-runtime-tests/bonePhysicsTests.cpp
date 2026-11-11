#include "physics/bonePhysics.h"

#include <cmath>
#include <fstream>
#include <gtest/gtest.h>

namespace
{
using namespace VL::Physics;

nlohmann::json CapsuleBody(const char* bone, const char* type, float mass)
{
    return {
        {"bone", bone}, {"type", type},
        {"shapes", nlohmann::json::array({{
            {"type", "capsule"}, {"radius", 2.0f}, {"halfHeight", 4.0f},
            {"center", {0.0f, 0.0f, 0.0f}}, {"rotation", {0.0f, 0.0f, 0.0f, 1.0f}}}})},
        {"mass", mass}, {"linearDamping", 0.5f}, {"angularDamping", 1.0f}};
}

nlohmann::json Limit(const char* motion, float degrees)
{
    return {{"motion", motion}, {"limit", degrees}};
}

nlohmann::json Constraint(const char* child, const char* parent)
{
    return {
        {"name", child}, {"child", child}, {"parent", parent},
        {"swing1", Limit("limited", 20.0f)}, {"swing2", Limit("limited", 20.0f)},
        {"twist", Limit("limited", 10.0f)}, {"stiffness", 0.0f}, {"damping", 0.0f},
        {"childFrame", {{"position", {0, 0, 0}}, {"rotation", {0, 0, 0, 1}}}},
        {"parentFrame", {{"position", {0, 0, 0}}, {"rotation", {0, 0, 0, 1}}}}};
}

nlohmann::json Asset()
{
    nlohmann::json set;
    set["name"] = "chain";
    set["asset"] = "PA_test";
    set["baseBone"] = "root";
    set["gravity"] = {0.0f, -980.0f, 0.0f};
    set["bodies"] = nlohmann::json::array(
        {CapsuleBody("anchor", "kinematic", 0.0f), CapsuleBody("tip", "simulated", 2.0f)});
    set["constraints"] = nlohmann::json::array({Constraint("tip", "anchor")});
    nlohmann::json root;
    root["format"] = "vl.bone-physics.v1";
    root["id"] = "test";
    root["sets"] = nlohmann::json::array({set});
    return root;
}

std::shared_ptr<const BonePhysicsAsset> Parse(nlohmann::json json)
{
    return std::make_shared<const BonePhysicsAsset>(
        ParseBonePhysicsAsset(json, "Maps/test/BPH.json"));
}
}

TEST(BonePhysics, ParsesCapsuleChain)
{
    const auto asset = Parse(Asset());
    ASSERT_EQ(asset->sets.size(), 1u);
    EXPECT_EQ(asset->sets[0].bodies.size(), 2u);
    EXPECT_EQ(asset->sets[0].constraints.size(), 1u);
    EXPECT_TRUE(asset->sets[0].bodies[1].simulated);
}

TEST(BonePhysics, SolverStepsConstrainedBody)
{
    std::vector<std::string> names{"root", "anchor", "tip"};
    std::vector<int> parents{-1, 0, 1};
    std::vector<Eigen::Matrix4f> bind(3, Eigen::Matrix4f::Identity());
    bind[1].block<3, 1>(0, 3) = Eigen::Vector3f(0, 90, 0);
    bind[2].block<3, 1>(0, 3) = Eigen::Vector3f(0, 20, 0);

    BonePhysicsSolver solver(names, parents, bind, Parse(Asset()));
    ASSERT_TRUE(solver.Valid());
    EXPECT_EQ(solver.State().simulatedBodies, 1);

    std::vector<Eigen::Matrix4f> globals(3, Eigen::Matrix4f::Identity());
    for (size_t joint = 0; joint < globals.size(); ++joint)
        globals[joint] = parents[joint] >= 0 ?
            globals[static_cast<size_t>(parents[joint])] * bind[joint] : bind[joint];

    for (int step = 0; step < 120; ++step)
        solver.Solve(globals, 1.0f / 60.0f);

    for (const auto& matrix : globals)
        for (int row = 0; row < 4; ++row)
            for (int column = 0; column < 4; ++column)
                EXPECT_TRUE(std::isfinite(matrix(row, column)));
    // 约束把 tip 拉在 anchor 附近，不会无限下落。
    const Eigen::Vector3f tip = globals[2].block<3, 1>(0, 3);
    EXPECT_LT(tip.norm(), 400.0f);
}

TEST(BonePhysics, RejectsNonCapsuleShape)
{
    auto json = Asset();
    json["sets"][0]["bodies"][0]["shapes"][0]["type"] = "box";
    EXPECT_THROW(Parse(json), std::invalid_argument);
}

class EchoBonePhysicsAsset : public ::testing::Test
{
protected:
    void SetUp() override
    {
        std::ifstream input(VL_BONE_PHYSICS_FIXTURE, std::ios::binary);
        ASSERT_TRUE(input.good());
        const std::string bytes((std::istreambuf_iterator<char>(input)),
            std::istreambuf_iterator<char>());
        asset = std::make_shared<const BonePhysicsAsset>(ParseBonePhysicsAsset(
            nlohmann::json::parse(bytes), "Maps/SC_valley_echo/Rig/BPH_EchoPostProcess.json"));
    }

    std::shared_ptr<const BonePhysicsAsset> asset;
};

TEST_F(EchoBonePhysicsAsset, MatchesPhysicsAssets)
{
    ASSERT_EQ(asset->sets.size(), 4u);
    int bodies = 0;
    int constraints = 0;
    for (const auto& set : asset->sets)
    {
        bodies += static_cast<int>(set.bodies.size());
        constraints += static_cast<int>(set.constraints.size());
    }
    EXPECT_EQ(bodies, 21);
    EXPECT_EQ(constraints, 9);
}
