#include "animation/skeletalPlayback.h"

#include <cmath>
#include <limits>
#include <ozz/animation/offline/animation_builder.h>
#include <ozz/animation/offline/raw_skeleton.h>
#include <ozz/animation/offline/skeleton_builder.h>
#include <gtest/gtest.h>

namespace
{
using namespace VL::Animation;

Eigen::Matrix4f Translation(float x, float y, float z)
{
    Eigen::Matrix4f value = Eigen::Matrix4f::Identity();
    value.block<3, 1>(0, 3) = Eigen::Vector3f(x, y, z);
    return value;
}

Clip MakeClip(const std::string& id, float hipX, float hipYawDegrees, float hipScale,
    bool displacedRoot = false)
{
    Clip clip;
    clip.id = id;
    clip.raw.duration = 4;
    clip.raw.tracks.resize(3);
    const float radians = hipYawDegrees * 3.14159265358979323846f / 180;
    auto& root = clip.raw.tracks[0];
    root.translations = {{0, {3, 2, 1}}, {4, {83, 42, 121}}};
    root.rotations = {{0, displacedRoot ?
        ozz::math::Quaternion{0, std::sqrt(0.5f), 0, std::sqrt(0.5f)} :
        ozz::math::Quaternion::identity()}};
    root.scales = {{0, displacedRoot ? ozz::math::Float3{2, 3, 4} : ozz::math::Float3::one()}};
    auto& hip = clip.raw.tracks[1];
    hip.translations = {{0, {hipX, 10, 0}}};
    hip.rotations = {{0, {0, std::sin(radians / 2), 0, std::cos(radians / 2)}}};
    hip.scales = {{0, {hipScale, hipScale, hipScale}}};
    auto& foot = clip.raw.tracks[2];
    foot.translations = {{0, {0, 0, 4}}};
    foot.rotations = {{0, ozz::math::Quaternion::identity()}};
    foot.scales = {{0, ozz::math::Float3::one()}};
    ozz::animation::offline::AnimationBuilder builder;
    clip.runtime = builder(clip.raw);
    return clip;
}

std::shared_ptr<SkeletalAsset> MakeAsset()
{
    auto asset = std::make_shared<SkeletalAsset>();
    ozz::animation::offline::RawSkeleton raw;
    raw.roots.resize(1);
    raw.roots[0].name = "root";
    raw.roots[0].transform = ozz::math::Transform::identity();
    raw.roots[0].transform.translation = {3, 2, 1};
    raw.roots[0].children.resize(1);
    auto& hip = raw.roots[0].children[0];
    hip.name = "hip";
    hip.transform = ozz::math::Transform::identity();
    hip.transform.translation = {0, 10, 0};
    hip.children.resize(1);
    hip.children[0].name = "foot";
    hip.children[0].transform = ozz::math::Transform::identity();
    hip.children[0].transform.translation = {0, 0, 4};
    ozz::animation::offline::SkeletonBuilder skeletonBuilder;
    asset->mesh.runtime = skeletonBuilder(raw);
    asset->mesh.joints = {
        {"root", -1, Translation(3, 2, 1), Translation(3, 2, 1)},
        {"hip", 0, Translation(0, 10, 0), Translation(3, 12, 1)},
        {"foot", 1, Translation(0, 0, 4), Translation(3, 12, 5)}};
    SkinSection section;
    for (uint32_t joint = 0; joint < 3; ++joint)
    {
        Vertex vertex{};
        vertex.position = asset->mesh.joints[joint].restModel.block<3, 1>(0, 3);
        section.vertices.push_back(vertex);
        section.influenceOffsets.push_back(joint);
        section.influences.push_back({joint, 1});
        section.inverseBinds.push_back(asset->mesh.joints[joint].restModel.inverse());
    }
    section.influenceOffsets.push_back(3);
    // 让主 blend 夹具走运行期快路径（导入器生产的 section 也带这套预计算）。
    ComputeJointBindBounds(section);
    asset->mesh.sections.push_back(std::move(section));
    asset->clips.push_back(MakeClip("a", 0, 0, 1));
    asset->clips.push_back(MakeClip("b", 8, 90, 2));
    asset->clips.push_back(MakeClip("c", -8, -90, 1));
    asset->clips.push_back(MakeClip("root-trs", 0, 0, 1, true));
    // 匀速位移片段：末段 hip.x 以 100 cm/s 前进，用于验证惯性化的出帧速度。
    Clip travel = MakeClip("travel", -392, 0, 1);
    travel.raw.tracks[1].translations = {{0, {-392, 10, 0}}, {4, {8, 10, 0}}};
    ozz::animation::offline::AnimationBuilder travelBuilder;
    travel.runtime = travelBuilder(travel.raw);
    asset->clips.push_back(std::move(travel));
    // 与 travel 末帧姿态重合、但速度为零的目标片段。
    asset->clips.push_back(MakeClip("still", 8, 0, 1));
    return asset;
}

// 匀速位移片段：用于检查第二片段是否按 A 的归一化相位采样。
Clip MakeTravelClip(const std::string& id, float duration, float hipFrom, float hipTo)
{
    Clip clip;
    clip.id = id;
    clip.raw.duration = duration;
    clip.raw.tracks.resize(3);
    auto& root = clip.raw.tracks[0];
    root.translations = {{0, {3, 2, 1}}};
    root.rotations = {{0, ozz::math::Quaternion::identity()}};
    root.scales = {{0, ozz::math::Float3::one()}};
    auto& hip = clip.raw.tracks[1];
    hip.translations = {{0, {hipFrom, 10, 0}}, {duration, {hipTo, 10, 0}}};
    hip.rotations = {{0, ozz::math::Quaternion::identity()}};
    hip.scales = {{0, ozz::math::Float3::one()}};
    auto& foot = clip.raw.tracks[2];
    foot.translations = {{0, {0, 0, 4}}};
    foot.rotations = {{0, ozz::math::Quaternion::identity()}};
    foot.scales = {{0, ozz::math::Float3::one()}};
    ozz::animation::offline::AnimationBuilder builder;
    clip.runtime = builder(clip.raw);
    return clip;
}

std::shared_ptr<SkeletalAsset> MakeBlendAsset()
{
    auto asset = MakeAsset();
    // A：4 s 内 hip.x 0→40（10 cm/s）；B：2 s 内 0→80（40 cm/s）。
    asset->clips.push_back(MakeTravelClip("travel-slow", 4, 0, 40));
    asset->clips.push_back(MakeTravelClip("travel-fast", 2, 0, 80));
    return asset;
}

TEST(SkeletalBlendSpace, SecondClipFollowsNormalizedPhaseAndWeight)
{
    auto asset = MakeBlendAsset();
    SkeletalPlayback player(asset);
    player.Play("travel-slow", true, 1, true);
    // 2 s → A 相位 0.5；B（2 s 片段）必须采样到 1 s，而不是从 0 开始推进 2 s。
    player.Tick(2.0f);
    EXPECT_FLOAT_EQ(player.Time(), 2.0f);
    player.SetBlendSpace1D("travel-slow", "travel-fast", 0.5f);
    ASSERT_TRUE(player.BlendSpaceActive());
    EXPECT_EQ(player.BlendSpaceClipId(), "travel-fast");
    player.Tick(0.0f);
    player.Tick(1.0f / 60);
    const float phase = player.Time() / 4.0f;
    // Models 含 root 的 3 cm 绑定平移，比较前加回。
    const float expected = 3.0f + 0.5f * (40.0f * phase) + 0.5f * (80.0f * phase);
    EXPECT_NEAR(player.Models()[1](0, 3), expected, 0.05f);
    // 第二片段没有自己的根轨迹：root motion 仍只来自 A。
    EXPECT_NEAR(player.BlendSpaceWeight(), 0.5f, 1e-6f);
}

TEST(SkeletalBlendSpace, EndpointWeightsMatchSingleClipPoses)
{
    auto asset = MakeBlendAsset();
    SkeletalPlayback pure(asset);
    pure.Play("travel-slow", true, 1, true);
    pure.Tick(1.0f);
    pure.Tick(1.0f / 60);
    const float pureHip = pure.Models()[1](0, 3);

    SkeletalPlayback zero(asset);
    zero.Play("travel-slow", true, 1, true);
    zero.Tick(1.0f);
    zero.SetBlendSpace1D("travel-slow", "travel-fast", 0.0f);
    zero.Tick(1.0f / 60);
    // 权重 0 必须与单片段逐元素一致（不经过 ozz 归一化）。
    const auto& reference = pure.Models();
    const auto& blended = zero.Models();
    for (size_t joint = 0; joint < reference.size(); ++joint)
        EXPECT_TRUE(reference[joint].isApprox(blended[joint], 1e-6f));
    EXPECT_NEAR(pureHip - 3.0f, 40.0f * (pure.Time() / 4.0f), 0.05f);

    SkeletalPlayback full(asset);
    full.Play("travel-slow", true, 1, true);
    full.Tick(1.0f);
    full.SetBlendSpace1D("travel-slow", "travel-fast", 1.0f);
    full.Tick(1.0f / 60);
    // 权重 1 等价于单独播放 B 到同一相位（B 时长 2 s，相位取自 A）。
    const float blendSeconds = (full.Time() / 4.0f) * 2.0f;
    SkeletalPlayback direct(asset);
    direct.Play("travel-fast", true, 1, true);
    direct.Tick(blendSeconds);
    const auto& fullModels = full.Models();
    const auto& directModels = direct.Models();
    for (size_t joint = 0; joint < fullModels.size(); ++joint)
        EXPECT_TRUE(fullModels[joint].isApprox(directModels[joint], 1e-5f));
}

TEST(SkeletalBlendSpace, ClampsWeightRejectsMismatchAndClearsOnTransition)
{
    auto asset = MakeBlendAsset();
    SkeletalPlayback player(asset);
    player.Play("travel-slow", true, 1, true);
    player.SetBlendSpace1D("travel-slow", "travel-fast", 5.0f);
    EXPECT_FLOAT_EQ(player.BlendSpaceWeight(), 1.0f);
    player.SetBlendSpace1D("travel-slow", "travel-fast", -3.0f);
    EXPECT_FLOAT_EQ(player.BlendSpaceWeight(), 0.0f);
    EXPECT_ANY_THROW(player.SetBlendSpace1D("travel-fast", "travel-slow", 0.5f));
    EXPECT_ANY_THROW(player.SetBlendSpace1D("travel-slow", "missing", 0.5f));
    player.TransitionTo("travel-fast", 0, true, 0.2f);
    EXPECT_FALSE(player.BlendSpaceActive());
    EXPECT_EQ(player.BlendSpaceClipId(), "");
}

// LocalSpaceBase additive 参照：hip 的局部增量为 (+5,0,0)、绕 Y 90°、缩放 2。
Clip MakeAdditiveClip(const std::string& id, float hipX, float yawDegrees, float scale)
{
    Clip clip;
    clip.id = id;
    clip.raw.duration = 2;
    clip.raw.tracks.resize(3);
    const float radians = yawDegrees * 3.14159265358979323846f / 180;
    auto& root = clip.raw.tracks[0];
    root.translations = {{0, {3, 2, 1}}};
    root.rotations = {{0, ozz::math::Quaternion::identity()}};
    root.scales = {{0, ozz::math::Float3::one()}};
    auto& hip = clip.raw.tracks[1];
    hip.translations = {{0, {hipX, 10, 0}}};
    hip.rotations = {{0, {0, std::sin(radians / 2), 0, std::cos(radians / 2)}}};
    hip.scales = {{0, {scale, scale, scale}}};
    auto& foot = clip.raw.tracks[2];
    foot.translations = {{0, {0, 0, 4}}};
    foot.rotations = {{0, ozz::math::Quaternion::identity()}};
    foot.scales = {{0, ozz::math::Float3::one()}};
    ozz::animation::offline::AnimationBuilder builder;
    clip.runtime = builder(clip.raw);
    clip.sequence.additiveAnimType = "LocalSpaceBase";
    return clip;
}

std::shared_ptr<SkeletalAsset> MakeAdditiveAsset()
{
    auto asset = MakeAsset();
    asset->clips.push_back(MakeAdditiveClip("add", 5, 90, 2));
    return asset;
}

TEST(SkeletalAdditive, LocalSpaceDeltaComposesOntoBasePose)
{
    auto asset = MakeAdditiveAsset();
    SkeletalPlayback base(asset);
    base.Play("a", true, 1, true);
    base.Tick(0.5f);

    SkeletalPlayback player(asset);
    player.Play("a", true, 1, true);
    player.Tick(0.5f);
    player.SetAdditive("add", 1.0f, false);
    EXPECT_TRUE(player.AdditiveActive());
    EXPECT_EQ(player.AdditiveClipId(), "add");
    player.Tick(1.0f / 60);
    // hip 全局 = root(3,2,1) + hip 局部 (5,10,0)；旋转 90°、缩放 2。
    const Eigen::Vector3f hip = player.Models()[1].block<3, 1>(0, 3);
    EXPECT_NEAR(hip.x(), 8.0f, 1e-3f);
    EXPECT_NEAR(hip.y(), 12.0f, 1e-3f);
    EXPECT_NEAR(hip.z(), 1.0f, 1e-3f);
    const Eigen::Matrix3f rotation = player.Models()[1].block<3, 3>(0, 0);
    // 模型矩阵列含缩放：长度=2，方向是绕 Y 90° 的 X 轴。
    const Eigen::Vector3f axis = rotation.col(0).normalized();
    EXPECT_NEAR(axis.y(), 0.0f, 1e-3f);
    EXPECT_NEAR(std::abs(axis.z()), 1.0f, 1e-3f);
    EXPECT_NEAR(rotation.col(0).norm(), 2.0f, 1e-3f);

    // alpha=0 与 base 逐元素一致；alpha=0.5 的增量是平移/角度/缩放的一半。
    SkeletalPlayback zero(asset);
    zero.Play("a", true, 1, true);
    zero.Tick(0.5f);
    zero.SetAdditive("add", 0.0f, false);
    zero.Tick(1.0f / 60);
    for (size_t joint = 0; joint < base.Models().size(); ++joint)
        EXPECT_TRUE(base.Models()[joint].isApprox(zero.Models()[joint], 1e-5f));

    SkeletalPlayback half(asset);
    half.Play("a", true, 1, true);
    half.Tick(0.5f);
    half.SetAdditive("add", 0.5f, false);
    half.Tick(1.0f / 60);
    EXPECT_NEAR(half.Models()[1](0, 3), 5.5f, 1e-3f);
    EXPECT_NEAR((half.Models()[1].block<3, 3>(0, 0).col(0).norm()), 1.5f, 1e-3f);
}

TEST(SkeletalAdditive, RejectsNonLocalSpaceAndClearsWithPlay)
{
    auto asset = MakeAdditiveAsset();
    SkeletalPlayback player(asset);
    player.Play("a", true, 1, true);
    player.Tick(0.0f);
    EXPECT_ANY_THROW(player.SetAdditive("b", 1.0f, false));
    EXPECT_ANY_THROW(player.SetAdditive("missing", 1.0f, false));
    player.SetAdditive("add", 1.0f, true);
    player.Play("b", true, 1, true);
    // additive 是节点级修饰，Play 不隐式清除；显式 Clear 才关闭。
    EXPECT_TRUE(player.AdditiveActive());
    player.ClearAdditive();
    EXPECT_FALSE(player.AdditiveActive());
}

TEST(SkeletalAdditiveMeshSpace, RotatesInMeshSpaceAndIgnoresTranslationScale)
{
    auto asset = MakeAsset();
    asset->clips.reserve(asset->clips.size() + 2);
    asset->clips.push_back(MakeClip("base90", 0, 90, 1));       // hip local yaw 90
    asset->clips.push_back(MakeClip("meshAdd", 5, 90, 2));      // hip yaw 90 + 平移/缩放
    asset->clips.back().sequence.additiveAnimType = "RotationOffsetMeshSpace";

    SkeletalPlayback base(asset);
    base.Play("base90", true, 1, true);
    base.Tick(0.5f);

    SkeletalPlayback player(asset);
    player.Play("base90", true, 1, true);
    player.Tick(0.5f);
    player.SetAdditive("meshAdd", 1.0f, false);
    player.Tick(1.0f / 60);

    // mesh 空间增量只改旋转：hip 的平移/缩放仍取 base。
    const Eigen::Vector3f baseHip = base.Models()[1].block<3, 1>(0, 3);
    const Eigen::Vector3f playerHip = player.Models()[1].block<3, 1>(0, 3);
    EXPECT_TRUE(baseHip.isApprox(playerHip, 1e-4f));
    const float scale = player.Models()[1].block<3, 3>(0, 0).col(0).norm();
    EXPECT_NEAR(scale, 1.0f, 1e-3f);
    // mesh 空间里 hip 90 + 增量 90 = 180（绕 Y），再转回局部仍是 180。
    const Eigen::Matrix3f hipRotation = player.Models()[1].block<3, 3>(0, 0);
    Eigen::Quaternionf orientation(hipRotation);
    orientation.normalize();
    const Eigen::Quaternionf expected(
        Eigen::AngleAxisf(3.14159265358979323846f, Eigen::Vector3f::UnitY()));
    EXPECT_LT(orientation.angularDistance(expected), 0.01f);
}

TEST(SkeletalAdditiveCurves, AccumulatesBaseAndWeightedAdditiveCurves)
{
    auto asset = MakeAsset();
    asset->clips.reserve(asset->clips.size() + 1);
    asset->clips[0].sequence.curves.push_back({"Bend", {0}, {1.0f}});
    Clip additive = MakeClip("add", 5, 0, 1);
    additive.sequence.curves.push_back({"Bend", {0}, {0.5f}});
    additive.sequence.additiveAnimType = "LocalSpaceBase";
    asset->clips.push_back(std::move(additive));

    SkeletalPlayback player(asset);
    player.Play("a", true, 1, true);
    player.Tick(0.1f);
    EXPECT_NEAR(player.FrameCurves().at("Bend"), 1.0f, 1e-5f);

    player.SetAdditive("add", 0.5f, false);
    player.Tick(1.0f / 60);
    EXPECT_NEAR(player.FrameCurves().at("Bend"), 1.25f, 1e-5f);

    player.SetAdditive("add", 0.0f, false);
    player.Tick(1.0f / 60);
    EXPECT_NEAR(player.FrameCurves().at("Bend"), 1.0f, 1e-5f);
}

TEST(SkeletalCachedPose, CaptureThenUseHoldsPoseWithoutClockAdvance)
{
    auto asset = MakeAdditiveAsset();
    SkeletalPlayback player(asset);
    player.Play("a", true, 1, true);
    player.Tick(0.5f);
    const auto capturedModels = player.Models();
    player.CaptureCachedPose("Locomotion");
    // 切到另一个片段后仍能用同一实例的缓存快照。
    player.Play("b", true, 1, true);
    player.Tick(0.5f);
    player.UseCachedPose("Locomotion");
    EXPECT_TRUE(player.CachedPoseActive());
    EXPECT_EQ(player.CachedPoseName(), "Locomotion");
    const float timeBefore = player.Time();
    player.Tick(0.25f);
    // 缓存姿态不推进片段时钟、不产生根位移。
    EXPECT_FLOAT_EQ(player.Time(), timeBefore);
    EXPECT_NEAR(player.RootMotionDelta().norm(), 0.0f, 1e-6f);
    for (size_t joint = 0; joint < capturedModels.size(); ++joint)
        EXPECT_TRUE(capturedModels[joint].isApprox(player.Models()[joint], 1e-4f));

    // 缓存姿态可以再叠加 additive。
    player.SetAdditive("add", 1.0f, false);
    player.Tick(1.0f / 60);
    EXPECT_NEAR(player.Models()[1](0, 3), 8.0f, 1e-3f);

    player.ClearCachedPose();
    EXPECT_FALSE(player.CachedPoseActive());
    EXPECT_ANY_THROW(player.UseCachedPose("missing"));
}

TEST(SkeletalCachedPose, CaptureDuringCachedPoseDoesNotAccumulateAdditive)
{
    auto asset = MakeAdditiveAsset();
    SkeletalPlayback player(asset);
    player.Play("a", true, 1, true);
    player.Tick(0.5f);
    player.CaptureCachedPose("Locomotion");
    player.UseCachedPose("Locomotion");
    player.SetAdditive("add", 1.0f, false);
    player.Tick(1.0f / 60);
    const auto expected = player.Models();
    // 宿主每帧都会无条件发送根 SaveCachedPose；缓存态下必须忽略，
    // 否则 display(缓存+additive) 会作为下一帧 Base 再次叠加（缩放逐帧翻倍）。
    for (int frame = 0; frame < 30; ++frame)
    {
        player.Tick(1.0f / 60);
        player.CaptureCachedPose("Locomotion");
        for (size_t joint = 0; joint < expected.size(); ++joint)
            EXPECT_TRUE(expected[joint].isApprox(player.Models()[joint], 1e-4f))
                << "frame " << frame << " joint " << joint;
    }
    const float scale = player.Models()[1].block<3, 3>(0, 0).col(0).norm();
    EXPECT_NEAR(scale, 2.0f, 1e-3f);
}

TEST(SkeletalCachedPose, BranchIsEvaluatedInTheSameFrame)
{
    auto asset = MakeAsset();
    SkeletalPlayback player(asset);
    player.Play("a", true, 1, true);
    player.Tick(0.5f);
    player.SetCachedPoseBranches({
        {"Branch", {{"b", 0.0f}}, 1.0f, true}
    });
    player.UseCachedPose("Branch");
    player.Tick(0.25f);
    // 分支与主状态同一帧求值：主状态读到的就是 b 在 0.25 s 的姿态。
    SkeletalPlayback direct(asset);
    direct.Play("b", true, 1, true);
    direct.Tick(0.25f);
    for (size_t joint = 0; joint < direct.Models().size(); ++joint)
        EXPECT_TRUE(direct.Models()[joint].isApprox(player.Models()[joint], 1e-4f));
    // 分支不推进主片段时钟。
    EXPECT_FLOAT_EQ(player.Time(), 0.5f);
}

TEST(SkeletalCachedPose, BlendBranchUsesAxisAndWeight)
{
    auto asset = MakeAsset();
    SkeletalPlayback player(asset);
    player.Play("c", true, 1, true);
    player.SetCachedPoseBranches({
        {"Loco", {{"a", 0.0f}, {"b", 100.0f}}, 1.0f, true}
    });
    player.SetCachedPoseBranchAxis("Loco", 50.0f);
    player.UseCachedPose("Loco");
    player.Tick(0.0f);
    // a 的 hip x=3、b 的 hip x=11；轴 50 取中点。
    EXPECT_NEAR(player.Models()[1](0, 3), 7.0f, 1e-2f);
    const float scale = player.Models()[1].block<3, 3>(0, 0).col(0).norm();
    EXPECT_NEAR(scale, 1.5f, 1e-2f);
}

TEST(SkeletalCachedPose, BranchRejectsInvalidAndFallsBackToSnapshot)
{
    auto asset = MakeAsset();
    SkeletalPlayback player(asset);
    player.Play("a", true, 1, true);
    player.Tick(0.5f);
    player.CaptureCachedPose("Snap");
    EXPECT_ANY_THROW(player.SetCachedPoseBranches({
        {"x", {{"missing", 0.0f}}, 1.0f, true}}));
    EXPECT_ANY_THROW(player.SetCachedPoseBranches({
        {"x", {{"a", 100.0f}, {"b", 50.0f}}, 1.0f, true}}));
    EXPECT_ANY_THROW(player.SetCachedPoseBranches({
        {"x", {{"a", 0.0f}}, 1.0f, true},
        {"x", {{"b", 0.0f}}, 1.0f, true}}));
    // 清空分支后仍能回退到上一帧快照。
    player.SetCachedPoseBranches({
        {"Loco", {{"b", 0.0f}}, 1.0f, true}});
    player.ClearCachedPoseBranches();
    player.UseCachedPose("Snap");
    player.Tick(1.0f / 60);
    EXPECT_TRUE(player.CachedPoseActive());
}

// 只有 foot 轨道不同：用于验证 mask 分层只改命中关节。
Clip MakeFootClip(const std::string& id, float footZ)
{
    Clip clip = MakeClip(id, 0, 0, 1);
    clip.raw.tracks[2].translations = {{0, {0, 0, footZ}}};
    ozz::animation::offline::AnimationBuilder builder;
    clip.runtime = builder(clip.raw);
    return clip;
}

TEST(SkeletalLayered, MaskedJointBlendsAndOthersStay)
{
    auto asset = MakeAdditiveAsset();
    asset->clips.push_back(MakeFootClip("foot9", 9));
    SkeletalPlayback base(asset);
    base.Play("a", true, 1, true);
    base.Tick(0.5f);

    SkeletalPlayback player(asset);
    player.Play("a", true, 1, true);
    player.Tick(0.5f);
    player.SetLayeredPose("foot9", 1.0f, false, {"foot"});
    EXPECT_TRUE(player.LayeredActive());
    EXPECT_EQ(player.LayeredClipId(), "foot9");
    EXPECT_EQ(player.LayeredMaskCount(), 1u);
    player.Tick(1.0f / 60);
    // hip / root 不在 mask 内：模型矩阵保持 base。
    EXPECT_TRUE(base.Models()[0].isApprox(player.Models()[0], 1e-5f));
    EXPECT_TRUE(base.Models()[1].isApprox(player.Models()[1], 1e-5f));
    // foot 局部 z 从 4 变为 9，模型 z 增加 5。
    EXPECT_NEAR(player.Models()[2](2, 3), base.Models()[2](2, 3) + 5.0f, 1e-3f);
}

TEST(SkeletalLayered, AlphaEmptyMaskAndRejections)
{
    auto asset = MakeAdditiveAsset();
    asset->clips.push_back(MakeFootClip("foot9", 9));
    SkeletalPlayback base(asset);
    base.Play("a", true, 1, true);
    base.Tick(0.5f);

    SkeletalPlayback half(asset);
    half.Play("a", true, 1, true);
    half.Tick(0.5f);
    half.SetLayeredPose("foot9", 0.5f, false, {"foot"});
    half.Tick(1.0f / 60);
    EXPECT_NEAR(half.Models()[2](2, 3), base.Models()[2](2, 3) + 2.5f, 1e-3f);

    SkeletalPlayback zero(asset);
    zero.Play("a", true, 1, true);
    zero.Tick(0.5f);
    zero.SetLayeredPose("foot9", 0.0f, false, {"foot"});
    zero.Tick(1.0f / 60);
    for (size_t joint = 0; joint < base.Models().size(); ++joint)
        EXPECT_TRUE(base.Models()[joint].isApprox(zero.Models()[joint], 1e-5f));

    // 空 mask 视为全身。
    SkeletalPlayback full(asset);
    full.Play("a", true, 1, true);
    full.Tick(0.5f);
    full.SetLayeredPose("foot9", 1.0f, false, {});
    full.Tick(1.0f / 60);
    EXPECT_EQ(full.LayeredMaskCount(), 3u);
    EXPECT_NEAR(full.Models()[2](2, 3), base.Models()[2](2, 3) + 5.0f, 1e-3f);

    SkeletalPlayback invalid(asset);
    invalid.Play("a", true, 1, true);
    EXPECT_ANY_THROW(invalid.SetLayeredPose("missing", 0.5f, false, {}));
    EXPECT_ANY_THROW(invalid.SetLayeredPose("foot9", 0.5f, false, {"missing"}));
    std::vector<std::string> tooMany(65, "foot");
    EXPECT_ANY_THROW(invalid.SetLayeredPose("foot9", 0.5f, false, tooMany));
}

TEST(SkeletalLayered, AppliesAfterAdditiveAndKeepsUntilCleared)
{
    auto asset = MakeAdditiveAsset();
    SkeletalPlayback player(asset);
    player.Play("a", true, 1, true);
    player.Tick(0.5f);
    player.SetAdditive("add", 1.0f, false);
    // b 的 hip 是 (8,10,0)/90°/scale 2，完全覆盖 additive 后的 hip。
    player.SetLayeredPose("b", 1.0f, false, {"hip"});
    player.Tick(1.0f / 60);
    EXPECT_NEAR(player.Models()[1](0, 3), 11.0f, 1e-2f);
    const float scale = player.Models()[1].block<3, 3>(0, 0).col(0).norm();
    EXPECT_NEAR(scale, 2.0f, 1e-3f);

    // Play 不隐式清除 layered；显式 Clear 才关闭。
    player.Play("c", true, 1, true);
    EXPECT_TRUE(player.LayeredActive());
    player.ClearLayeredPose();
    EXPECT_FALSE(player.LayeredActive());
}

TEST(SkeletalSlotMontage, MaskedOverlayKeepsUnmaskedJointsOnBase)
{
    auto asset = MakeAsset();
    SkeletalPlayback base(asset);
    base.Play("a", true, 1, true);
    base.Tick(0.5f);

    SkeletalPlayback player(asset);
    player.Play("a", true, 1, true);
    player.Tick(0.5f);
    player.PlaySlotMontage("b", 1.0f, true, 1.0f, {"hip"});
    EXPECT_TRUE(player.SlotMontageActive());
    EXPECT_EQ(player.SlotMontageClipId(), "b");
    EXPECT_EQ(player.SlotMontageMaskCount(), 1u);
    base.Tick(1.0f / 60);
    player.Tick(1.0f / 60);
    // root 未命中 mask，保持 base；hip 命中 mask，取 overlay；foot 的局部姿态
    // 两段相同但父级换成 overlay 的 hip，因此模型矩阵跟随 overlay。
    EXPECT_TRUE(base.Models()[0].isApprox(player.Models()[0], 1e-5f));
    SkeletalPlayback overlay(asset);
    overlay.Play("b", true, 1, true);
    overlay.Tick(1.0f / 60);
    EXPECT_TRUE(overlay.Models()[1].isApprox(player.Models()[1], 1e-4f));
    EXPECT_TRUE(overlay.Models()[2].isApprox(player.Models()[2], 1e-4f));
    // slot montage 不推进基础片段时钟。
    EXPECT_FLOAT_EQ(player.Time(), 0.5f + 1.0f / 60);
}

TEST(SkeletalSlotMontage, AlphaScalesMaskedBlendAndCompletes)
{
    auto asset = MakeAsset();
    SkeletalPlayback half(asset);
    half.Play("a", true, 1, true);
    half.Tick(0.5f);
    half.PlaySlotMontage("b", 0.5f, false, 1.0f, {});
    half.Tick(1.0f / 60);
    SkeletalPlayback base(asset);
    base.Play("a", true, 1, true);
    base.Tick(0.5f);
    base.Tick(1.0f / 60);
    SkeletalPlayback overlay(asset);
    overlay.Play("b", true, 1, true);
    overlay.Tick(1.0f / 60);
    const Eigen::Vector3f midpoint =
        (base.Models()[1].block<3, 1>(0, 3) + overlay.Models()[1].block<3, 1>(0, 3)) * 0.5f;
    EXPECT_NEAR((half.Models()[1].block<3, 1>(0, 3) - midpoint).norm(), 0.0f, 1e-3f);

    SkeletalPlayback done(asset);
    done.Play("a", true, 1, true);
    done.PlaySlotMontage("b", 1.0f, false, 1.0f, {"hip"});
    done.Tick(5.0f);
    EXPECT_TRUE(done.SlotMontageCompleted());
    done.StopSlotMontage();
    EXPECT_FALSE(done.SlotMontageActive());
    done.Tick(1.0f / 60);
    SkeletalPlayback plain(asset);
    plain.Play("a", true, 1, true);
    plain.Tick(5.0f + 1.0f / 60);
    EXPECT_TRUE(plain.Models()[1].isApprox(done.Models()[1], 1e-4f));
}

TEST(SkeletalSlotMontage, RejectsUnknownBoneAndOversizedMask)
{
    auto asset = MakeAsset();
    SkeletalPlayback player(asset);
    player.Play("a", true, 1, true);
    EXPECT_ANY_THROW(player.PlaySlotMontage("b", 1.0f, true, 1.0f, {"missing"}));
    std::vector<std::string> tooMany(65, "hip");
    EXPECT_ANY_THROW(player.PlaySlotMontage("b", 1.0f, true, 1.0f, tooMany));
    EXPECT_ANY_THROW(player.PlaySlotMontage("missing", 1.0f, true, 1.0f, {}));
}

TEST(SkeletalSlotMontage, OverlayFiresItsNotifiesWhileBaseClockAdvances)
{
    auto asset = MakeAsset();
    asset->clips[1].sequence.notifies.push_back({"", "Attack", 0.4f, 0.0f});
    SkeletalPlayback player(asset);
    player.Play("a", true, 1, true);
    player.Tick(0.1f);
    const float baseTime = player.Time();
    player.PlaySlotMontage("b", 1.0f, false, 1.0f, {});
    bool fired = false;
    for (int frame = 0; frame < 45 && !fired; ++frame)
    {
        player.Tick(1.0f / 60);
        for (const auto& notify : player.FiredNotifies())
            if (notify.name == "Attack" && notify.phase == "instant") fired = true;
    }
    EXPECT_TRUE(fired);
    EXPECT_GT(player.Time(), baseTime);
    EXPECT_TRUE(player.SlotMontageActive());
}

TEST(SkeletalBlend, BlendsLocalTrsBeforeHierarchyWithQuaternionNormalization)
{
    SkeletalPlayback player(MakeAsset());
    player.Play("a", true, 1, true);
    player.TransitionTo("b", 0, true, 1);
    player.Tick(0.5f);
    EXPECT_FLOAT_EQ(player.BlendAlpha(), 0.5f);
    const auto& models = player.Models();
    // 手算：hip 平移 4，均匀缩放 1.5，yaw 45°；foot 的局部 4cm
    // 必须先经该旋转/缩放，再加父级平移，模型矩阵 lerp 无法得到此结果。
    const float diagonal = 3 * std::sqrt(2.0f);
    EXPECT_NEAR(models[1](0, 3), 7, 0.002f);
    EXPECT_NEAR(models[1](1, 3), 12, 0.002f);
    EXPECT_NEAR(models[2](0, 3), 7 + diagonal, 0.004f);
    EXPECT_NEAR(models[2](2, 3), 1 + diagonal, 0.004f);
    EXPECT_NEAR((models[1].block<3, 1>(0, 0).norm()), 1.5f, 0.001f);
    EXPECT_NEAR((models[1].block<3, 3>(0, 0).determinant()), 3.375f, 0.005f);
}

TEST(SkeletalBlend, TransitionStartsAtExactDisplayedPoseAndRetainsOldSnapshot)
{
    SkeletalPlayback player(MakeAsset());
    player.Play("a", true, 1, true);
    player.Tick(0.3f);
    const auto before = player.Pose(0);
    const auto models = player.Models();
    player.TransitionTo("b", 1, true, 0.5f);
    EXPECT_EQ(player.Pose(0), before);
    EXPECT_FLOAT_EQ(player.Time(), 1);
    EXPECT_FLOAT_EQ(player.BlendAlpha(), 0);
    player.Tick(0.25f);
    EXPECT_NE(player.Pose(0), before);
    EXPECT_FLOAT_EQ(player.Time(), 1.25f);
    EXPECT_EQ(player.Pose(0)->discontinuity, before->discontinuity);
    for (size_t joint = 0; joint < models.size(); ++joint)
    {
        EXPECT_TRUE(before->matrices[joint].isApprox(
            models[joint] * player.Asset().mesh.sections[0].inverseBinds[joint], 0.00001f));
        EXPECT_TRUE(player.Pose(0)->matrices[joint].isApprox(
            player.Models()[joint] * player.Asset().mesh.sections[0].inverseBinds[joint], 0.00001f));
    }
}

TEST(SkeletalBlend, InterruptedBlendFreezesCurrentDisplayedPoseAsOnlySource)
{
    SkeletalPlayback player(MakeAsset());
    player.Play("a", true, 1, true);
    player.TransitionTo("b", 0, true, 1);
    player.Tick(0.5f);
    const auto before = player.Pose(0);
    const auto epoch = before->discontinuity;
    player.TransitionTo("c", 0, true, 1);
    EXPECT_EQ(player.Pose(0), before);
    player.Tick(0.25f);
    // 新源是 hip.x=4 的显示姿态，目标 -8；25% 后为 1，加绑定根 x=3。
    EXPECT_NEAR(player.Models()[1](0, 3), 4, 0.003f);
    EXPECT_EQ(player.Pose(0)->discontinuity, epoch);
    for (int interrupt = 0; interrupt < 32; ++interrupt)
    {
        const auto frozen = player.Pose(0);
        player.TransitionTo(interrupt % 2 ? "b" : "c", 0, true, 1);
        EXPECT_EQ(player.Pose(0), frozen);
        player.Tick(0.01f);
        EXPECT_EQ(player.Pose(0)->discontinuity, epoch);
    }
}

// 一次性片段播完那一帧 playing 已经是 false；惯性化不能把“播放器停住”误当成“姿态静止”。
// 误测成零速度时整个过渡从零速度起步，只在切换瞬间表现为一次顿挫。
TEST(SkeletalBlend, InertializationFromCompletedOneShotKeepsExitVelocity)
{
    SkeletalPlayback player(MakeAsset());
    player.Play("travel", false, 1, true);
    // Play 会把片段级 Inertialization 复位成 false；过渡请求是在 TransitionTo 之前设的。
    player.SetInertializationEnabled(true);
    player.Tick(4.0f);
    ASSERT_FLOAT_EQ(player.Time(), 4.0f);
    const float before = player.Models()[1](0, 3);
    // 源片段末段 hip.x 速度 100 cm/s；目标片段 hip.x 恒为 8 且与末帧重合（位移偏移 0），
    // 所以首帧位移只可能来自源片段速度。
    player.TransitionTo("still", 0, true, 0.5f);
    player.Tick(0.05f);
    const float after = player.Models()[1](0, 3);
    // 正确速度下 50 ms 约走 4.7 cm；速度被误测成 0 时位移恒为 0。
    EXPECT_GT(after - before, 3.0f);
}

TEST(SkeletalBlend, PauseAndZeroDeltaFreezeBothNativeTimeAndBlend)
{
    SkeletalPlayback player(MakeAsset());
    player.Play("a", true, 1, true);
    player.TransitionTo("b", 0.2f, true, 1);
    player.Tick(0.25f);
    const auto before = player.Pose(0);
    player.Tick(0);
    EXPECT_EQ(player.Pose(0), before);
    player.SetPlaying(false);
    player.Tick(2);
    EXPECT_EQ(player.Pose(0), before);
    EXPECT_FLOAT_EQ(player.Time(), 0.45f);
    EXPECT_FLOAT_EQ(player.BlendAlpha(), 0.25f);
    player.SetPlaying(true);
    player.Tick(0.25f);
    EXPECT_FLOAT_EQ(player.Time(), 0.7f);
    EXPECT_FLOAT_EQ(player.BlendAlpha(), 0.5f);
}

TEST(SkeletalBlend, RateChangePreservesPoseAndScalesOnlyTargetClock)
{
    SkeletalPlayback player(MakeAsset());
    player.Play("a", true, 1, true);
    player.TransitionTo("b", 0.2f, true, 1);
    player.Tick(0.25f);
    const auto before = player.Pose(0);
    player.SetRate(2);
    EXPECT_EQ(player.Pose(0), before);
    EXPECT_FLOAT_EQ(player.Rate(), 2);
    EXPECT_FLOAT_EQ(player.Time(), 0.45f);
    EXPECT_FLOAT_EQ(player.BlendAlpha(), 0.25f);
    EXPECT_TRUE(player.IsPlaying());
    EXPECT_EQ(player.SelectedClip(), 1u);
    player.Tick(0.25f);
    EXPECT_FLOAT_EQ(player.Time(), 0.95f);
    EXPECT_FLOAT_EQ(player.BlendAlpha(), 0.5f);
    EXPECT_EQ(player.Pose(0)->discontinuity, before->discontinuity);
    // 50% 混合仍从原始 hip.x=0 出发，不能把改速时的显示姿态重冻为新源。
    EXPECT_NEAR(player.Models()[1](0, 3), 7, 0.003f);
    const auto advanced = player.Pose(0);
    EXPECT_THROW(player.SetRate(0), std::invalid_argument);
    EXPECT_THROW(player.SetRate(4.01f), std::invalid_argument);
    EXPECT_THROW(player.SetRate(std::numeric_limits<float>::quiet_NaN()), std::invalid_argument);
    EXPECT_THROW(player.SetRate(std::numeric_limits<float>::infinity()), std::invalid_argument);
    EXPECT_EQ(player.Pose(0), advanced);
    EXPECT_FLOAT_EQ(player.Rate(), 2);
    EXPECT_FLOAT_EQ(player.Time(), 0.95f);
    EXPECT_FLOAT_EQ(player.BlendAlpha(), 0.5f);
}

TEST(SkeletalBlend, OneShotCompletesOnlyAfterTargetEndpointAndBlendFinish)
{
    SkeletalPlayback player(MakeAsset());
    player.Play("a", true, 1, true);
    player.TransitionTo("b", 3.9f, false, 1);
    player.Tick(0.2f);
    EXPECT_FLOAT_EQ(player.Time(), 4);
    EXPECT_FALSE(player.Completed());
    EXPECT_TRUE(player.IsPlaying());
    player.Tick(0.8f);
    EXPECT_TRUE(player.Completed());
    EXPECT_FALSE(player.IsBlending());
    EXPECT_FALSE(player.IsPlaying());
    const auto end = player.Pose(0);
    player.Tick(1);
    EXPECT_EQ(player.Pose(0), end);
}

TEST(SkeletalBlend, BlendFinishMatchesExactTargetPose)
{
    const auto asset = MakeAsset();
    SkeletalPlayback player(asset);
    SkeletalPlayback target(asset);
    player.Play("a", true, 1, true);
    target.Play("b", true, 1, true);
    player.TransitionTo("b", 0.5f, true, 0.5f);
    player.Tick(0.5f);
    target.Seek(1);
    EXPECT_FALSE(player.IsBlending());
    EXPECT_FLOAT_EQ(player.BlendAlpha(), 1);
    for (size_t joint = 0; joint < target.Models().size(); ++joint)
        EXPECT_TRUE(player.Models()[joint].isApprox(target.Models()[joint], 0.00001f));
}

TEST(SkeletalBlend, InPlaceRemovesCompleteRootTrsBeforeEveryBlendedSample)
{
    SkeletalPlayback player(MakeAsset());
    player.Play("a", true, 1, true);
    player.TransitionTo("root-trs", 2, true, 1);
    for (int step = 0; step < 8; ++step)
    {
        player.Tick(0.25f);
        EXPECT_TRUE(player.Models()[0].isApprox(player.Asset().mesh.joints[0].restModel, 0.0001f));
        EXPECT_NEAR(player.Models()[2](2, 3), 5, 0.003f);
        EXPECT_TRUE(player.Pose(0)->matrices[0].isIdentity(0.0001f));
    }
}

TEST(SkeletalBlend, InPlaceLoopPreservesPoseEpochDuringAndAfterBlend)
{
    SkeletalPlayback player(MakeAsset());
    player.Play("a", true, 1, true);
    const auto epoch = player.Pose(0)->discontinuity;
    player.TransitionTo("b", 3.9f, true, 0.5f);
    player.Tick(0.2f);
    EXPECT_NEAR(player.Time(), 0.1f, 0.00001f);
    EXPECT_EQ(player.Pose(0)->discontinuity, epoch);
    player.Tick(4);
    EXPECT_EQ(player.Pose(0)->discontinuity, epoch);
    EXPECT_FALSE(player.IsBlending());
}

TEST(SkeletalBlend, HardCutSeekAndPlayCancelBlendAndMarkDiscontinuity)
{
    SkeletalPlayback player(MakeAsset());
    player.Play("a", true, 2, true);
    player.TransitionTo("b", 0.5f, true, 1);
    EXPECT_FLOAT_EQ(player.Rate(), 1);
    const auto epoch = player.Pose(0)->discontinuity;
    player.Seek(1);
    EXPECT_FALSE(player.IsBlending());
    EXPECT_GT(player.Pose(0)->discontinuity, epoch);
    player.TransitionTo("c", 0, true, 1);
    player.Play("a", false, 2, true);
    EXPECT_FALSE(player.IsBlending());
    EXPECT_FLOAT_EQ(player.Time(), 0);
    EXPECT_FLOAT_EQ(player.Rate(), 2);
    const auto before = player.Pose(0)->discontinuity;
    player.TransitionTo("b", 1, true, 0);
    EXPECT_FALSE(player.IsBlending());
    EXPECT_GT(player.Pose(0)->discontinuity, before);
    EXPECT_NEAR(player.Models()[1](0, 3), 11, 0.002f);
}

TEST(SkeletalBlend, InvalidTransitionRetainsActiveClockPoseAndBlend)
{
    SkeletalPlayback player(MakeAsset());
    player.Play("a", true, 1, true);
    player.TransitionTo("b", 0, true, 1);
    player.Tick(0.25f);
    const auto before = player.Pose(0);
    EXPECT_THROW(player.TransitionTo("missing", 0, false, 1), std::invalid_argument);
    EXPECT_THROW(player.TransitionTo("c", -1, false, 1), std::invalid_argument);
    EXPECT_THROW(player.TransitionTo("c", 5, false, 1), std::invalid_argument);
    EXPECT_THROW(player.TransitionTo("c", 0, false, -1), std::invalid_argument);
    EXPECT_THROW(player.TransitionTo("c", 0, false, std::numeric_limits<float>::infinity()),
        std::invalid_argument);
    EXPECT_EQ(player.Pose(0), before);
    EXPECT_EQ(player.SelectedClip(), 1u);
    EXPECT_FLOAT_EQ(player.Time(), 0.25f);
    EXPECT_FLOAT_EQ(player.BlendAlpha(), 0.25f);
    EXPECT_TRUE(player.IsLooping());
}

TEST(SkeletalBlend, RootPolicyChangeRequiresExplicitHardCut)
{
    SkeletalPlayback player(MakeAsset());
    player.Tick(0.5f);
    const auto before = player.Pose(0);
    EXPECT_THROW(player.TransitionTo("root-trs", 1, true, 0.5f, true), std::invalid_argument);
    EXPECT_EQ(player.Pose(0), before);
    EXPECT_FALSE(player.InPlace());
    player.TransitionTo("root-trs", 1, true, 0, true);
    EXPECT_TRUE(player.InPlace());
    EXPECT_TRUE(player.Models()[0].isApprox(player.Asset().mesh.joints[0].restModel, 0.0001f));
    EXPECT_GT(player.Pose(0)->discontinuity, before->discontinuity);
}

std::shared_ptr<SkeletalAsset> MakeLegAsset()
{
    auto asset = std::make_shared<SkeletalAsset>();
    ozz::animation::offline::RawSkeleton raw;
    raw.roots.resize(1);
    auto& root = raw.roots[0];
    root.name = "root";
    root.transform = ozz::math::Transform::identity();
    root.children.resize(1);
    auto& pelvis = root.children[0];
    pelvis.name = "pelvis";
    pelvis.transform = ozz::math::Transform::identity();
    pelvis.transform.translation = {0, 80, 0};
    pelvis.children.resize(2);
    for (size_t side = 0; side < 2; ++side)
    {
        const std::string suffix = side == 0 ? "_l" : "_r";
        auto& thigh = pelvis.children[side];
        thigh.name = ("thigh" + suffix).c_str();
        thigh.transform = ozz::math::Transform::identity();
        thigh.transform.translation = {side == 0 ? -10.0f : 10.0f, 0, 0};
        thigh.children.resize(1);
        auto& calf = thigh.children[0];
        calf.name = ("calf" + suffix).c_str();
        calf.transform = ozz::math::Transform::identity();
        calf.transform.translation = {0, -35, 10};
        calf.children.resize(1);
        auto& foot = calf.children[0];
        foot.name = ("foot" + suffix).c_str();
        foot.transform = ozz::math::Transform::identity();
        foot.transform.translation = {0, -35, -10};
        foot.children.resize(1);
        foot.children[0].name = ("ball" + suffix).c_str();
        foot.children[0].transform = ozz::math::Transform::identity();
        foot.children[0].transform.translation = {0, -5, 15};
    }
    asset->mesh.runtime = ozz::animation::offline::SkeletonBuilder()(raw);
    const std::array<int, 10> parents{-1, 0, 1, 2, 3, 4, 1, 6, 7, 8};
    const std::array<Eigen::Vector3f, 10> offsets{Eigen::Vector3f(0, 0, 0), Eigen::Vector3f(0, 80, 0),
        Eigen::Vector3f(-10, 0, 0), Eigen::Vector3f(0, -35, 10), Eigen::Vector3f(0, -35, -10),
        Eigen::Vector3f(0, -5, 15), Eigen::Vector3f(10, 0, 0), Eigen::Vector3f(0, -35, 10),
        Eigen::Vector3f(0, -35, -10), Eigen::Vector3f(0, -5, 15)};
    Clip clip;
    clip.id = "legs";
    clip.raw.duration = 2;
    clip.raw.tracks.resize(parents.size());
    SkinSection section;
    for (size_t joint = 0; joint < parents.size(); ++joint)
    {
        const auto& offset = offsets[joint];
        const auto local = Translation(offset.x(), offset.y(), offset.z());
        const Eigen::Matrix4f model = parents[joint] < 0 ? local :
            (asset->mesh.joints[parents[joint]].restModel * local).eval();
        asset->mesh.joints.push_back({asset->mesh.runtime->joint_names()[joint], parents[joint], local, model});
        clip.raw.tracks[joint].translations = {{0, {offset.x(), offset.y(), offset.z()}}};
        clip.raw.tracks[joint].rotations = {{0, ozz::math::Quaternion::identity()}};
        clip.raw.tracks[joint].scales = {{0, ozz::math::Float3::one()}};
        Vertex vertex{};
        vertex.position = model.block<3, 1>(0, 3);
        section.vertices.push_back(vertex);
        section.influenceOffsets.push_back(static_cast<uint32_t>(joint));
        section.influences.push_back({static_cast<uint32_t>(joint), 1});
        section.inverseBinds.push_back(model.inverse());
    }
    section.influenceOffsets.push_back(static_cast<uint32_t>(parents.size()));
    asset->mesh.sections.push_back(std::move(section));
    clip.runtime = ozz::animation::offline::AnimationBuilder()(clip.raw);
    asset->clips.push_back(std::move(clip));
    return asset;
}

class FootPlaneProbe : public GroundProbe
{
public:
    bool hit = true;
    Eigen::Vector3f normal = Eigen::Vector3f::UnitY();
    FootGroundHit ProbeGround(const std::string&, const Eigen::Vector3f& origin,
        const Eigen::Vector3f& displacement) const override
    {
        FootGroundHit result;
        const float fraction = -normal.dot(origin) / normal.dot(displacement);
        result.hit = hit && fraction >= 0 && fraction <= 1;
        result.point = origin + fraction * displacement;
        result.normal = normal;
        result.supportIdentity = "static-plane";
        return result;
    }
};

void TickFeet(SkeletalPlayback& player, const GroundProbe& plane, const Eigen::Matrix4f& component,
    bool grounded = true, float delta = 1.0f / 60)
{
    player.Tick(delta);
    player.ApplyFootPlacement(component, grounded, "owner", delta, plane);
}

TEST(SkeletalBlend, FootPlacementKeepsWorldBallReferenceAnchoredAndPreIkPoseIndependent)
{
    SkeletalPlayback player(MakeLegAsset());
    player.Play("legs", true, 1, true);
    player.SetFootPlacementEnabled(true);
    FootPlaneProbe plane;
    Eigen::Matrix4f component = Eigen::Matrix4f::Identity();
    TickFeet(player, plane, component);
    TickFeet(player, plane, component);
    ASSERT_TRUE(player.FootPlacement().active);
    const auto anchor = player.FootPlacement().feet[0].anchor;
    const auto frozen = player.Pose(0);
    const auto frozenMatrix = frozen->matrices[4];
    const auto rawFoot = player.PreIkModels()[4];
    for (int step = 1; step <= 12; ++step)
    {
        component(2, 3) = step * 0.2f;
        TickFeet(player, plane, component);
        ASSERT_TRUE(player.FootPlacement().feet[0].planted);
        EXPECT_TRUE(player.FootPlacement().feet[0].anchor.isApprox(anchor, 0.0001f));
        EXPECT_LT(player.FootPlacement().feet[0].targetError, 0.1f);
        EXPECT_TRUE(player.PreIkModels()[4].isApprox(rawFoot, 0.0001f));
        EXPECT_TRUE(frozen->matrices[4].isApprox(frozenMatrix, 0));
    }
    EXPECT_NEAR(player.Time(), 14.0f / 60, 0.00001f);
    EXPECT_GT((player.Models()[4] - player.PreIkModels()[4]).norm(), 0.5f);
}

TEST(SkeletalBlend, FootPlacementLockTypeSelectsBallOrAnkleAnchorAndResetsHistory)
{
    SkeletalPlayback player(MakeLegAsset());
    player.Play("legs", true, 1, true);
    player.SetFootPlacementEnabled(true);
    EXPECT_EQ(player.FootPlacementLockType(), FootLockType::Auto);
    FootPlaneProbe plane;
    const Eigen::Matrix4f component = Eigen::Matrix4f::Identity();
    TickFeet(player, plane, component);
    TickFeet(player, plane, component);
    ASSERT_TRUE(player.FootPlacement().active);
    EXPECT_EQ(player.FootPlacement().lockMode, "auto");
    EXPECT_EQ(player.FootPlacement().feet[0].contactMode, "ball");
    const auto ballAnchor = player.FootPlacement().feet[0].anchor;
    player.SetFootPlacementLockType(FootLockType::PivotAroundAnkle);
    EXPECT_EQ(player.FootPlacementLockType(), FootLockType::PivotAroundAnkle);
    EXPECT_EQ(player.FootPlacement().lockMode, "ankle");
    EXPECT_EQ(player.FootPlacement().reason, "lock-mode");
    TickFeet(player, plane, component);
    TickFeet(player, plane, component);
    ASSERT_TRUE(player.FootPlacement().feet[0].planted);
    EXPECT_EQ(player.FootPlacement().feet[0].contactMode, "heel");
    EXPECT_LT(player.FootPlacement().feet[0].targetError, 0.1f);
    EXPECT_GT((player.FootPlacement().feet[0].anchor - ballAnchor).norm(), 1.0f);
    player.SetFootPlacementLockType(FootLockType::PivotAroundBall);
    TickFeet(player, plane, component);
    TickFeet(player, plane, component);
    ASSERT_TRUE(player.FootPlacement().feet[0].planted);
    EXPECT_EQ(player.FootPlacement().feet[0].contactMode, "ball");
    EXPECT_LT(player.FootPlacement().feet[0].targetError, 0.1f);
}

TEST(SkeletalBlend, FootPlacementAlignsBoundReferenceUpToSlopeWithoutMovingActor)
{
    SkeletalPlayback player(MakeLegAsset());
    player.Play("legs", true, 1, true);
    player.SetFootPlacementEnabled(true);
    FootPlaneProbe plane;
    const float slope = 15 * 3.14159265358979323846f / 180;
    plane.normal = {std::sin(slope), std::cos(slope), 0};
    Eigen::Matrix4f component = Eigen::Matrix4f::Identity();
    for (int step = 0; step < 30; ++step) TickFeet(player, plane, component);
    ASSERT_TRUE(player.FootPlacement().active);
    for (size_t side = 0; side < 2; ++side)
    {
        const auto& state = player.FootPlacement().feet[side];
        EXPECT_TRUE(state.planted);
        EXPECT_LT(std::abs(plane.normal.dot(state.postSole)), 0.1f);
        EXPECT_LT(state.targetError, 0.1f);
        const size_t foot = side == 0 ? 4 : 8;
        const float normalAlignment = player.Models()[foot].block<3, 1>(0, 1).normalized().dot(plane.normal);
        EXPECT_GT(normalAlignment, 0.999f);
    }
    EXPECT_GE(player.FootPlacement().pelvisOffset, -15);
    EXPECT_LE(player.FootPlacement().pelvisOffset, 0);
    EXPECT_TRUE(component.isIdentity());
    std::array<Eigen::Vector3f, 2> anchors{player.FootPlacement().feet[0].anchor,
        player.FootPlacement().feet[1].anchor};
    bool replanted = false;
    float previousPelvis = player.FootPlacement().pelvisOffset;
    // 慢速沿坡漂移跨过 20cm 锁半径；重锚当帧也必须满足几何误差，不能只检查最终收敛。
    for (int step = 1; step <= 120; ++step)
    {
        component(0, 3) = step * 0.2f;
        TickFeet(player, plane, component);
        EXPECT_LE(std::abs(player.FootPlacement().pelvisOffset - previousPelvis), 1.0001f);
        previousPelvis = player.FootPlacement().pelvisOffset;
        for (size_t side = 0; side < 2; ++side)
        {
            const auto& state = player.FootPlacement().feet[side];
            EXPECT_TRUE(state.planted);
            EXPECT_LT(std::abs(plane.normal.dot(state.postSole)), 1);
            EXPECT_LT(state.targetError, 1);
            replanted = replanted || (state.anchor - anchors[side]).norm() > 5;
        }
    }
    EXPECT_TRUE(replanted);
}

class FootStepProbe : public GroundProbe
{
public:
    bool upperHeel = false;
    bool missingLower = false;
    FootGroundHit ProbeGround(const std::string&, const Eigen::Vector3f& origin,
        const Eigen::Vector3f& displacement) const override
    {
        const bool upper = (origin.z() < 7) == upperHeel;
        const float height = upper ? 5.0f : 0.0f;
        const float fraction = (height - origin.y()) / displacement.y();
        FootGroundHit hit;
        hit.hit = (!missingLower || upper) && fraction >= 0 && fraction <= 1;
        hit.point = origin + fraction * displacement;
        hit.normal = Eigen::Vector3f::UnitY();
        hit.supportIdentity = upper ? "upper-tread" : "lower-tread";
        return hit;
    }
};

TEST(SkeletalBlend, FootPlacementUsesAnExplicitStepEdgePivotWithoutPenetratingTheOtherReference)
{
    for (bool upperHeel : {false, true})
        for (bool missingLower : {false, true})
        {
            SCOPED_TRACE(upperHeel);
            SCOPED_TRACE(missingLower);
            SkeletalPlayback player(MakeLegAsset());
            player.Play("legs", true, 1, true);
            player.SetFootPlacementEnabled(true);
            // 显式模式：higher 踏面上的参考点决定 ball/ankle，不再由求解器自动切换。
            player.SetFootPlacementLockType(upperHeel ? FootLockType::PivotAroundAnkle :
                FootLockType::PivotAroundBall);
            FootStepProbe step;
            step.upperHeel = upperHeel;
            step.missingLower = missingLower;
            const Eigen::Matrix4f component = Eigen::Matrix4f::Identity();
            TickFeet(player, step, component);
            TickFeet(player, step, component);
            ASSERT_TRUE(player.FootPlacement().active);
            for (const auto& foot : player.FootPlacement().feet)
            {
                ASSERT_TRUE(foot.planted);
                EXPECT_EQ(foot.contactMode, upperHeel ? "heel" : "ball");
                EXPECT_LT(foot.targetError, 0.1f);
                const auto& pivot = upperHeel ? foot.postHeel : foot.postSole;
                const auto& other = upperHeel ? foot.postSole : foot.postHeel;
                EXPECT_NEAR(pivot.y(), 5, 0.1f);
                EXPECT_GE(other.y(), -0.1f);
                // 台阶边缘允许另一点悬空；不把两个水平踏面伪造成中间斜面。
                EXPECT_NEAR(pivot.y(), other.y(), 0.1f);
                EXPECT_NEAR(foot.groundNormal.y(), 1, 0.0001f);
                EXPECT_TRUE(foot.heelGrounded == (upperHeel || !missingLower));
                EXPECT_TRUE(foot.grounded == (!upperHeel || !missingLower));
            }
            EXPECT_TRUE(component.isIdentity());
        }
}

TEST(SkeletalBlend, FootPlacementKeepsSourceRollWhenBallSupportsAndHeelMovesRapidly)
{
    auto asset = MakeLegAsset();
    auto& clip = asset->clips[0];
    const float angle = 30 * 3.14159265358979323846f / 180;
    const Eigen::Quaternionf endRotation(Eigen::AngleAxisf(angle, Eigen::Vector3f::UnitX()));
    const Eigen::Vector3f ballLocal(0, -10, 15);
    const Eigen::Vector3f endFoot = Eigen::Vector3f(0, 0, 15) - endRotation * ballLocal;
    for (size_t foot : {4u, 8u})
    {
        auto& track = clip.raw.tracks[foot];
        // 平移补偿保持 ball 支点；heel 随源 foot 旋转抬起，而非整条腿快速平移。
        track.translations = {{0, {0, -35, -10}},
            {0.05f, {endFoot.x(), -35 + endFoot.y() - 10, -10 + endFoot.z()}},
            {2, {endFoot.x(), -35 + endFoot.y() - 10, -10 + endFoot.z()}}};
        const ozz::math::Quaternion rotation{endRotation.x(), endRotation.y(), endRotation.z(), endRotation.w()};
        track.rotations = {{0, ozz::math::Quaternion::identity()}, {0.05f, rotation}, {2, rotation}};
    }
    clip.runtime = ozz::animation::offline::AnimationBuilder()(clip.raw);
    SkeletalPlayback player(asset);
    player.Play("legs", true, 1, true);
    player.SetFootPlacementEnabled(true);
    FootPlaneProbe plane;
    const Eigen::Matrix4f component = Eigen::Matrix4f::Identity();
    TickFeet(player, plane, component);
    const auto previous = player.FootPlacement().feet[0];
    TickFeet(player, plane, component);
    const auto& foot = player.FootPlacement().feet[0];
    ASSERT_LT((foot.preSole - previous.preSole).norm() * 60, 40);
    ASSERT_GT((foot.preHeel - previous.preHeel).norm() * 60, 80);
    ASSERT_TRUE(foot.planted);
    EXPECT_EQ(foot.contactMode, "ball");
    EXPECT_LT(foot.targetError, 0.5f);
    const float sourceHeight = foot.preHeel.y() - foot.preSole.y();
    const float correctedHeight = foot.postHeel.y() - foot.postSole.y();
    ASSERT_GT(sourceHeight, 3);
    EXPECT_NEAR(correctedHeight, sourceHeight, 0.1f);
    const float rotationDifference =
        (player.Models()[4].block<3, 3>(0, 0) - player.PreIkModels()[4].block<3, 3>(0, 0)).norm();
    EXPECT_LT(rotationDifference, 0.001f);
    // 已锁定的 ball 下一帧仍稳定；不能借预览 pause 或直接写求解后矩阵伪造源滚动。
    const auto anchor = foot.anchor;
    TickFeet(player, plane, component);
    EXPECT_TRUE(player.FootPlacement().feet[0].anchor.isApprox(anchor, 0.0001f));
    EXPECT_LT(player.FootPlacement().feet[0].targetError, 0.5f);
}

TEST(SkeletalBlend, FootPlacementReleasesGroundedPelvisAtBoundedSpeedAfterBothFeetUnlock)
{
    SkeletalPlayback player(MakeLegAsset());
    player.Play("legs", true, 1, true);
    player.SetFootPlacementEnabled(true);
    FootPlaneProbe plane;
    Eigen::Matrix4f component = Translation(0, 5, 0);
    for (int frame = 0; frame < 20; ++frame) TickFeet(player, plane, component);
    ASSERT_LT(player.FootPlacement().pelvisOffset, -1);
    const auto frozen = player.Pose(0);
    const auto frozenFoot = frozen->matrices[4];
    const auto preIkFoot = player.PreIkModels()[4];
    float previousOffset = player.FootPlacement().pelvisOffset;
    for (int frame = 1; frame <= 20; ++frame)
    {
        component(2, 3) = frame * 3.0f;
        TickFeet(player, plane, component);
        const auto& state = player.FootPlacement();
        EXPECT_FALSE(state.feet[0].planted);
        EXPECT_FALSE(state.feet[1].planted);
        EXPECT_LE(std::abs(state.pelvisOffset - previousOffset), 1.0001f);
        EXPECT_GE(state.pelvisOffset, previousOffset);
        if (state.pelvisOffset != 0)
        {
            EXPECT_TRUE(state.active);
            EXPECT_EQ(state.reason, "releasing");
        }
        previousOffset = state.pelvisOffset;
        EXPECT_TRUE(player.PreIkModels()[4].isApprox(preIkFoot, 0.0001f));
        EXPECT_TRUE(frozen->matrices[4].isApprox(frozenFoot, 0));
    }
    EXPECT_FLOAT_EQ(player.FootPlacement().pelvisOffset, 0);
    EXPECT_FALSE(player.FootPlacement().active);
    for (size_t joint = 0; joint < player.Models().size(); ++joint)
        EXPECT_TRUE(player.Models()[joint].isApprox(player.PreIkModels()[joint], 0));
}

TEST(SkeletalBlend, FootPlacementReleasesOnSwingAirborneMissingGroundTeleportAndLargeDelta)
{
    SkeletalPlayback player(MakeLegAsset());
    player.Play("legs", true, 1, true);
    player.SetFootPlacementEnabled(true);
    FootPlaneProbe plane;
    Eigen::Matrix4f component = Eigen::Matrix4f::Identity();
    TickFeet(player, plane, component);
    TickFeet(player, plane, component);
    ASSERT_TRUE(player.FootPlacement().active);
    component(2, 3) = 3; // 180cm/s 的未求解足速必须释放，不能靠命中地面压住摆腿。
    TickFeet(player, plane, component);
    EXPECT_FALSE(player.FootPlacement().active);
    for (size_t joint = 0; joint < player.Models().size(); ++joint)
        EXPECT_TRUE(player.Models()[joint].isApprox(player.PreIkModels()[joint], 0));
    TickFeet(player, plane, component);
    EXPECT_TRUE(player.FootPlacement().active);
    TickFeet(player, plane, component, false);
    EXPECT_FALSE(player.FootPlacement().active);
    EXPECT_EQ(player.FootPlacement().reason, "airborne");
    TickFeet(player, plane, component);
    TickFeet(player, plane, component);
    plane.hit = false;
    TickFeet(player, plane, component);
    EXPECT_FALSE(player.FootPlacement().active);
    EXPECT_EQ(player.FootPlacement().reason, "no-ground");
    plane.hit = true;
    TickFeet(player, plane, component);
    TickFeet(player, plane, component);
    component(0, 3) = 200;
    TickFeet(player, plane, component);
    EXPECT_EQ(player.FootPlacement().reason, "teleported");
    EXPECT_FALSE(player.FootPlacement().active);
    TickFeet(player, plane, component, true, 0.2f);
    EXPECT_EQ(player.FootPlacement().reason, "large-delta");
    player.SetFootPlacementEnabled(false);
    EXPECT_FALSE(player.FootPlacement().enabled);
    // 一次性片段到端点后不再产生新 pre-IK 指针，但正 delta 的物理退出仍须清锁。
    player.Play("legs", false, 1, true);
    player.SetFootPlacementEnabled(true);
    player.Seek(player.Duration());
    TickFeet(player, plane, component);
    TickFeet(player, plane, component);
    ASSERT_TRUE(player.Completed());
    ASSERT_TRUE(player.FootPlacement().active);
    const auto endpoint = player.Pose(0);
    const auto endpointAnchor = player.FootPlacement().feet[0].anchor;
    player.SetPlaying(false);
    player.ApplyFootPlacement(component, false, "owner", 1.0f / 60, plane);
    EXPECT_EQ(player.Pose(0), endpoint);
    EXPECT_TRUE(player.FootPlacement().active);
    EXPECT_TRUE(player.FootPlacement().feet[0].anchor.isApprox(endpointAnchor, 0));
    player.SetPlaying(true);
    player.ApplyFootPlacement(component, false, "owner", 1.0f / 60, plane);
    EXPECT_FALSE(player.FootPlacement().active);
    EXPECT_EQ(player.FootPlacement().reason, "airborne");
    EXPECT_NE(player.Pose(0), endpoint);
}

TEST(SkeletalBlend, FootPlacementFreezesWithPauseAndRejectsRigOrScaleWithoutPoisoningBlendSource)
{
    SkeletalPlayback invalid(MakeAsset());
    EXPECT_THROW(invalid.SetFootPlacementEnabled(true), std::invalid_argument);
    const auto asset = MakeLegAsset();
    SkeletalPlayback player(asset), control(asset);
    player.Play("legs", true, 1, true);
    control.Play("legs", true, 1, true);
    player.SetFootPlacementEnabled(true);
    FootPlaneProbe plane;
    Eigen::Matrix4f component = Translation(0, 2, 0);
    TickFeet(player, plane, component);
    TickFeet(player, plane, component);
    ASSERT_TRUE(player.FootPlacement().active);
    const auto corrected = player.Pose(0);
    player.ApplyFootPlacement(component, true, "owner", 0, plane);
    EXPECT_EQ(player.Pose(0), corrected);
    player.SetPlaying(false);
    TickFeet(player, plane, component);
    EXPECT_EQ(player.Pose(0), corrected);
    player.SetPlaying(true);
    Eigen::Matrix4f invalidScale = component;
    invalidScale(0, 0) = 2;
    player.Tick(1.0f / 60);
    const auto before = player.Pose(0);
    EXPECT_THROW(player.ApplyFootPlacement(invalidScale, true, "owner", 1.0f / 60, plane),
        std::invalid_argument);
    EXPECT_EQ(player.Pose(0), before);
    player.TransitionTo("legs", 0.3f, true, 0.1f);
    control.TransitionTo("legs", 0.3f, true, 0.1f);
    player.Tick(0.05f);
    control.Tick(0.05f);
    for (size_t joint = 0; joint < player.Models().size(); ++joint)
        EXPECT_TRUE(player.PreIkModels()[joint].isApprox(control.PreIkModels()[joint], 0.0001f));
    player.Seek(0.5f);
    EXPECT_FALSE(player.FootPlacement().active);
    EXPECT_TRUE(player.FootPlacement().enabled);
    EXPECT_EQ(player.FootPlacement().reason, "seek");
}
}

