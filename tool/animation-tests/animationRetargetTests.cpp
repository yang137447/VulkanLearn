#include "animation/animationRetarget.h"
#include "animation/animationScriptData.h"
#include "animation/motionRuntimeDatabase.h"
#include "animation/skeletalPlayback.h"
#include "mesh/validation/meshAssetValidator.h"

#include <ozz/animation/offline/animation_builder.h>
#include <ozz/animation/offline/raw_skeleton.h>
#include <ozz/animation/offline/skeleton_builder.h>
#include <gtest/gtest.h>
#include <limits>

namespace
{
using namespace VL::Animation;
using nlohmann::json;
constexpr float Pi = 3.14159265358979323846f;

Eigen::Matrix4f Trs(const Eigen::Vector3f& translation, float yaw = 0)
{
    Eigen::Matrix4f result = Eigen::Matrix4f::Identity();
    result.block<3, 3>(0, 0) =
        Eigen::AngleAxisf(yaw * Pi / 180, Eigen::Vector3f::UnitY()).toRotationMatrix();
    result.block<3, 1>(0, 3) = translation;
    return result;
}

SkeletonMesh MakeMesh(float length = 10, float yaw = 0, bool extraJoint = false)
{
    SkeletonMesh mesh;
    mesh.joints = {
        {"root", -1, Trs({3, 2, 1}), Trs({3, 2, 1})},
        {"limb", 0, Trs({0, length, 0}, yaw), Eigen::Matrix4f::Identity()}};
    if (extraJoint)
        mesh.joints.push_back(
            {"tip", 1, Trs({0, 0, 7}, 20), Eigen::Matrix4f::Identity()});

    // 夹具用同一份 TRS 构建 ozz 和权重流，独立断言仍以手算模型矩阵为准。
    ozz::animation::offline::RawSkeleton raw;
    raw.roots.resize(1);
    auto* joint = &raw.roots[0];
    for (size_t index = 0; index < mesh.joints.size(); ++index)
    {
        const auto& value = mesh.joints[index];
        if (index > 0)
        {
            joint->children.resize(1);
            joint = &joint->children[0];
        }
        joint->name = value.name.c_str();
        joint->transform = ozz::math::Transform::identity();
        const Eigen::Vector3f translation = value.restLocal.block<3, 1>(0, 3);
        const Eigen::Quaternionf rotation(value.restLocal.block<3, 3>(0, 0));
        joint->transform.translation = {translation.x(), translation.y(), translation.z()};
        joint->transform.rotation = {rotation.x(), rotation.y(), rotation.z(), rotation.w()};
        mesh.joints[index].restModel = index == 0 ? value.restLocal :
            (mesh.joints[index - 1].restModel * value.restLocal).eval();
    }
    mesh.runtime = ozz::animation::offline::SkeletonBuilder()(raw);
    SkinSection section;
    for (size_t index = 0; index < mesh.joints.size(); ++index)
    {
        Vertex vertex{};
        vertex.position = mesh.joints[index].restModel.block<3, 1>(0, 3);
        section.vertices.push_back(vertex);
        section.influenceOffsets.push_back(static_cast<uint32_t>(index));
        section.influences.push_back({static_cast<uint32_t>(index), 1});
        section.inverseBinds.push_back(mesh.joints[index].restModel.inverse());
    }
    section.influenceOffsets.push_back(static_cast<uint32_t>(mesh.joints.size()));
    mesh.sections.push_back(std::move(section));
    return mesh;
}

Clip MakeClip(const SkeletonMesh& mesh, const std::string& id, float yaw)
{
    Clip clip;
    clip.id = id;
    clip.raw.duration = 2;
    clip.raw.tracks.resize(mesh.joints.size());
    for (size_t index = 0; index < mesh.joints.size(); ++index)
    {
        const auto& rest = mesh.joints[index].restLocal;
        const Eigen::Vector3f translation = rest.block<3, 1>(0, 3);
        Eigen::Quaternionf rotation(rest.block<3, 3>(0, 0));
        if (index == 1)
            rotation = Eigen::AngleAxisf(yaw * Pi / 180, Eigen::Vector3f::UnitX()) * rotation;
        auto& track = clip.raw.tracks[index];
        track.translations = {{0, {translation.x(), translation.y(), translation.z()}}};
        track.rotations = {{0, {rotation.x(), rotation.y(), rotation.z(), rotation.w()}}};
        track.scales = {{0, ozz::math::Float3::one()}};
    }
    clip.raw.tracks[0].translations.push_back({2, {23, 2, 41}});
    clip.runtime = ozz::animation::offline::AnimationBuilder()(clip.raw);
    clip.sequence.id = id;
    clip.sequence.duration = 2;
    clip.sequence.notifies.push_back({"Step", "1", 0.5f, 0});
    return clip;
}

RetargetProfile MakeProfile()
{
    return {"SK_Source", "SK_Target", "root", "root",
        {{"root", "root"}, {"limb", "limb"}}};
}

json ProfileJson()
{
    return {{"format", "vl.retarget.v1"}, {"sourceSkeleton", "SK_Source"},
        {"targetSkeleton", "SK_Target"}, {"sourceRoot", "root"}, {"targetRoot", "root"},
        {"joints", json::array({{{"source", "root"}, {"target", "root"}},
            {{"source", "limb"}, {"target", "limb"}}})}};
}

std::shared_ptr<SkeletalAsset> MakeTarget()
{
    auto source = std::make_shared<SkeletalAsset>();
    source->mesh = MakeMesh();
    source->skeleton.id = "SK_Source";
    source->clips.push_back(MakeClip(source->mesh, "idle", 0));
    source->clips.push_back(MakeClip(source->mesh, "bend", 90));
    Montage montage;
    montage.id = "AM_Bend";
    montage.sourcePath = "Maps/Test/AM_Bend.json";
    montage.derivedClipId = "bend";
    montage.length = 2;
    montage.sections.push_back({"Default", 0, 2, "", 0, ""});
    source->montages.push_back(std::move(montage));

    auto target = std::make_shared<SkeletalAsset>();
    target->mesh = MakeMesh(25, 90, true);
    target->skeleton.id = "SK_Target";
    target->animationSource = std::move(source);
    target->retargetProfile = std::make_shared<const RetargetProfile>(MakeProfile());
    return target;
}

json TargetDescriptor()
{
    return {{"type", "mesh"}, {"modelDataPath", "Maps/Test/target.fbx"},
        {"materialSlots", json::array({
            {{"name", "Default"}, {"materialInstancePath", "Maps/Test/MI_default.json"}}})},
        {"skinning", {{"skeleton", "Maps/Test/SK_Target.json"}, {"clips", json::array()},
            {"sourceModel", "Maps/Test/SM_Source.json"},
            {"retarget", "Maps/Test/RT_Target.json"}}}};
}

TEST(AnimationRetarget, ParsesExplicitSkeletonIdentitiesAndJointPairs)
{
    const auto profile = ParseRetargetProfile(ProfileJson(), "RT_Target.json");
    EXPECT_EQ(profile.sourceSkeleton, "SK_Source");
    EXPECT_EQ(profile.targetSkeleton, "SK_Target");
    EXPECT_EQ(profile.sourceRoot, "root");
    EXPECT_EQ(profile.targetRoot, "root");
    ASSERT_EQ(profile.joints.size(), 2u);
    EXPECT_EQ(profile.joints[1].source, "limb");
    EXPECT_EQ(profile.joints[1].target, "limb");
}

TEST(AnimationRetarget, RejectsWrongFormatAndEmptyJointNames)
{
    auto value = ProfileJson();
    value["format"] = "vl.retarget.v2";
    EXPECT_THROW(ParseRetargetProfile(value, "RT.json"), std::invalid_argument);
    value = ProfileJson();
    value["joints"][0]["target"] = "";
    EXPECT_THROW(ParseRetargetProfile(value, "RT.json"), std::invalid_argument);
    value = ProfileJson();
    value.erase("sourceSkeleton");
    EXPECT_THROW(ParseRetargetProfile(value, "RT.json"), std::invalid_argument);
}

TEST(AnimationRetarget, RejectsUnknownSourceAndTargetJoints)
{
    const auto source = MakeMesh();
    const auto target = MakeMesh();
    auto profile = MakeProfile();
    profile.joints[1].source = "unknown";
    EXPECT_THROW(profile.Validate(source, target), std::invalid_argument);
    profile = MakeProfile();
    profile.joints[1].target = "unknown";
    EXPECT_THROW(profile.Validate(source, target), std::invalid_argument);
}

TEST(AnimationRetarget, RejectsDuplicateSourceAndTargetMappings)
{
    const auto source = MakeMesh();
    const auto target = MakeMesh();
    auto profile = MakeProfile();
    profile.joints[1].source = "root";
    EXPECT_THROW(profile.Validate(source, target), std::invalid_argument);
    profile = MakeProfile();
    profile.joints[1].target = "root";
    EXPECT_THROW(profile.Validate(source, target), std::invalid_argument);
}

TEST(AnimationRetarget, RequiresRootPairAndJointZeroRoots)
{
    const auto source = MakeMesh();
    const auto target = MakeMesh();
    auto profile = MakeProfile();
    profile.joints.erase(profile.joints.begin());
    EXPECT_THROW(profile.Validate(source, target), std::invalid_argument);
    profile = MakeProfile();
    profile.sourceRoot = "limb";
    EXPECT_THROW(profile.Validate(source, target), std::invalid_argument);
    profile = MakeProfile();
    profile.targetRoot = "limb";
    EXPECT_THROW(profile.Validate(source, target), std::invalid_argument);
}

TEST(AnimationRetarget, RejectsDisconnectedOrUnorderedHierarchy)
{
    const auto source = MakeMesh();
    auto target = MakeMesh(25, 90, true);
    target.joints[2].parent = -1;
    EXPECT_THROW(MakeProfile().Validate(source, target), std::invalid_argument);
    target.joints[2].parent = 2;
    EXPECT_THROW(MakeProfile().Validate(source, target), std::invalid_argument);
}

TEST(AnimationRetarget, RestPosePreservesTargetWithDifferentJointCountAndAxes)
{
    const auto source = MakeMesh();
    const auto target = MakeMesh(25, 90, true);
    const auto rest = source.runtime->joint_rest_poses();
    const auto result = CompiledRetarget(source, target, MakeProfile()).Apply(
        LocalPose(rest.begin(), rest.end()));
    const auto models = LocalToModels(target, result);
    ASSERT_EQ(models.size(), 3u);
    for (size_t index = 0; index < models.size(); ++index)
        EXPECT_TRUE(models[index].isApprox(target.joints[index].restModel, 0.0001f));
}

TEST(AnimationRetarget, ParsesNamedCalibrationWithXyzwQuaternion)
{
    auto value = ProfileJson();
    const float half = std::sqrt(0.5f);
    value["targetPose"] = {{"name", "AlignedArms"}, {"rotationOffsets", json::array({
        {{"joint", "limb"}, {"rotation", {0, 0, half, half}}}})}};
    const auto profile = ParseRetargetProfile(value, "RT.json");
    EXPECT_EQ(profile.sourcePose.name, "BindPose");
    EXPECT_TRUE(profile.sourcePose.rotationOffsets.empty());
    EXPECT_EQ(profile.targetPose.name, "AlignedArms");
    ASSERT_EQ(profile.targetPose.rotationOffsets.size(), 1u);
    EXPECT_TRUE((profile.targetPose.rotationOffsets[0].rotation * Eigen::Vector3f::UnitX())
        .isApprox(Eigen::Vector3f::UnitY(), 0.0001f));
}

TEST(AnimationRetarget, RejectsMalformedCalibrationInsteadOfIgnoringIt)
{
    const auto valid = json{{"name", "Aligned"}, {"rotationOffsets", json::array({
        {{"joint", "limb"}, {"rotation", {0, 0, 0, 1}}}})}};
    for (const char* key : {"sourcePose", "targetPose"})
    {
        for (const auto& malformed : {json(nullptr), json::array(), json{{"name", ""}},
            json{{"name", "Aligned"}, {"rotationOffsets", json::object()}},
            json{{"name", "Aligned"}, {"rotationOffsets", json::array()}, {"space", "world"}}})
        {
            auto value = ProfileJson();
            value[key] = malformed;
            EXPECT_THROW(ParseRetargetProfile(value, "RT.json"), std::invalid_argument);
        }
        for (const auto& rotation : {json::array({0, 0, 1}), json::array({0, 0, 0, "1"}),
            json::array({0, 0, 0, 0}), json::array({0, 0, 0, 2}),
            json::array({0, 0, 0, std::numeric_limits<double>::infinity()})})
        {
            auto value = ProfileJson();
            value[key] = valid;
            value[key]["rotationOffsets"][0]["rotation"] = rotation;
            EXPECT_THROW(ParseRetargetProfile(value, "RT.json"), std::invalid_argument);
        }
    }
}

TEST(AnimationRetarget, CalibrationRejectsUnknownJoint)
{
    auto profile = MakeProfile();
    profile.sourcePose.rotationOffsets.push_back({"absent", Eigen::Quaternionf::Identity()});
    EXPECT_THROW(profile.Validate(MakeMesh(), MakeMesh()), std::invalid_argument);
    profile = MakeProfile();
    profile.targetPose.rotationOffsets.push_back({"absent", Eigen::Quaternionf::Identity()});
    EXPECT_THROW(profile.Validate(MakeMesh(), MakeMesh()), std::invalid_argument);
}

TEST(AnimationRetarget, CalibrationRejectsDuplicateJoint)
{
    auto profile = MakeProfile();
    profile.targetPose.rotationOffsets = {{"limb", Eigen::Quaternionf::Identity()},
        {"limb", Eigen::Quaternionf::Identity()}};
    EXPECT_THROW(profile.Validate(MakeMesh(), MakeMesh()), std::invalid_argument);
}

TEST(AnimationRetarget, CalibrationRejectsLocomotionRootOffset)
{
    auto profile = MakeProfile();
    profile.sourcePose.rotationOffsets.push_back({"root", Eigen::Quaternionf::Identity()});
    EXPECT_THROW(profile.Validate(MakeMesh(), MakeMesh()), std::invalid_argument);
    profile = MakeProfile();
    profile.targetPose.rotationOffsets.push_back({"root", Eigen::Quaternionf::Identity()});
    EXPECT_THROW(profile.Validate(MakeMesh(), MakeMesh()), std::invalid_argument);
}

TEST(AnimationRetarget, NativeCalibrationRejectsInvalidQuaternion)
{
    auto profile = MakeProfile();
    profile.targetPose.rotationOffsets.push_back({"limb", Eigen::Quaternionf(0, 0, 0, 0)});
    EXPECT_THROW(profile.Validate(MakeMesh(), MakeMesh()), std::invalid_argument);
    profile.targetPose.rotationOffsets[0].rotation =
        Eigen::Quaternionf(std::numeric_limits<float>::quiet_NaN(), 0, 0, 0);
    EXPECT_THROW(profile.Validate(MakeMesh(), MakeMesh()), std::invalid_argument);
}

TEST(AnimationRetarget, SourceCalibrationMapsToTargetCalibrationUsingBindLocalAxes)
{
    const auto source = MakeMesh(10, 90);
    const auto target = MakeMesh(25, -35);
    auto profile = MakeProfile();
    const Eigen::Quaternionf sourceOffset(Eigen::AngleAxisf(Pi / 6, Eigen::Vector3f::UnitX()));
    const Eigen::Quaternionf targetOffset(Eigen::AngleAxisf(Pi / 4, Eigen::Vector3f::UnitZ()));
    profile.sourcePose.rotationOffsets.push_back({"limb", sourceOffset});
    profile.targetPose.rotationOffsets.push_back({"limb", targetOffset});
    auto clip = MakeClip(source, "source-reference", 0);
    // 手算非交换旋转；若把局部偏移左乘 bind，本测试不会得到目标校准姿态。
    const Eigen::Quaternionf sourceReference =
        Eigen::Quaternionf(Eigen::AngleAxisf(Pi / 2, Eigen::Vector3f::UnitY())) * sourceOffset;
    clip.raw.tracks[1].rotations = {{0, {sourceReference.x(), sourceReference.y(),
        sourceReference.z(), sourceReference.w()}}};
    clip.runtime = ozz::animation::offline::AnimationBuilder()(clip.raw);
    const auto result = RetargetLocalPose(source, target, profile, SampleLocals(source, clip, 0));
    const auto models = LocalToModels(target, result);
    const Eigen::Matrix3f expected =
        (Eigen::Quaternionf(Eigen::AngleAxisf(-35 * Pi / 180, Eigen::Vector3f::UnitY())) *
            targetOffset).toRotationMatrix();
    EXPECT_TRUE((models[1].block<3, 3>(0, 0).isApprox(expected, 0.001f)));
    EXPECT_NEAR((models[1].block<3, 1>(0, 3) - models[0].block<3, 1>(0, 3)).norm(), 25, 0.001f);
}

TEST(AnimationRetarget, MotionDeltaIsMeasuredFromCalibratedSourceReference)
{
    const auto source = MakeMesh();
    const auto target = MakeMesh(25, 90);
    auto profile = MakeProfile();
    profile.sourcePose.rotationOffsets.push_back(
        {"limb", Eigen::Quaternionf(Eigen::AngleAxisf(Pi / 6, Eigen::Vector3f::UnitX()))});
    profile.targetPose.rotationOffsets.push_back(
        {"limb", Eigen::Quaternionf(Eigen::AngleAxisf(Pi / 4, Eigen::Vector3f::UnitZ()))});
    const auto clip = MakeClip(source, "bend", 50);
    const auto models = LocalToModels(target,
        RetargetLocalPose(source, target, profile, SampleLocals(source, clip, 0)));
    const Eigen::Matrix3f expected =
        (Eigen::AngleAxisf(20 * Pi / 180, Eigen::Vector3f::UnitX()) *
            Eigen::AngleAxisf(Pi / 2, Eigen::Vector3f::UnitY()) *
            Eigen::AngleAxisf(Pi / 4, Eigen::Vector3f::UnitZ())).toRotationMatrix();
    EXPECT_TRUE((models[1].block<3, 3>(0, 0).isApprox(expected, 0.001f)));
}

TEST(AnimationRetarget, UnmappedCalibrationInheritsParentWithoutChangingBindData)
{
    const auto source = MakeMesh();
    const auto target = MakeMesh(25, 90, true);
    const Eigen::Matrix4f originalBind = target.joints[2].restLocal;
    const Eigen::Matrix4f originalInverseBind = target.sections[0].inverseBinds[2];
    auto profile = MakeProfile();
    profile.targetPose.rotationOffsets.push_back(
        {"tip", Eigen::Quaternionf(Eigen::AngleAxisf(Pi / 6, Eigen::Vector3f::UnitX()))});
    const auto clip = MakeClip(source, "bend", 60);
    const auto models = LocalToModels(target,
        RetargetLocalPose(source, target, profile, SampleLocals(source, clip, 0)));
    Eigen::Matrix4f expectedLocal = Trs({0, 0, 7}, 20);
    expectedLocal.block<3, 3>(0, 0) *=
        Eigen::AngleAxisf(Pi / 6, Eigen::Vector3f::UnitX()).toRotationMatrix();
    EXPECT_TRUE((models[1].inverse() * models[2]).isApprox(expectedLocal, 0.001f));
    EXPECT_TRUE(target.joints[2].restLocal.isApprox(originalBind, 0));
    EXPECT_TRUE(target.sections[0].inverseBinds[2].isApprox(originalInverseBind, 0));
}

TEST(AnimationRetarget, TransfersModelRotationDeltaAndRetainsTargetBoneLength)
{
    const auto source = MakeMesh();
    const auto target = MakeMesh(25, 90, true);
    const auto clip = MakeClip(source, "bend", 90);
    const auto result = RetargetLocalPose(source, target, MakeProfile(),
        SampleLocals(source, clip, 0));
    const auto models = LocalToModels(target, result);
    const Eigen::Matrix3f expected =
        Eigen::AngleAxisf(Pi / 2, Eigen::Vector3f::UnitX()).toRotationMatrix() *
        Eigen::AngleAxisf(Pi / 2, Eigen::Vector3f::UnitY()).toRotationMatrix();
    EXPECT_TRUE((models[1].block<3, 3>(0, 0).isApprox(expected, 0.001f)));
    EXPECT_NEAR((models[1].block<3, 1>(0, 3) -
        models[0].block<3, 1>(0, 3)).norm(), 25, 0.001f);
    // tip 没有映射；继承父骨动作，但自己的局部绑定 TRS 保持原值。
    EXPECT_TRUE((models[1].inverse() * models[2]).isApprox(
        target.joints[2].restLocal, 0.001f));
}

TEST(AnimationRetarget, RootTranslationUsesDifferenceWithoutHeightScaling)
{
    const auto source = MakeMesh();
    auto target = MakeMesh(25, 90, true);
    const auto clip = MakeClip(source, "bend", 0);
    const auto pose = RetargetLocalPose(source, target, MakeProfile(),
        SampleLocals(source, clip, 1));
    const auto models = LocalToModels(target, pose);
    EXPECT_NEAR(models[0](0, 3), 13, 0.0001f);
    EXPECT_NEAR(models[0](1, 3), 2, 0.0001f);
    EXPECT_NEAR(models[0](2, 3), 21, 0.0001f);
}

TEST(AnimationRetarget, RejectsSourcePoseWithWrongSoaSize)
{
    const auto source = MakeMesh();
    const auto target = MakeMesh();
    EXPECT_THROW(CompiledRetarget(source, target, MakeProfile()).Apply({}),
        std::invalid_argument);
}

TEST(AnimationRetarget, PlaybackSamplesSourceAndPublishesTargetPalette)
{
    const auto asset = MakeTarget();
    SkeletalPlayback player(asset);
    player.Play("bend", false, 1, false);
    player.Tick(1);
    EXPECT_EQ(player.SelectedClipId(), "bend");
    EXPECT_FLOAT_EQ(player.Time(), 1);
    EXPECT_FLOAT_EQ(player.Duration(), 2);
    ASSERT_EQ(player.Models().size(), 3u);
    ASSERT_EQ(player.Pose(0)->matrices.size(), 3u);
    EXPECT_NEAR(player.Models()[0](2, 3), 21, 0.0001f);
    EXPECT_NEAR(player.RootMotionDelta().z(), 20, 0.0001f);
    EXPECT_EQ(player.FiredNotifies().size(), 1u);
    EXPECT_TRUE(asset->clips.empty());
    for (size_t index = 0; index < 3; ++index)
        EXPECT_TRUE(player.Pose(0)->matrices[index].isApprox(
            player.Models()[index] * asset->mesh.sections[0].inverseBinds[index], 0.0001f));
}

TEST(AnimationRetarget, InPlaceAndBlendOperateOnTargetPose)
{
    const auto asset = MakeTarget();
    SkeletalPlayback player(asset);
    player.Play("idle", true, 1, true);
    player.TransitionTo("bend", 0, true, 1);
    player.Tick(0.5f);
    EXPECT_TRUE(player.Models()[0].isApprox(asset->mesh.joints[0].restModel, 0.0001f));
    const Eigen::Matrix3f expected =
        Eigen::AngleAxisf(Pi / 4, Eigen::Vector3f::UnitX()).toRotationMatrix() *
        Eigen::AngleAxisf(Pi / 2, Eigen::Vector3f::UnitY()).toRotationMatrix();
    EXPECT_TRUE((player.Models()[1].block<3, 3>(0, 0).isApprox(expected, 0.002f)));
    EXPECT_TRUE(player.Pose(0)->matrices[0].isIdentity(0.0001f));
    EXPECT_NEAR((player.Models()[1].block<3, 1>(0, 3) -
        player.Models()[0].block<3, 1>(0, 3)).norm(), 25, 0.001f);
}

TEST(AnimationRetarget, MontageAndScriptCatalogUseSourceAnimation)
{
    const auto asset = MakeTarget();
    const auto* montage = asset->FindMontage("AM_Bend");
    ASSERT_NE(montage, nullptr);
    const auto catalog = BuildAnimationScriptAssets(*asset);
    ASSERT_EQ(catalog.at("montages").size(), 1u);
    SkeletalPlayback player(asset);
    PlayMontage(player, *montage, {}, false, 1, true);
    player.Tick(0.5f);
    EXPECT_EQ(player.SelectedClipId(), "bend");
    EXPECT_EQ(player.MontageId(), "AM_Bend");
}

TEST(AnimationRetarget, PlaybackRejectsSourceWithoutProfile)
{
    auto asset = MakeTarget();
    asset->retargetProfile.reset();
    EXPECT_THROW(SkeletalPlayback{asset}, std::invalid_argument);
}

TEST(AnimationRetarget, PlaybackRejectsProfileWithoutSource)
{
    auto asset = MakeTarget();
    asset->animationSource.reset();
    EXPECT_THROW(SkeletalPlayback{asset}, std::invalid_argument);
}

TEST(AnimationRetarget, PlaybackRejectsNestedAnimationSources)
{
    auto asset = MakeTarget();
    asset->animationSource = MakeTarget();
    EXPECT_THROW(SkeletalPlayback{asset}, std::invalid_argument);
}

TEST(AnimationRetarget, PlaybackRejectsTargetOwnedClips)
{
    auto asset = MakeTarget();
    asset->clips.push_back(MakeClip(asset->mesh, "target", 0));
    EXPECT_THROW(SkeletalPlayback{asset}, std::invalid_argument);
}

TEST(AnimationRetarget, PlaybackRejectsTargetOwnedMontages)
{
    auto asset = MakeTarget();
    asset->montages.push_back(Montage{});
    EXPECT_THROW(SkeletalPlayback{asset}, std::invalid_argument);
}

TEST(AnimationRetarget, DescriptorPairsSourceAndProfileAndExtendsCacheKey)
{
    auto descriptor = TargetDescriptor();
    const auto plan = MeshAssetValidator::BuildLoadPlan("Maps/Test/SM_Target.json", descriptor);
    EXPECT_EQ(plan.animationSourceModelPath, "Maps/Test/SM_Source.json");
    EXPECT_EQ(plan.retargetProfilePath, "Maps/Test/RT_Target.json");
    EXPECT_NE(plan.modelCacheKey.find("|animationSourceModel=Maps/Test/SM_Source.json"),
        std::string::npos);
    EXPECT_NE(plan.modelCacheKey.find("|retarget=Maps/Test/RT_Target.json"), std::string::npos);
    descriptor["skinning"].erase("retarget");
    EXPECT_THROW(MeshAssetValidator::BuildLoadPlan("Maps/Test/SM_Target.json", descriptor),
        std::runtime_error);
    descriptor = TargetDescriptor();
    descriptor["skinning"].erase("sourceModel");
    EXPECT_THROW(MeshAssetValidator::BuildLoadPlan("Maps/Test/SM_Target.json", descriptor),
        std::runtime_error);
}

TEST(AnimationRetarget, DescriptorRejectsSelfReferenceAndTargetOwnedAnimations)
{
    auto descriptor = TargetDescriptor();
    descriptor["skinning"]["sourceModel"] = "Maps/Test/SM_Target.json";
    EXPECT_THROW(MeshAssetValidator::BuildLoadPlan("Maps/Test/SM_Target.json", descriptor),
        std::runtime_error);
    descriptor = TargetDescriptor();
    descriptor["skinning"]["clips"].push_back({{"id", "walk"}, {"path", "walk.fbx"}});
    EXPECT_THROW(MeshAssetValidator::BuildLoadPlan("Maps/Test/SM_Target.json", descriptor),
        std::runtime_error);
    descriptor = TargetDescriptor();
    descriptor["skinning"]["montages"] =
        json::array({{{"id", "AM_Bend"}, {"path", "Maps/Test/AM_Bend.json"}}});
    EXPECT_THROW(MeshAssetValidator::BuildLoadPlan("Maps/Test/SM_Target.json", descriptor),
        std::runtime_error);
}

TEST(AnimationRetarget, RejectsMotionDatabaseBeforeAnyDiskAccess)
{
    const auto asset = MakeTarget();
    try
    {
        LoadMotionRuntimeDatabase("missing-resource-root", "missing-db.json", "target.json", *asset);
        FAIL() << "Retargeted Motion Matching must reject before reading inputs";
    }
    catch (const std::invalid_argument& error)
    {
        EXPECT_NE(std::string(error.what()).find("source-space pose history"), std::string::npos);
    }
}
}
