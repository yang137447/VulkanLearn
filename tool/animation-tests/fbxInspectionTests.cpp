#include "fbxInspection.h"

#include <stdexcept>
#include <gtest/gtest.h>

namespace
{
TEST(AnimationFbxInspection, RetainsUnweightedParentNodesAndAnimationChannels)
{
    const auto report = VL::AnimationPreflight::InspectFbx(VL_ANIMATION_FBX_FIXTURE);
    EXPECT_EQ(report["nodes"].size(), 21u);
    ASSERT_EQ(report["animations"].size(), 1u);
    const auto& animation = report["animations"][0];
    EXPECT_EQ(animation["channels"].size(), 15u);
    EXPECT_DOUBLE_EQ(animation["ticksPerSecond"], 24.0);
    EXPECT_DOUBLE_EQ(animation["durationTicks"], 20.0);
    bool foundArmature = false;
    for (const auto& node : report["nodes"])
    {
        if (node["name"] == "Armature")
        {
            foundArmature = true;
            EXPECT_EQ(node["parent"], "RootNode");
        }
    }
    EXPECT_TRUE(foundArmature);
}

TEST(AnimationFbxInspection, ReportsSourceWeightErrorsWithoutNormalizingOrTruncating)
{
    const auto report = VL::AnimationPreflight::InspectFbx(VL_ANIMATION_FBX_FIXTURE);
    ASSERT_EQ(report["meshes"].size(), 1u);
    const auto& mesh = report["meshes"][0];
    EXPECT_EQ(mesh["vertices"], 4224u);
    EXPECT_EQ(mesh["bones"].size(), 14u);
    EXPECT_EQ(mesh["maxInfluences"], 6u);
    EXPECT_EQ(mesh["invalidWeights"], 0u);
    EXPECT_EQ(mesh["unweightedVertices"], 0u);
    // 上游夹具确有未归一化权重；预检必须暴露误差，不能先“修好”再报告。
    EXPECT_NEAR(mesh["maxWeightSumError"].get<double>(), 0.376511752605438, 0.00001);
}

TEST(AnimationFbxInspection, RejectsNonFbxAndMissingInputs)
{
    EXPECT_THROW(VL::AnimationPreflight::InspectFbx("not-a-source.obj"), std::runtime_error);
    EXPECT_THROW(VL::AnimationPreflight::InspectFbx("missing-source.fbx"), std::runtime_error);
}
}