namespace
{
using namespace VL::Animation;

const Eigen::Matrix4f& Identity4()
{
    static const Eigen::Matrix4f identity = Eigen::Matrix4f::Identity();
    return identity;
}

Eigen::Matrix4f RotationMat(float degrees, const Eigen::Vector3f& axis)
{
    Eigen::Matrix4f value = Eigen::Matrix4f::Identity();
    value.block<3, 3>(0, 0) =
        Eigen::AngleAxisf(degrees * 3.14159265358979323846f / 180.0f, axis).toRotationMatrix();
    return value;
}

Eigen::Matrix4f ScaleMat(const Eigen::Vector3f& scale)
{
    Eigen::Matrix4f value = Eigen::Matrix4f::Identity();
    value.block<3, 3>(0, 0) = scale.asDiagonal();
    return value;
}

// 三个关节：0/1 承载顶点（含一个 0.5/0.5 混合顶点），2 没有顶点。
SkinSection MakeBoundsSection()
{
    SkinSection section;
    section.inverseBinds = {Identity4(), Identity4(), Identity4()};
    section.vertices.resize(3);
    section.vertices[0].position = Eigen::Vector3f(0, 0, 0);
    section.vertices[1].position = Eigen::Vector3f(0, 10, 0);
    section.vertices[2].position = Eigen::Vector3f(10, 0, 0);
    section.influenceOffsets = {0, 1, 2, 4};
    section.influences = {{0, 1.0f}, {1, 1.0f}, {0, 0.5f}, {1, 0.5f}};
    ComputeJointBindBounds(section);
    return section;
}

void ExpectContains(const Eigen::Vector3f& min, const Eigen::Vector3f& max,
    const Eigen::Vector3f& point, float tolerance = 1.0e-4f)
{
    EXPECT_LE(min.x(), point.x() + tolerance);
    EXPECT_LE(min.y(), point.y() + tolerance);
    EXPECT_LE(min.z(), point.z() + tolerance);
    EXPECT_GE(max.x(), point.x() - tolerance);
    EXPECT_GE(max.y(), point.y() - tolerance);
    EXPECT_GE(max.z(), point.z() - tolerance);
}

void ExpectFastContainsExact(const SkinSection& section, const std::vector<Eigen::Matrix4f>& models,
    const char* label)
{
    const auto fast = BuildSkinPose(section, models, 0);
    const auto exact = BuildSkinPoseReference(section, models, 0);
    EXPECT_TRUE((fast->boundsMin.array() <= exact->boundsMin.array() + 1.0e-4f).all())
        << label << " min " << fast->boundsMin.transpose() << " vs " << exact->boundsMin.transpose();
    EXPECT_TRUE((fast->boundsMax.array() >= exact->boundsMax.array() - 1.0e-4f).all())
        << label << " max " << fast->boundsMax.transpose() << " vs " << exact->boundsMax.transpose();
    // 再逐顶点验证一次：快速盒子必须包含每个精确蒙皮位置。
    for (size_t vertex = 0; vertex < section.vertices.size(); ++vertex)
    {
        const Eigen::Vector4f point =
            (BlendSkinMatrix(section, vertex, fast->matrices) *
                section.vertices[vertex].position.homogeneous());
        ExpectContains(fast->boundsMin, fast->boundsMax, point.head<3>());
    }
}
}

