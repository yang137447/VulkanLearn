#include "editor/assets/editorRetargetPreview.h"
#include <nlohmann/json.hpp>
#include <gtest/gtest.h>
#include <cmath>

namespace
{
using nlohmann::json;

json Joint(const char* name, int parent, float x, float y, float z)
{
    return {{"name", name}, {"parent", parent},
        {"restLocal", {1, 0, 0, x, 0, 1, 0, y, 0, 0, 1, z, 0, 0, 0, 1}}};
}

json Skeleton(const char* id)
{
    return {{"format", "vl.skeleton.v1"}, {"skeleton", id}, {"root", "root"},
        {"slots", {"DefaultSlot"}}, {"joints", {
            Joint("root", -1, 0, 0, 0), Joint("shoulder", 0, 0, 10, 0),
            Joint("twist", 1, 0, 3, 0), Joint("wrist", 2, 0, 7, 0)}}};
}

json Profile()
{
    return {{"format", "vl.retarget.v1"}, {"sourceSkeleton", "Source"}, {"targetSkeleton", "Target"},
        {"sourceRoot", "root"}, {"targetRoot", "root"}, {"joints", {
            {{"source", "root"}, {"target", "root"}},
            {{"source", "shoulder"}, {"target", "shoulder"}},
            {{"source", "wrist"}, {"target", "wrist"}}}}};
}

TEST(EditorRetargetPreview, UsesRuntimeCalibrationWithoutChangingBindPositions)
{
    auto profile = Profile();
    const float half = std::sqrt(0.5f);
    profile["targetPose"] = {{"name", "Aligned"}, {"rotationOffsets", {
        {{"joint", "shoulder"}, {"rotation", {0, 0, half, half}}}}}};
    const auto preview = VL::Editor::BuildEditorRetargetPreview(profile, Skeleton("Source"), Skeleton("Target"));
    ASSERT_EQ(preview.target.size(), 4u);
    EXPECT_NEAR(preview.target[3].bindPosition[0], 0, 1e-5f);
    EXPECT_NEAR(preview.target[3].bindPosition[1], 20, 1e-5f);
    EXPECT_NEAR(preview.target[3].referencePosition[0], -10, 1e-4f);
    EXPECT_NEAR(preview.target[3].referencePosition[1], 10, 1e-4f);
    EXPECT_NEAR(preview.target[1].referenceAxes[0][1], 1, 1e-5f);
    EXPECT_EQ(preview.targetPose, "Aligned");
    EXPECT_EQ(preview.targetOffsets, 1u);
}

TEST(EditorRetargetPreview, FindsCommonMappedAncestorAcrossUnmappedTwist)
{
    const auto preview = VL::Editor::BuildEditorRetargetPreview(Profile(), Skeleton("Source"), Skeleton("Target"));
    ASSERT_EQ(preview.mappings.size(), 3u);
    EXPECT_EQ(preview.mappings[2].ancestor, 1);
    EXPECT_EQ(preview.mappings[0].ancestor, -1);
    EXPECT_EQ(preview.defaultAnchor, 1u);
}

TEST(EditorRetargetPreview, RemapsNonDepthFirstSkeletonOrderToRuntimeIndices)
{
    auto source = Skeleton("Source");
    source["joints"] = {Joint("root", -1, 0, 0, 0), Joint("shoulder", 0, 0, 10, 0),
        Joint("sibling", 0, 4, 0, 0), Joint("wrist", 1, 0, 10, 0)};
    const auto preview = VL::Editor::BuildEditorRetargetPreview(Profile(), source, Skeleton("Target"));
    EXPECT_EQ(preview.source[2].name, "wrist");
    EXPECT_EQ(preview.source[2].parent, 1);
    EXPECT_EQ(preview.mappings[2].source, 2);
    EXPECT_NEAR(preview.source[2].referencePosition[1], 20, 1e-5f);
    EXPECT_EQ(preview.source[3].name, "sibling");
}

TEST(EditorRetargetPreview, RejectsSkeletonIdentityMismatch)
{
    EXPECT_THROW(VL::Editor::BuildEditorRetargetPreview(Profile(), Skeleton("Other"), Skeleton("Target")),
        std::runtime_error);
}

TEST(EditorRetargetPreview, RejectsUnknownOffsetThroughProductionValidation)
{
    auto profile = Profile();
    profile["targetPose"] = {{"name", "Bad"}, {"rotationOffsets", {
        {{"joint", "unknown"}, {"rotation", {0, 0, 0, 1}}}}}};
    EXPECT_THROW(VL::Editor::BuildEditorRetargetPreview(profile, Skeleton("Source"), Skeleton("Target")),
        std::invalid_argument);
}
}
