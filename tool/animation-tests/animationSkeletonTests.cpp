#include "animation/animationSkeleton.h"
#include "animation/animationAssets.h"
#include "mesh/validation/meshAssetValidator.h"

#include <cmath>
#include <functional>
#include <limits>
#include <stdexcept>
#include <string>
#include <gtest/gtest.h>

namespace
{
using nlohmann::json;
using VL::Animation::Joint;
using VL::Animation::ParseSkeleton;
using VL::Animation::SkeletonAsset;
using VL::Animation::SkeletonMesh;

json IdentityArray()
{
    return json::array({1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1});
}

json TranslationArray(float x)
{
    json values = IdentityArray();
    values[3] = x;
    return values;
}

json MatrixArray(const Eigen::Matrix4f& matrix)
{
    json values = json::array();
    for (int row = 0; row < 4; ++row)
    {
        for (int column = 0; column < 4; ++column)
        {
            values.push_back(matrix(row, column));
        }
    }
    return values;
}

json MakeSkeletonJson()
{
    return json{
        {"format", "vl.skeleton.v1"},
        {"skeleton", "SK_Test"},
        {"root", "root"},
        {"joints", json::array({
            {{"name", "root"}, {"parent", -1}, {"restLocal", IdentityArray()}},
            {{"name", "child"}, {"parent", 0}, {"restLocal", TranslationArray(10.0f)}}})},
        {"slots", json::array({"DefaultSlot"})},
        {"curves", {{"distance_from_ledge", "Float"}}}
    };
}

std::string ErrorOf(const std::function<void()>& action)
{
    try
    {
        action();
    }
    catch (const std::exception& error)
    {
        return error.what();
    }
    return {};
}

SkeletonMesh MakeMatchingMesh()
{
    SkeletonMesh mesh;
    Joint root;
    root.name = "root";
    root.parent = -1;
    Joint child;
    child.name = "child";
    child.parent = 0;
    child.restLocal = Eigen::Matrix4f::Identity();
    child.restLocal(0, 3) = 10.0f;
    mesh.joints = {root, child};
    return mesh;
}

TEST(AnimationSkeleton, ParsesTopologySlotsAndCurveRegistry)
{
    const SkeletonAsset skeleton = ParseSkeleton(MakeSkeletonJson(), "Maps/Test/SK_Test.json");

    EXPECT_EQ(skeleton.id, "SK_Test");
    EXPECT_EQ(skeleton.root, "root");
    ASSERT_EQ(skeleton.joints.size(), 2u);
    EXPECT_EQ(skeleton.joints[0].name, "root");
    EXPECT_EQ(skeleton.joints[0].parent, -1);
    EXPECT_EQ(skeleton.joints[1].name, "child");
    EXPECT_EQ(skeleton.joints[1].parent, 0);
    EXPECT_FLOAT_EQ(skeleton.joints[1].restLocal(0, 3), 10.0f);
    EXPECT_TRUE((skeleton.joints[1].restLocal.block<3, 3>(0, 0).isIdentity(1e-6f)));
    ASSERT_EQ(skeleton.slots.size(), 1u);
    EXPECT_TRUE(skeleton.HasSlot("DefaultSlot"));
    EXPECT_TRUE(skeleton.HasCurve("distance_from_ledge"));
    // 注册表查找大小写不敏感，但保留原始拼写。
    EXPECT_TRUE(skeleton.HasCurve("Distance_From_Ledge"));
    ASSERT_EQ(skeleton.curves.count("distance_from_ledge"), 1u);
    EXPECT_EQ(skeleton.JointIndex("child"), 1);
    EXPECT_EQ(skeleton.JointIndex("missing"), -1);
    EXPECT_TRUE(skeleton.sourceDigest.empty());
}

TEST(AnimationSkeleton, PreservesVirtualBonesAndAdditiveReferencePose)
{
    json source = MakeSkeletonJson();
    source["virtualBones"] = json::array({{{"name", "VB hand"}}});
    source["additiveReferencePose"] = {{"referencePose", "RefPose"}};
    source["source"] = {
        {"fbx", "Maps/Test/SKM_Test.fbx"},
        {"fbxBlake3", std::string(64, 'a')},
        {"tool", "export_test_skeleton.py"}};

    const SkeletonAsset skeleton = ParseSkeleton(source, "SK_Test.json");
    EXPECT_EQ(json::parse(skeleton.virtualBonesJson)[0]["name"], "VB hand");
    EXPECT_EQ(json::parse(skeleton.additiveReferencePoseJson)["referencePose"], "RefPose");
    EXPECT_EQ(skeleton.sourceFbx, "Maps/Test/SKM_Test.fbx");
    EXPECT_EQ(skeleton.sourceFbxDigest, std::string(64, 'a'));
    EXPECT_EQ(skeleton.sourceTool, "export_test_skeleton.py");
}

TEST(AnimationSkeleton, RejectsWrongFormatOrMissingHeaderFields)
{
    json source = MakeSkeletonJson();
    source["format"] = "vl.skeleton.v2";
    EXPECT_NE(ErrorOf([&] { ParseSkeleton(source, "SK_bad.json"); }).find("vl.skeleton.v1"),
        std::string::npos);

    source = MakeSkeletonJson();
    source.erase("skeleton");
    EXPECT_NE(ErrorOf([&] { ParseSkeleton(source, "SK_bad.json"); }).find("skeleton"),
        std::string::npos);

    source = MakeSkeletonJson();
    source["joints"] = json::array();
    EXPECT_NE(ErrorOf([&] { ParseSkeleton(source, "SK_bad.json"); }).find("joints"),
        std::string::npos);
}

TEST(AnimationSkeleton, RejectsAmbiguousOrMismatchedRoot)
{
    json source = MakeSkeletonJson();
    source["root"] = "missing";
    const std::string message = ErrorOf([&] { ParseSkeleton(source, "SK_bad.json"); });
    EXPECT_NE(message.find("root"), std::string::npos);
    EXPECT_NE(message.find("parent=-1"), std::string::npos);

    source = MakeSkeletonJson();
    source["joints"][1]["parent"] = -1;
    EXPECT_NE(ErrorOf([&] { ParseSkeleton(source, "SK_bad.json"); }).find("parent -1"),
        std::string::npos);
}

TEST(AnimationSkeleton, RejectsDuplicateJointNamesAndBadParentIndexes)
{
    json source = MakeSkeletonJson();
    source["joints"][1]["name"] = "root";
    EXPECT_NE(ErrorOf([&] { ParseSkeleton(source, "SK_bad.json"); }).find("duplicates joint name"),
        std::string::npos);

    source = MakeSkeletonJson();
    source["joints"][1]["parent"] = 1;
    EXPECT_NE(ErrorOf([&] { ParseSkeleton(source, "SK_bad.json"); }).find("topological order"),
        std::string::npos);

    source = MakeSkeletonJson();
    source["joints"][1]["parent"] = -2;
    EXPECT_NE(ErrorOf([&] { ParseSkeleton(source, "SK_bad.json"); }).find("-1 or a preceding"),
        std::string::npos);
}

TEST(AnimationSkeleton, RejectsNonFiniteRestLocal)
{
    json source = MakeSkeletonJson();
    source["joints"][1]["restLocal"][3] =
        std::numeric_limits<float>::quiet_NaN();
    const std::string message = ErrorOf([&] { ParseSkeleton(source, "SK_bad.json"); });
    EXPECT_NE(message.find("restLocal"), std::string::npos);
    EXPECT_NE(message.find("finite"), std::string::npos);
}

TEST(AnimationSkeleton, RejectsNonAffineRestLocal)
{
    json source = MakeSkeletonJson();
    source["joints"][1]["restLocal"][15] = 2.0f;
    EXPECT_NE(ErrorOf([&] { ParseSkeleton(source, "SK_bad.json"); }).find("last row"),
        std::string::npos);

    source = MakeSkeletonJson();
    source["joints"][1]["restLocal"][1] = 0.5f;
    EXPECT_NE(ErrorOf([&] { ParseSkeleton(source, "SK_bad.json"); }).find("shear"),
        std::string::npos);

    source = MakeSkeletonJson();
    source["joints"][1]["restLocal"][0] = -1.0f;
    EXPECT_NE(ErrorOf([&] { ParseSkeleton(source, "SK_bad.json"); }).find("reflected"),
        std::string::npos);
}

TEST(AnimationSkeleton, RejectsEmptyDuplicateOrInvalidSlots)
{
    json source = MakeSkeletonJson();
    source["slots"] = json::array();
    EXPECT_NE(ErrorOf([&] { ParseSkeleton(source, "SK_bad.json"); }).find("slots"),
        std::string::npos);

    source = MakeSkeletonJson();
    source["slots"] = json::array({"DefaultSlot", "DefaultSlot"});
    EXPECT_NE(ErrorOf([&] { ParseSkeleton(source, "SK_bad.json"); }).find("duplicates slot"),
        std::string::npos);

    source = MakeSkeletonJson();
    source["slots"] = json::array({""});
    EXPECT_NE(ErrorOf([&] { ParseSkeleton(source, "SK_bad.json"); }).find("must not be empty"),
        std::string::npos);
}

TEST(AnimationSkeleton, RejectsDuplicateCurveNamesIgnoringCaseAndNonFloatTypes)
{
    json source = MakeSkeletonJson();
    source["curves"] = {
        {"Distance_From_Ledge", "Float"},
        {"distance_from_ledge", "Float"}};
    EXPECT_NE(ErrorOf([&] { ParseSkeleton(source, "SK_bad.json"); }).find("case-insensitively"),
        std::string::npos);

    source = MakeSkeletonJson();
    source["curves"] = {{"contact_l", "Bool"}};
    const std::string message = ErrorOf([&] { ParseSkeleton(source, "SK_bad.json"); });
    EXPECT_NE(message.find("contact_l"), std::string::npos);
    EXPECT_NE(message.find("Float"), std::string::npos);
}

TEST(AnimationSkeleton, ReadsRowMajorRestLocalIntoEigenColumnMajorStorage)
{
    // 旋转 90° 绕 Z、XY 各缩放 2、厘米平移 (10,20,30)。行/列写反会立刻被对照拒绝。
    Eigen::Matrix4f expected = Eigen::Matrix4f::Identity();
    expected << 0, -2, 0, 10,
        2, 0, 0, 20,
        0, 0, 1, 30,
        0, 0, 0, 1;

    json source = MakeSkeletonJson();
    source["joints"][1]["restLocal"] = MatrixArray(expected);
    const SkeletonAsset skeleton = ParseSkeleton(source, "SK_Test.json");
    ASSERT_EQ(skeleton.joints.size(), 2u);
    EXPECT_TRUE((skeleton.joints[1].restLocal.isApprox(expected, 1e-6f)));

    SkeletonMesh mesh = MakeMatchingMesh();
    mesh.joints[1].restLocal = expected;
    EXPECT_NO_THROW(VL::Animation::ValidateSkeletonAgainstMesh(
        skeleton, mesh, "Maps/Test/walk.fbx"));
}

TEST(AnimationSkeleton, MatchesMeshJointsWithinTolerance)
{
    const SkeletonAsset skeleton = ParseSkeleton(MakeSkeletonJson(), "SK_Test.json");
    SkeletonMesh mesh = MakeMatchingMesh();
    EXPECT_NO_THROW(VL::Animation::ValidateSkeletonAgainstMesh(
        skeleton, mesh, "Maps/Test/walk.fbx"));
}

TEST(AnimationSkeleton, RejectsMeshJointCountNameAndParentMismatches)
{
    const SkeletonAsset skeleton = ParseSkeleton(MakeSkeletonJson(), "SK_Test.json");

    SkeletonMesh mesh = MakeMatchingMesh();
    mesh.joints.pop_back();
    EXPECT_NE(ErrorOf([&] { VL::Animation::ValidateSkeletonAgainstMesh(
        skeleton, mesh, "walk.fbx"); }).find("joint count"), std::string::npos);

    mesh = MakeMatchingMesh();
    mesh.joints[1].name = "pelvis";
    std::string message = ErrorOf([&] { VL::Animation::ValidateSkeletonAgainstMesh(
        skeleton, mesh, "walk.fbx"); });
    EXPECT_NE(message.find("child"), std::string::npos);
    EXPECT_NE(message.find("pelvis"), std::string::npos);
    EXPECT_NE(message.find("name mismatch"), std::string::npos);

    mesh = MakeMatchingMesh();
    mesh.joints[1].parent = -1;
    message = ErrorOf([&] { VL::Animation::ValidateSkeletonAgainstMesh(
        skeleton, mesh, "walk.fbx"); });
    EXPECT_NE(message.find("child"), std::string::npos);
    EXPECT_NE(message.find("parent mismatch"), std::string::npos);
}

TEST(AnimationSkeleton, RejectsMeshRestLocalDifferenceAboveTolerance)
{
    const SkeletonAsset skeleton = ParseSkeleton(MakeSkeletonJson(), "SK_Test.json");

    SkeletonMesh withinTolerance = MakeMatchingMesh();
    withinTolerance.joints[1].restLocal(0, 3) = 10.000005f;
    EXPECT_NO_THROW(VL::Animation::ValidateSkeletonAgainstMesh(
        skeleton, withinTolerance, "walk.fbx"));

    SkeletonMesh outsideTolerance = MakeMatchingMesh();
    outsideTolerance.joints[1].restLocal(0, 3) = 10.0002f;
    const std::string message = ErrorOf([&] {
        VL::Animation::ValidateSkeletonAgainstMesh(
            skeleton, outsideTolerance, "walk.fbx"); });
    EXPECT_NE(message.find("child"), std::string::npos);
    EXPECT_NE(message.find("restLocal mismatch"), std::string::npos);
    EXPECT_NE(message.find("tolerance"), std::string::npos);
}

TEST(AnimationSkeleton, RejectsLegacySkinningRootWithSkeletonHint)
{
    const json meshAsset = {
        {"type", "mesh"},
        {"modelDataPath", "Maps/Test/SM_body.fbx"},
        {"materialSlots", json::array({
            {{"name", "Default"}, {"materialInstancePath", "Maps/Test/MI_default.json"}}})},
        {"skinning", {
            {"root", "root"},
            {"skeleton", "Maps/Test/Skeletons/SK_body.json"},
            {"clips", json::array({{{"id", "walk"}, {"path", "Maps/Test/walk.fbx"}}})}}}};

    // 即使同时给了 skeleton，残留的 root 也必须被拒绝：旧字段没有兼容分支。
    const std::string message = ErrorOf([&] {
        MeshAssetValidator::BuildLoadPlan("Maps/Test/SM_body.json", meshAsset); });
    EXPECT_NE(message.find("skinning.root"), std::string::npos);
    EXPECT_NE(message.find("use skinning.skeleton"), std::string::npos);
    EXPECT_NE(message.find("Maps/Test/SM_body.json"), std::string::npos);
}

TEST(AnimationSkeleton, RejectsSkinningClipsWithoutSkeleton)
{
    const json meshAsset = {
        {"type", "mesh"},
        {"modelDataPath", "Maps/Test/SM_body.fbx"},
        {"materialSlots", json::array({
            {{"name", "Default"}, {"materialInstancePath", "Maps/Test/MI_default.json"}}})},
        {"skinning", {
            {"clips", json::array({{{"id", "walk"}, {"path", "Maps/Test/walk.fbx"}}})}}}};

    const std::string message = ErrorOf([&] {
        MeshAssetValidator::BuildLoadPlan("Maps/Test/SM_body.json", meshAsset); });
    EXPECT_NE(message.find("skinning.skeleton"), std::string::npos);
    EXPECT_NE(message.find("Maps/Test/SM_body.json"), std::string::npos);
}

TEST(AnimationSkeleton, SkeletonAndSequencePathsExtendPlanAndCacheKey)
{
    const json meshAsset = {
        {"type", "mesh"},
        {"modelDataPath", "Maps/Test/SM_body.fbx"},
        {"materialSlots", json::array({
            {{"name", "Default"}, {"materialInstancePath", "Maps/Test/MI_default.json"}}})},
        {"skinning", {
            {"skeleton", "Maps/Test/Skeletons/SK_body.json"},
            {"clips", json::array({{{"id", "walk"},
                {"path", "Maps/Test/walk.fbx"},
                {"sequence", "Maps/Test/walk.anim.json"}}})}}}};

    const MeshAssetBuildPlan plan =
        MeshAssetValidator::BuildLoadPlan("Maps/Test/SM_body.json", meshAsset);
    EXPECT_TRUE(plan.IsSkeletal());
    EXPECT_EQ(plan.skeletonPath, "Maps/Test/Skeletons/SK_body.json");
    ASSERT_EQ(plan.animationClips.size(), 1u);
    EXPECT_EQ(plan.animationClips[0].sequencePath, "Maps/Test/walk.anim.json");
    // 缓存键只保留 |skeleton= 段，旧 |skinning= 段已随 root 字段删除。
    EXPECT_EQ(plan.modelCacheKey,
        "Maps/Test/SM_body.fbx|skeleton=Maps/Test/Skeletons/SK_body.json"
        "|clip=walk:Maps/Test/walk.fbx"
        "|sequence=walk:Maps/Test/walk.anim.json");
}

TEST(AnimationSkeleton, RejectsInvalidSequencePathSyntax)
{
    json meshAsset = {
        {"type", "mesh"},
        {"modelDataPath", "Maps/Test/SM_body.fbx"},
        {"materialSlots", json::array({
            {{"name", "Default"}, {"materialInstancePath", "Maps/Test/MI_default.json"}}})},
        {"skinning", {
            {"skeleton", "Maps/Test/Skeletons/SK_body.json"},
            {"clips", json::array({{{"id", "walk"},
                {"path", "Maps/Test/walk.fbx"},
                {"sequence", "not-json.txt"}}})}}}};
    EXPECT_THROW(MeshAssetValidator::BuildLoadPlan("SM_body.json", meshAsset), std::runtime_error);

    meshAsset["skinning"]["clips"][0]["sequence"] = "../walk.anim.json";
    EXPECT_THROW(MeshAssetValidator::BuildLoadPlan("SM_body.json", meshAsset), std::runtime_error);
}
}