TEST(SkinBounds, FastBoundsAreConservativeForIdentityTranslationRotationAndExtremePose)
{
    const SkinSection section = MakeBoundsSection();
    const std::vector<std::vector<Eigen::Matrix4f>> poses = {
        {Identity4(), Identity4(), Identity4()},
        {Identity4(), Translation(3, -7, 11), Identity4()},
        {RotationMat(37, Eigen::Vector3f::UnitZ()), RotationMat(-64, Eigen::Vector3f::UnitX()),
            Identity4()},
        // 极端姿态：接近 180° 的旋转 + 缩放 + 平移，用来暴露只记主影响的不保守。
        {RotationMat(170, Eigen::Vector3f::UnitY()) * ScaleMat(Eigen::Vector3f(3, 0.5f, 2)) *
                Translation(-40, 12, 5),
            RotationMat(-175, Eigen::Vector3f::UnitX()) * ScaleMat(Eigen::Vector3f(2, 2, 0.25f)) *
                Translation(9, -80, 30),
            Translation(500, 500, 500)}};
    for (size_t index = 0; index < poses.size(); ++index)
        ExpectFastContainsExact(section, poses[index], "pose");
}

TEST(SkinBounds, CrossJointBlendCannotBeCoveredByDominantJointAlone)
{
    const SkinSection section = MakeBoundsSection();
    // 顶点 2 的权重是 0.5/0.5。若只按“主影响（等权取最小关节）”归属，它只会进
    // 关节 0 的盒子；两个关节反向平移时精确位置在原点，主影响盒子却只覆盖 +200。
    const std::vector<Eigen::Matrix4f> models = {
        Translation(200, 0, 0), Translation(-200, 0, 0), Identity4()};
    const auto fast = BuildSkinPose(section, models, 0);
    ExpectContains(fast->boundsMin, fast->boundsMax, Eigen::Vector3f(0, 0, 0));
    ExpectFastContainsExact(section, models, "opposed");
}

TEST(SkinBounds, JointWithoutVerticesIsIgnoredAndEmptySectionStillRejected)
{
    const SkinSection section = MakeBoundsSection();
    // 关节 2 没有顶点：即使把它推到极远处，也不应污染本 section 的包围盒。
    const std::vector<Eigen::Matrix4f> models = {
        Identity4(), Translation(0, 10, 0), Translation(1.0e6f, 1.0e6f, 1.0e6f)};
    const auto fast = BuildSkinPose(section, models, 0);
    EXPECT_LT(fast->boundsMax.maxCoeff(), 1.0e5f);
    ExpectFastContainsExact(section, models, "ignored");

    SkinSection empty = section;
    empty.vertices.clear();
    empty.influenceOffsets = {0};
    empty.influences.clear();
    ComputeJointBindBounds(empty);
    EXPECT_TRUE(empty.jointBindBounds.size() == section.inverseBinds.size());
    EXPECT_THROW(BuildSkinPose(empty, models, 0), std::invalid_argument);
    EXPECT_THROW(BuildSkinPoseReference(empty, models, 0), std::invalid_argument);
}

TEST(SkinBounds, DegeneratePaletteStillRejectedByBothPaths)
{
    SkinSection section;
    section.inverseBinds = {Identity4()};
    section.vertices.resize(1);
    section.vertices[0].position = Eigen::Vector3f(1, 2, 3);
    section.influenceOffsets = {0, 1};
    section.influences = {{0, 1.0f}};
    ComputeJointBindBounds(section);
    std::vector<Eigen::Matrix4f> singular = {ScaleMat(Eigen::Vector3f(0, 0, 0))};
    EXPECT_THROW(BuildSkinPose(section, singular, 0), std::runtime_error);
    EXPECT_THROW(BuildSkinPoseReference(section, singular, 0), std::runtime_error);
    std::vector<Eigen::Matrix4f> nonfinite = {Identity4()};
    nonfinite[0](0, 3) = std::numeric_limits<float>::quiet_NaN();
    EXPECT_THROW(BuildSkinPose(section, nonfinite, 0), std::runtime_error);
    EXPECT_THROW(BuildSkinPoseReference(section, nonfinite, 0), std::runtime_error);
    // 正常姿态仍通过，并且 palette 与原语义一致。
    const auto pose = BuildSkinPose(section, {Translation(1, 1, 1)}, 0);
    EXPECT_TRUE(pose->matrices[0].isApprox(Translation(1, 1, 1), 0));
}

TEST(SkinBounds, GlobalBindToPoseUsesRestModelIndependentOfSection)
{
    SkinSection section;
    section.inverseBinds = {Identity4()};
    section.vertices.resize(1);
    section.vertices[0].position = Eigen::Vector3f(0, 100, 0);
    section.influenceOffsets = {0, 1};
    section.influences = {{0, 1.0f}};
    ComputeJointBindBounds(section);
    // 绑定姿态关节在 y=100；pose 把它沿 +X 移 5 cm。
    std::vector<Joint> joints(1);
    joints[0].restModel = Translation(0, 100, 0);
    const std::vector<Eigen::Matrix4f> models = {Translation(5, 100, 0)};

    const auto pose = BuildSkinPose(section, models, 0, 0, &joints);
    ASSERT_EQ(pose->bindToPoseMatrices.size(), 1u);
    // 全骨架 bind→pose 把绑定空间点映射到姿态空间：T(5,100)*T(0,-100)=T(5,0)。
    EXPECT_TRUE(pose->bindToPoseMatrices[0].isApprox(Translation(5, 0, 0), 1.0e-5f));
    // per-section palette 在 inverseBind 为单位阵时退化为绝对 model 矩阵；
    // 这正是它不能跨 section 复用的原因。
    EXPECT_TRUE(pose->matrices[0].isApprox(Translation(5, 100, 0), 1.0e-5f));

    // 不传 joints 时保持旧行为：不生成全局矩阵。
    const auto legacy = BuildSkinPose(section, models, 0);
    EXPECT_TRUE(legacy->bindToPoseMatrices.empty());
}

TEST(SkinBounds, WeightSumDeviationIsAbsorbedByConservativeExpansion)
{
    SkinSection section;
    section.inverseBinds = {Identity4(), Identity4()};
    section.vertices.resize(1);
    section.vertices[0].position = Eigen::Vector3f(0, 0, 0);
    section.influenceOffsets = {0, 2};
    // LBS 不归一化：Σw = 1.0001，精确位置比两个关节盒子的并集再远 0.01%。
    section.influences = {{0, 0.50005f}, {1, 0.50005f}};
    ComputeJointBindBounds(section);
    ASSERT_GT(section.jointBindWeightSumError, 0.0f);
    const std::vector<Eigen::Matrix4f> models = {Translation(10000, 0, 0), Translation(10000, 0, 0)};
    const auto fast = BuildSkinPose(section, models, 0);
    const auto exact = BuildSkinPoseReference(section, models, 0);
    ExpectContains(fast->boundsMin, fast->boundsMax, exact->boundsMax, 1.0e-3f);
}
