#include <gtest/gtest.h>
#include <assimp/scene.h>
#include "animation/skeletalPlayback.h"
#include "animation/animationMontage.h"
#include "animation/animationSkeleton.h"
#include "animation/animationSequenceData.h"
#include "mesh/loader/animation/assimpSkeletalImporter.h"
#include "script/scriptRuntime.h"
#include "script/scriptComponents.h"
#include "input/inputSubsystem.h"
#include "engine/diagnosticsSubsystem.h"
#include "world/world.h"
#include "sceneNode.h"
#include "animation/motionRuntimeDatabase.h"
#include "shader/build/contentHash.h"
#include <cmath>
#include <array>
#include <algorithm>
#include <filesystem>
#include <fstream>
#include <map>
#include <limits>
#include <optional>

namespace
{
using namespace VL;
using Json = nlohmann::json;

std::filesystem::path FormalResourceRoot()
{
    const auto project = std::filesystem::path(__FILE__).parent_path().parent_path().parent_path();
    std::ifstream input(project / "config/config.json");
    const auto config = Json::parse(input);
    return std::filesystem::weakly_canonical(project / config.at("resourcePath").get<std::string>());
}

std::shared_ptr<Animation::SkeletalPlayback> FormalPlayer()
{
    static std::shared_ptr<Animation::SkeletalAsset> asset;
    if (!asset)
    {
        const auto root = FormalResourceRoot();
        const auto capture = Animation::CaptureMotionDatabaseInputs(root, "Maps/SC_gasp/MotionMatching/DB_gasp.json");
        const auto& model = capture.at("model");
        auto candidate = std::make_shared<Animation::SkeletalAsset>();
        candidate->mesh = Animation::AssimpSkeletalImporter::ReadMesh(
            root / model.at("modelDataPath").get<std::string>(),
            capture.at("rootJoint").get<std::string>());
        for (const auto& clip : model.at("skinning").at("clips"))
            candidate->clips.push_back(Animation::AssimpSkeletalImporter::ReadClip(
                root / clip.at("path").get<std::string>(), candidate->mesh, clip.at("id")));
        for (size_t index = 2; index < capture.at("inputs").size(); ++index)
            candidate->sourceInputs.emplace_back(capture.at("inputs")[index].at("path"),
                capture.at("inputs")[index].at("blake3"));
        asset = std::move(candidate);
    }
    return std::make_shared<Animation::SkeletalPlayback>(asset);
}

Json ReadResourceJson(const std::filesystem::path& root, const std::string& relative)
{
    std::ifstream input(root / relative);
    if (!input) throw std::runtime_error("Missing animation test resource: " + relative);
    return Json::parse(input);
}

// 真实 Hurdle 资产（SK + sidecar + Montage）用于 C# CurveValue/PlayMontage/notify 提交面测试。
// 只导入被 Montage 引用的一个 clip，避免把 22 个 FBX 都读一遍；合成 Montage 变体只进入
// catalog，用于验证 C# 执行拒绝矩阵，不参与实际播放。
std::shared_ptr<Animation::SkeletalPlayback> FormalMontagePlayer()
{
    static std::shared_ptr<Animation::SkeletalAsset> asset;
    if (!asset)
    {
        const auto root = FormalResourceRoot();
        auto candidate = std::make_shared<Animation::SkeletalAsset>();
        const std::string skeletonPath = "Maps/SC_gasp/Skeletons/SK_UEFN_Mannequin.json";
        const std::string smPath = "Maps/SC_gasp/Meshes/SM_mannequin.json";
        candidate->skeleton = Animation::ParseSkeleton(ReadResourceJson(root, skeletonPath), skeletonPath);
        candidate->skeleton.sourceDigest = VL::ContentHasher::HashFile(root / skeletonPath).ToHex();
        const auto sm = ReadResourceJson(root, smPath);
        const auto& skinning = sm.at("skinning");
        candidate->mesh = Animation::AssimpSkeletalImporter::ReadMesh(
            root / sm.at("modelDataPath").get<std::string>(), candidate->skeleton.root);
        Animation::ValidateSkeletonAgainstMesh(candidate->skeleton, candidate->mesh,
            sm.at("modelDataPath").get<std::string>());
        const std::string clipId = "M_Neutral_Traversal_Hurdle_1_0_stand_F_V2_Lfoot";
        for (const auto& clip : skinning.at("clips"))
            if (clip.at("id") == clipId)
                candidate->clips.push_back(Animation::AssimpSkeletalImporter::ReadClip(
                    root / clip.at("path").get<std::string>(), candidate->mesh, clipId));
        if (candidate->clips.empty()) throw std::runtime_error("Hurdle clip is missing from SM_mannequin");
        const std::string sequencePath = "Maps/SC_gasp/Source/" + clipId + ".anim.json";
        candidate->clips[0].sequence = Animation::ParseSequence(ReadResourceJson(root, sequencePath), sequencePath);
        candidate->clips[0].sequence.sourceDigest = VL::ContentHasher::HashFile(root / sequencePath).ToHex();
        Animation::ValidateSequenceAgainstSkeleton(candidate->clips[0].sequence, candidate->skeleton, sequencePath);
        const std::string montagePath = "Maps/SC_gasp/Animations/AM_" + clipId + ".json";
        auto montage = Animation::ParseMontage(ReadResourceJson(root, montagePath), montagePath);
        montage.sourcePath = montagePath;
        Animation::MontageImportContext context;
        context.skeleton = &candidate->skeleton;
        context.sequences.push_back({montage.sequencePath, &candidate->clips[0].sequence,
            candidate->clips[0].sequence.sourceDigest});
        context.clipIds = {clipId};
        montage.derivedClipId = Animation::BindMontage(montage, context, montagePath);
        candidate->montages.push_back(montage);

        auto variant = montage;
        variant.id = "AM_Test_MultiSection";
        variant.sourcePath.clear();
        variant.sections.push_back({"Second", 0.5f, 1.0f});
        candidate->montages.push_back(variant);
        variant = montage;
        variant.id = "AM_Test_Static";
        variant.sourcePath.clear();
        for (auto& notify : variant.notifies)
            if (notify.warp && notify.warp->provider == "Bone")
            {
                notify.warp->provider = "Static";
                notify.warp->bone.clear();
                notify.warp->staticPose = Animation::MontageStaticPose{};
                break;
            }
        candidate->montages.push_back(variant);
        variant = montage;
        variant.id = "AM_Test_NonLinear";
        variant.sourcePath.clear();
        for (auto& notify : variant.notifies)
            if (notify.warp)
            {
                notify.warp->easing = "EaseInOut";
                break;
            }
        candidate->montages.push_back(variant);
        asset = std::move(candidate);
    }
    return std::make_shared<Animation::SkeletalPlayback>(asset);
}

Json MotionRequest(const std::string& component, Json allowed)
{
    return {{"component", component}, {"mode", "motionMatching"},
        {"database", "Maps/SC_gasp/MotionMatching/DB_gasp.json"},
        {"allowedClips", std::move(allowed)}, {"actionLocked", false}};
}

Json MontageRequest(const std::string& component, Json targets,
    const std::string& montage = "AM_Test", std::optional<float> startTime = std::nullopt)
{
    auto request = Json{{"component", component}, {"mode", "playMontage"}, {"montage", montage},
        {"targets", std::move(targets)}, {"loop", false}, {"rate", 1}, {"inPlace", true}};
    // 缺省时不写 startTime：wire 形状保持旧请求逐位一致，只有显式入口才带该字段。
    if (startTime) request["startTime"] = *startTime;
    return request;
}

// 最小可手算根轨迹；通过正式导入器和真实 CLR/Jolt 验证接口，不依赖 Vulkan。
std::shared_ptr<Animation::SkeletalAsset> TestAsset()
{
    aiScene scene;
    scene.mMetaData = aiMetadata::Alloc(7);
    scene.mMetaData->Set(0, "UnitScaleFactor", 1.0);
    scene.mMetaData->Set(1, "UpAxis", 2); scene.mMetaData->Set(2, "UpAxisSign", 1);
    scene.mMetaData->Set(3, "FrontAxis", 1); scene.mMetaData->Set(4, "FrontAxisSign", -1);
    scene.mMetaData->Set(5, "CoordAxis", 0); scene.mMetaData->Set(6, "CoordAxisSign", 1);
    scene.mRootNode = new aiNode("root");
    scene.mRootNode->mNumMeshes = 1; scene.mRootNode->mMeshes = new unsigned[1]{0};
    scene.mNumMeshes = 1; scene.mMeshes = new aiMesh*[1]{new aiMesh()};
    auto& mesh = *scene.mMeshes[0];
    mesh.mNumVertices = 3;
    mesh.mVertices = new aiVector3D[3]{{0, 0, 0}, {1, 0, 0}, {0, 1, 0}};
    mesh.mNormals = new aiVector3D[3]{{0, 0, 1}, {0, 0, 1}, {0, 0, 1}};
    mesh.mTextureCoords[0] = new aiVector3D[3]{{0, 0, 0}, {1, 0, 0}, {0, 1, 0}};
    mesh.mNumUVComponents[0] = 2; mesh.mNumFaces = 1;
    mesh.mFaces = new aiFace[1]; mesh.mFaces[0].mNumIndices = 3;
    mesh.mFaces[0].mIndices = new unsigned[3]{0, 1, 2};
    mesh.mNumBones = 1; mesh.mBones = new aiBone*[1]{new aiBone()};
    mesh.mBones[0]->mName = "root"; mesh.mBones[0]->mNumWeights = 3;
    mesh.mBones[0]->mWeights = new aiVertexWeight[3]{{0, 1}, {1, 1}, {2, 1}};
    scene.mNumAnimations = 1; scene.mAnimations = new aiAnimation*[1]{new aiAnimation()};
    auto& animation = *scene.mAnimations[0];
    animation.mDuration = 30; animation.mTicksPerSecond = 30; animation.mNumChannels = 1;
    animation.mChannels = new aiNodeAnim*[1]{new aiNodeAnim()};
    animation.mChannels[0]->mNodeName = "root";
    animation.mChannels[0]->mNumPositionKeys = 2;
    animation.mChannels[0]->mPositionKeys = new aiVectorKey[2]{{0, {0, 0, 0}}, {30, {0, -100, 0}}};
    auto asset = std::make_shared<Animation::SkeletalAsset>();
    asset->mesh = Animation::AssimpSkeletalImporter::BuildMesh(scene, "root");
    for (const char* clip : {"walk", "jump", "M_Neutral_Stand_Idle_Loop", "M_Neutral_Walk_Loop_F",
        "M_Neutral_Run_Loop_F", "M_Neutral_Jump_F_Start_Stand_Lfoot",
        "M_Neutral_Jump_Loop_Fall", "M_Neutral_Jump_F_Land_Stand_Light_Lfoot"})
        asset->clips.push_back(Animation::AssimpSkeletalImporter::BuildClip(scene, asset->mesh, clip));
    return asset;
}

std::shared_ptr<Animation::SkeletalPlayback> TestPlayer()
{
    return std::make_shared<Animation::SkeletalPlayback>(TestAsset());
}

// 合成资产只验证请求装配（playMontage 的分支/窗口/范围/notify），不依赖外部资源仓库。
std::shared_ptr<Animation::SkeletalPlayback> TestMontagePlayer()
{
    auto asset = TestAsset();
    auto& clip = asset->clips.front();
    clip.sequence.id = clip.id;
    clip.sequence.duration = 1.0f;
    clip.sequence.curves.push_back({"distance_from_ledge", {0, 1}, {10, 20}});
    Animation::Montage montage;
    montage.id = "AM_Test";
    montage.sequencePath = "Maps/Test/walk.anim.json";
    montage.derivedClipId = "walk";
    montage.slot = "DefaultSlot";
    montage.length = 0.8f;
    montage.sections.push_back({"Default", 0, 0.8f, "", 0, ""});
    montage.notifies.push_back({"notify", "1", "Footstep", 0, 0, std::nullopt});
    Animation::MontageWarp warp;
    warp.target = "FrontLedge";
    warp.provider = "None";
    warp.subtractRemainingRootMotion = true;
    warp.ignoreZAxis = true;
    montage.notifies.push_back({"notifyState", "1", "MotionWarping", 0.1f, 0.3f, warp});
    montage.windows.push_back({"FrontLedge", 0.1f, 0.4f});
    asset->montages.push_back(std::move(montage));
    return std::make_shared<Animation::SkeletalPlayback>(asset);
}

Json Actor(const std::string& id, const std::string& type = "ScriptFixtures.AnimationProbe")
{
    return {{"type", "blueprintInstance"}, {"id", id}, {"name", id}, {"class", type},
        {"position", {0, 0, 0}}, {"rotation", {0, 0, 0}}, {"scale", {1, 1, 1}},
        {"properties", Json::object()}, {"components", Json::array({
            {{"id", "visual"}, {"type", "mesh"}, {"modelPath", "Maps/SC_gasp/Meshes/SM_mannequin.json"}}})}};
}

TEST(ScriptAnimationWorld, SpawnClonesShareAssetAndSectionsButNeverTheirClock)
{
    World world(1, "animation");
    auto actor = Actor("template");
    WorldMeshObject section;
    section.sceneObjectIdentity = "__blueprint/template/visual";
    section.animation = TestPlayer(); section.skinPose = section.animation->Pose(0);
    world.AddMeshObject(section.sceneObjectIdentity, section);
    world.AddMeshObject(section.sceneObjectIdentity + "~section.1", section);
    auto first = Actor("one"), second = Actor("two");
    world.CloneScriptBlueprintMeshes("template", first);
    world.CloneScriptBlueprintMeshes("template", second);
    std::shared_ptr<Animation::SkeletalPlayback> one, two;
    for (const auto& entry : world.GetMeshObjects())
    {
        if (entry.second.sceneObjectIdentity == "__blueprint/one/visual")
        {
            if (one) EXPECT_EQ(entry.second.animation, one);
            one = entry.second.animation;
        }
        if (entry.second.sceneObjectIdentity == "__blueprint/two/visual") two = entry.second.animation;
    }
    ASSERT_TRUE(one); ASSERT_TRUE(two);
    EXPECT_NE(one, two); EXPECT_NE(one, section.animation);
    EXPECT_EQ(&one->Asset(), &two->Asset());
    one->Tick(0.25f);
    EXPECT_FLOAT_EQ(two->Time(), 0);
    EXPECT_FLOAT_EQ(section.animation->Time(), 0);
}

TEST(ScriptAnimationWorld, InvalidBatchLeavesEarlierTargetsUntouched)
{
    World world(1, "animation");
    auto actor = Actor("owner");
    actor["components"].push_back({{"id", "extra"}, {"type", "mesh"},
        {"modelPath", "Maps/SC_gasp/Meshes/SM_mannequin.json"}});
    for (const auto& node : actor["components"])
    {
        WorldMeshObject mesh; mesh.sceneObjectIdentity = ScriptMeshIdentity("owner", node);
        mesh.animation = TestPlayer(); mesh.skinPose = mesh.animation->Pose(0);
        world.AddMeshObject(mesh.sceneObjectIdentity, mesh);
    }
    const auto before = world.GetMeshObjects().at("__blueprint/owner/visual").animation->Pose(0);
    EXPECT_THROW(world.ApplyScriptAnimationRequests(actor, Json::array({
        {{"component", "visual"}, {"clip", "jump"}, {"loop", false}, {"rate", 1}, {"inPlace", true}},
        {{"component", "extra"}, {"clip", "missing"}, {"loop", true}, {"rate", 1}, {"inPlace", true}}
    })), std::invalid_argument);
    EXPECT_EQ(world.GetMeshObjects().at("__blueprint/owner/visual").animation->Pose(0), before);
}

// 旧 clip 提交面的 warpTargets 已删除；字段出现即拒绝，且不得改动目标播放器。
TEST(ScriptAnimationWorld, RejectsClipRequestWithWarpTargets)
{
    World world(1, "animation");
    auto actor = Actor("owner");
    WorldMeshObject mesh;
    mesh.sceneObjectIdentity = ScriptMeshIdentity("owner", actor.at("components").front());
    mesh.animation = TestPlayer(); mesh.skinPose = mesh.animation->Pose(0);
    world.AddMeshObject(mesh.sceneObjectIdentity, mesh);
    const auto before = world.GetMeshObjects().at("__blueprint/owner/visual").animation->Pose(0);
    std::string message;
    try
    {
        world.ApplyScriptAnimationRequests(actor, Json::array({
            {{"component", "visual"}, {"clip", "walk"}, {"loop", true}, {"rate", 1},
             {"inPlace", true}, {"warpTargets", Json::array()}}}));
    }
    catch (const std::invalid_argument& error)
    {
        message = error.what();
    }
    EXPECT_NE(message.find("warp targets are defined by montage assets"), std::string::npos);
    EXPECT_EQ(world.GetMeshObjects().at("__blueprint/owner/visual").animation->Pose(0), before);
}

TEST(ScriptAnimationWorld, CatalogKeepsClipListAndPublishesCurvesAndMontagesSeparately)
{
    World world(1, "animation");
    auto actor = Actor("owner");
    WorldMeshObject mesh;
    mesh.sceneObjectIdentity = ScriptMeshIdentity("owner", actor.at("components").front());
    mesh.animation = TestMontagePlayer();
    mesh.skinPose = mesh.animation->Pose(0);
    world.AddMeshObject(mesh.sceneObjectIdentity, mesh);
    world.SetScriptResolvedBlueprints(Json::array({actor}));

    // 旧 catalog 仍是 modelPath -> clip id 数组，不因新资产段改变形状。
    const auto catalog = world.ScriptAnimationCatalog();
    const auto& clips = catalog.at("Maps/SC_gasp/Meshes/SM_mannequin.json");
    ASSERT_TRUE(clips.is_array());
    EXPECT_EQ(clips.size(), mesh.animation->Asset().clips.size());
    EXPECT_EQ(clips.front(), "walk");

    const auto assets = world.ScriptAnimationAssets();
    const auto& entry = assets.at("Maps/SC_gasp/Meshes/SM_mannequin.json");
    // 只有带 sidecar 曲线的 clip 进入 curves；没有 sidecar 的 jump 不发布空结构。
    ASSERT_TRUE(entry.at("curves").contains("walk"));
    EXPECT_FALSE(entry.at("curves").contains("jump"));
    const auto& curve = entry.at("curves").at("walk").front();
    EXPECT_EQ(curve.at("name"), "distance_from_ledge");
    EXPECT_EQ(curve.at("times"), Json::array({0, 1}));
    EXPECT_EQ(curve.at("values"), Json::array({10, 20}));

    ASSERT_EQ(entry.at("montages").size(), 1u);
    const auto& montage = entry.at("montages").front();
    EXPECT_EQ(montage.at("id"), "AM_Test");
    EXPECT_EQ(montage.at("sequencePath"), "Maps/Test/walk.anim.json");
    EXPECT_EQ(montage.at("derivedClipId"), "walk");
    EXPECT_FLOAT_EQ(montage.at("length").get<float>(), 0.8f);
    EXPECT_EQ(montage.at("slot"), "DefaultSlot");
    ASSERT_EQ(montage.at("sections").size(), 1u);
    EXPECT_EQ(montage.at("sections")[0].at("name"), "Default");
    ASSERT_EQ(montage.at("notifies").size(), 2u);
    EXPECT_FALSE(montage.at("notifies")[0].contains("warp"));
    const auto& warp = montage.at("notifies")[1].at("warp");
    EXPECT_EQ(warp.at("target"), "FrontLedge");
    EXPECT_EQ(warp.at("provider"), "None");
    EXPECT_TRUE(warp.at("subtractRemainingRootMotion").get<bool>());
    EXPECT_TRUE(warp.at("ignoreZAxis").get<bool>());
    EXPECT_EQ(warp.at("easing"), "Linear");
}

TEST(ScriptAnimationWorld, PlayMontageAppliesRangeNotifiesAndMontageFeedback)
{
    World world(1, "animation");
    auto actor = Actor("owner");
    WorldMeshObject mesh;
    mesh.sceneObjectIdentity = ScriptMeshIdentity("owner", actor.at("components").front());
    mesh.animation = TestMontagePlayer();
    mesh.skinPose = mesh.animation->Pose(0);
    world.AddMeshObject(mesh.sceneObjectIdentity, mesh);
    world.SetScriptBlueprintStates(Json::array({actor}));

    world.ApplyScriptAnimationRequests(actor, Json::array({
        MontageRequest("visual", Json{{"FrontLedge", {0, 100, 200}}})}));
    auto player = world.GetMeshObjects().at("__blueprint/owner/visual").animation;
    EXPECT_EQ(player->SelectedClipId(), "walk");
    EXPECT_EQ(player->MontageId(), "AM_Test");
    EXPECT_FLOAT_EQ(player->MontageLength(), 0.8f);
    EXPECT_FLOAT_EQ(player->PlaybackRangeStart(), 0.0f);
    EXPECT_FLOAT_EQ(player->PlaybackRangeEnd(), 0.8f);
    EXPECT_FLOAT_EQ(player->Duration(), 0.8f);
    EXPECT_FLOAT_EQ(player->Time(), 0.0f);

    world.TickScriptAnimations(0.05f);
    EXPECT_EQ(player->MotionWarpState(), "FrontLedge:waiting");
    const auto feedback = world.ScriptAnimationFeedback();
    ASSERT_EQ(feedback.size(), 1u);
    const auto& value = feedback.front();
    EXPECT_EQ(value.at("clip"), "walk");
    EXPECT_EQ(value.at("montage").at("id"), "AM_Test");
    EXPECT_FLOAT_EQ(value.at("montage").at("length").get<float>(), 0.8f);
    EXPECT_EQ(value.at("montage").at("range"), Json::array({0.0f, 0.8f}));
    ASSERT_EQ(value.at("notifies").size(), 1u);
    const auto& notify = value.at("notifies")[0];
    EXPECT_EQ(notify.at("name"), "Footstep");
    EXPECT_EQ(notify.at("track"), "1");
    EXPECT_EQ(notify.at("kind"), "notify");
    EXPECT_EQ(notify.at("phase"), "instant");
    EXPECT_FLOAT_EQ(notify.at("time").get<float>(), 0.0f);
    EXPECT_FLOAT_EQ(notify.at("startTime").get<float>(), 0.0f);
    EXPECT_FLOAT_EQ(notify.at("duration").get<float>(), 0.0f);
    EXPECT_FALSE(notify.at("interrupted").get<bool>());
}

TEST(ScriptAnimationWorld, PlayMontageRejectionLeavesEveryTargetUntouched)
{
    World world(1, "animation");
    auto actor = Actor("owner");
    actor["components"].push_back({{"id", "extra"}, {"type", "mesh"},
        {"modelPath", "Maps/SC_gasp/Meshes/SM_mannequin.json"}});
    for (const auto& node : actor["components"])
    {
        WorldMeshObject mesh; mesh.sceneObjectIdentity = ScriptMeshIdentity("owner", node);
        mesh.animation = TestMontagePlayer(); mesh.skinPose = mesh.animation->Pose(0);
        world.AddMeshObject(mesh.sceneObjectIdentity, mesh);
    }
    std::map<std::string, std::shared_ptr<const Animation::SkinPose>> before;
    for (const auto& entry : world.GetMeshObjects())
        before.emplace(entry.first, entry.second.animation->Pose(0));
    // 同一批里前一个组件合法、后一个缺 target：候选失败不得发布前一个组件的新播放段。
    EXPECT_THROW(world.ApplyScriptAnimationRequests(actor, Json::array({
        MontageRequest("visual", Json{{"FrontLedge", {0, 100, 200}}}),
        MontageRequest("extra", Json::object())})), std::invalid_argument);
    for (const auto& entry : world.GetMeshObjects())
        EXPECT_EQ(entry.second.animation->Pose(0), before.at(entry.first));
    // 未知 Montage 与多余 target 同样拒绝，当前播放器保持不变。
    EXPECT_THROW(world.ApplyScriptAnimationRequests(actor, Json::array({
        MontageRequest("visual", Json{{"FrontLedge", {0, 100, 200}}}, "AM_missing")})),
        std::invalid_argument);
    EXPECT_THROW(world.ApplyScriptAnimationRequests(actor, Json::array({
        MontageRequest("visual", Json{{"FrontLedge", {0, 100, 200}}, {"Extra", {0, 0, 0}}})})),
        std::invalid_argument);
    for (const auto& entry : world.GetMeshObjects())
        EXPECT_EQ(entry.second.animation->Pose(0), before.at(entry.first));
}

// UE 的 PoseSearchBranchIn 不选帧：它把动画在 DB 里的可采样范围钉成 notify 的
// [Time, Time+Duration]，真正选帧的是 Chooser 的 Pose Match 列。VL 没有该列，
// 入口取 notify.Time（= 范围下界）：Catch 的窗口约 1 帧，与 UE 选帧的差 ≤1 帧；
// 其余家族窗口从 0 开始，VL 从 0 起播。该规则只适用于 Montage —— locomotion
// sequence 上也有同名 notify，任何“扫 clip”的通用规则都会读错时间。
TEST(ScriptAnimationWorld, MontageBranchInWindowsAreSamplingRanges)
{
    const auto root = FormalResourceRoot();
    const auto sm = ReadResourceJson(root, "Maps/SC_gasp/Meshes/SM_mannequin.json");
    int catchCount = 0;
    int otherCount = 0;
    for (const auto& entry : sm.at("skinning").at("montages"))
    {
        const auto id = entry.at("id").get<std::string>();
        const auto montage = ReadResourceJson(root, entry.at("path").get<std::string>());
        float time = -1;
        float duration = -1;
        for (const auto& notify : montage.at("notifies"))
        {
            if (notify.at("name") != "PoseSearchBranchIn") continue;
            time = notify.at("time").get<float>();
            duration = notify.at("duration").get<float>();
            break;
        }
        ASSERT_GE(time, 0.0f) << id;
        if (id.find("Catch_") != std::string::npos)
        {
            ++catchCount;
            // 30 fps 资产：Catch 的可采样窗口约 1 帧（留 1.2 帧余量）。
            EXPECT_LE(duration, 1.2f / 30.0f) << id;
        }
        else
        {
            ++otherCount;
            EXPECT_NEAR(time, 0.0f, 1e-4f) << id;
        }
    }
    EXPECT_EQ(catchCount, 9);
    EXPECT_EQ(otherCount, 10);
}

TEST(ScriptAnimationWorld, PlayMontageStartTimeAppliesEntryOffsetAndSkipsEarlierNotifies)
{
    World world(1, "animation");
    auto actor = Actor("owner");
    WorldMeshObject mesh;
    mesh.sceneObjectIdentity = ScriptMeshIdentity("owner", actor.at("components").front());
    mesh.animation = TestMontagePlayer();
    mesh.skinPose = mesh.animation->Pose(0);
    world.AddMeshObject(mesh.sceneObjectIdentity, mesh);
    world.SetScriptBlueprintStates(Json::array({actor}));

    // 夹具：clip 1 s、Montage section [0,0.8]、Footstep 在 0、warp 窗口 [0.1,0.4]。
    // 入口 0.5 落在窗口之后，等价 Catch 从 PoseSearchBranchIn 进入、跳过前摇。
    world.ApplyScriptAnimationRequests(actor, Json::array({
        MontageRequest("visual", Json{{"FrontLedge", {0, 100, 200}}}, "AM_Test", 0.5f)}));
    auto player = world.GetMeshObjects().at("__blueprint/owner/visual").animation;
    EXPECT_FLOAT_EQ(player->Time(), 0.5f);
    EXPECT_FLOAT_EQ(player->PlaybackRangeStart(), 0.5f);
    EXPECT_FLOAT_EQ(player->PlaybackRangeEnd(), 0.8f);
    EXPECT_NEAR(player->Duration(), 0.3f, 1e-6f);

    world.TickScriptAnimations(0.05f);
    EXPECT_NEAR(player->Time(), 0.55f, 1e-5f);
    // 入口之前的 notify 不补发，已结束的窗口不再激活。
    EXPECT_EQ(player->MotionWarpState(), "FrontLedge:done");
    EXPECT_TRUE(player->FiredNotifies().empty());
    EXPECT_TRUE(player->ActiveNotifyStates().empty());
    // 根位移只从入口起积分；精确数值由合成 WarpClip 的 C++ 测试
    // `AnimationScriptData.PlayMontageStartTimeSkipsEarlierNotifiesAndRootMotion` 覆盖，
    // 这里的 assimp 夹具只保证不会回放入口之前的位移方向。
    EXPECT_LE(player->RootMotionDelta().y(), 0.0f);
    const auto feedback = world.ScriptAnimationFeedback();
    ASSERT_EQ(feedback.size(), 1u);
    EXPECT_TRUE(feedback.front().at("notifies").empty());
    EXPECT_FLOAT_EQ(feedback.front().at("montage").at("range")[0].get<float>(), 0.5f);
    EXPECT_FLOAT_EQ(feedback.front().at("montage").at("range")[1].get<float>(), 0.8f);

    // 控制面与编辑器时间轴使用播放范围内的相对秒数；非零入口必须仍能定位到
    // clip-local 的绝对时间，而不能把 0 直接传给 Seek。
    EXPECT_TRUE(player->IsPlaying());
    EXPECT_FALSE(world.ControlScriptAnimation(world.GetGeneration(),
        "__blueprint/owner/visual", 0, 0.31f));
    EXPECT_TRUE(player->IsPlaying());
    ASSERT_TRUE(world.ControlScriptAnimation(world.GetGeneration(),
        "__blueprint/owner/visual", 0, 0.0f));
    EXPECT_FLOAT_EQ(player->Time(), 0.5f);
    ASSERT_TRUE(world.ControlScriptAnimation(world.GetGeneration(),
        "__blueprint/owner/visual", 0, 0.1f));
    EXPECT_FLOAT_EQ(player->Time(), 0.6f);
    ASSERT_TRUE(world.ControlScriptAnimation(world.GetGeneration(),
        "__blueprint/owner/visual", 0, player->Duration()));
    EXPECT_FLOAT_EQ(player->Time(), player->PlaybackRangeEnd());
}

TEST(ScriptAnimationWorld, PlayMontageStartTimeWireRejectsInvalidValues)
{
    World world(1, "animation");
    auto actor = Actor("owner");
    WorldMeshObject mesh;
    mesh.sceneObjectIdentity = ScriptMeshIdentity("owner", actor.at("components").front());
    mesh.animation = TestMontagePlayer();
    mesh.skinPose = mesh.animation->Pose(0);
    world.AddMeshObject(mesh.sceneObjectIdentity, mesh);
    const auto before = world.GetMeshObjects().at("__blueprint/owner/visual").animation->Pose(0);

    const auto reject = [&](Json startTime) {
        auto request = MontageRequest("visual", Json{{"FrontLedge", {0, 100, 200}}});
        request["startTime"] = std::move(startTime);
        std::string message;
        try { world.ApplyScriptAnimationRequests(actor, Json::array({request})); }
        catch (const std::invalid_argument& error) { message = error.what(); }
        return message;
    };
    // 非数字、负数、非有限、null 都是 wire 形状错误，统一拒绝码。
    EXPECT_EQ(reject("0.5"), "Script.InvalidMontageStartTime");
    EXPECT_EQ(reject(-0.1), "Script.InvalidMontageStartTime");
    EXPECT_EQ(reject(std::numeric_limits<double>::infinity()), "Script.InvalidMontageStartTime");
    EXPECT_EQ(reject(nullptr), "Script.InvalidMontageStartTime");
    // 形状合法但落在 section 0 之外：由原生执行层带 montage id/范围拒绝。
    const std::string outside = reject(0.8);
    EXPECT_NE(outside.find("AM_Test"), std::string::npos);
    EXPECT_NE(outside.find("startTime"), std::string::npos);
    EXPECT_NE(outside.find("playable range"), std::string::npos);
    // 任何拒绝都不得改动当前播放器。
    EXPECT_EQ(world.GetMeshObjects().at("__blueprint/owner/visual").animation->Pose(0), before);
}

TEST(ScriptAnimationWorld, PlayMontageTargetWireAcceptsPositionOrPositionPlusRotation)
{
    World world(1, "animation");
    auto actor = Actor("owner");
    WorldMeshObject mesh;
    mesh.sceneObjectIdentity = ScriptMeshIdentity("owner", actor.at("components").front());
    mesh.animation = TestMontagePlayer(); mesh.skinPose = mesh.animation->Pose(0);
    world.AddMeshObject(mesh.sceneObjectIdentity, mesh);

    // 7 个数 = 位置 + UE 四元数 (x,y,z,w)；旧的 3 个数也必须继续可用。
    EXPECT_NO_THROW(world.ApplyScriptAnimationRequests(actor, Json::array({
        MontageRequest("visual", Json{{"FrontLedge", {0, 100, 200, 0, 0, 0, 1}}})})));
    EXPECT_NO_THROW(world.ApplyScriptAnimationRequests(actor, Json::array({
        MontageRequest("visual", Json{{"FrontLedge", {0, 100, 200}}})})));
    // 4 个数、零四元数、非有限数都拒绝；形状错误不能静默当成纯位置。
    EXPECT_THROW(world.ApplyScriptAnimationRequests(actor, Json::array({
        MontageRequest("visual", Json{{"FrontLedge", {0, 100, 200, 0}}})})), std::invalid_argument);
    EXPECT_THROW(world.ApplyScriptAnimationRequests(actor, Json::array({
        MontageRequest("visual", Json{{"FrontLedge", {0, 100, 200, 0, 0, 0, 0}}})})),
        std::invalid_argument);
    EXPECT_THROW(world.ApplyScriptAnimationRequests(actor, Json::array({
        MontageRequest("visual", Json{{"FrontLedge",
            Json::array({0, 100, 200, 0, 0, 0, "w"})}})})), std::invalid_argument);
}

TEST(ScriptAnimationWorld, DatabaseRejectsSameNamedClipsFromDifferentImportedBytes)
{
    auto player = TestPlayer();
    EXPECT_THROW(Animation::LoadMotionRuntimeDatabase(FormalResourceRoot(),
        "Maps/SC_gasp/MotionMatching/DB_gasp.json", "Maps/SC_gasp/Meshes/SM_mannequin.json", player->Asset()),
        std::invalid_argument);
}

TEST(ScriptAnimationWorld, CandidateBatchFailureDoesNotPublishEarlierMotionBinding)
{
    World world(1, "motion");
    world.SetMotionResourceRoot(FormalResourceRoot().string());
    auto actor = Actor("owner");
    actor["components"].push_back({{"id", "extra"}, {"type", "mesh"},
        {"modelPath", "Maps/SC_gasp/Meshes/SM_mannequin.json"}});
    for (const auto& node : actor["components"])
    {
        WorldMeshObject mesh;
        mesh.sceneObjectIdentity = ScriptMeshIdentity("owner", node);
        mesh.animation = FormalPlayer(); mesh.skinPose = mesh.animation->Pose(0);
        world.AddMeshObject(mesh.sceneObjectIdentity, mesh);
    }
    const auto before = world.GetMeshObjects().at("__blueprint/owner/visual").animation->Pose(0);
    auto first = MotionRequest("visual", Json::array({"M_Neutral_Stand_Idle_Loop"}));
    auto invalid = MotionRequest("extra", Json::array({"missing"}));
    EXPECT_THROW(world.ApplyScriptAnimationRequests(actor, Json::array({first, invalid})), std::invalid_argument);
    world.SetScriptBlueprintStates(Json::array({actor}));
    EXPECT_EQ(world.GetMeshObjects().at("__blueprint/owner/visual").animation->Pose(0), before);
    EXPECT_FALSE(world.ScriptAnimationFeedback().front().contains("motion"));
    // 旧客户端省略开关时保持关闭；后一个非法布尔值不能提前发布前一个组件的开关。
    auto second = MotionRequest("extra", Json::array({"M_Neutral_Stand_Idle_Loop"}));
    world.ApplyScriptAnimationRequests(actor, Json::array({first, second}));
    world.TickScriptAnimations(1.0f / 60);
    first["matchSpeed"] = true;
    second["matchSpeed"] = "true";
    EXPECT_THROW(world.ApplyScriptAnimationRequests(actor, Json::array({first, second})), std::invalid_argument);
    world.TickScriptAnimations(1.0f / 60);
    for (const auto& feedback : world.ScriptAnimationFeedback())
        EXPECT_FALSE(feedback.at("motion").at("speedMatching").at("enabled").get<bool>());
    world.ApplyScriptAnimationRequests(actor, Json::array({first}));
    world.TickScriptAnimations(1.0f / 60);
    EXPECT_TRUE(world.ScriptAnimationFeedback().front().at("motion").at("speedMatching").at("enabled").get<bool>());
    first.erase("matchSpeed");
    world.ApplyScriptAnimationRequests(actor, Json::array({first}));
    world.TickScriptAnimations(1.0f / 60);
    EXPECT_FALSE(world.ScriptAnimationFeedback().front().at("motion").at("speedMatching").at("enabled").get<bool>());
    first["footPlacement"] = true;
    second["matchSpeed"] = false;
    second["footPlacement"] = "true";
    EXPECT_THROW(world.ApplyScriptAnimationRequests(actor, Json::array({first, second})), std::invalid_argument);
    world.TickScriptAnimations(1.0f / 60);
    for (const auto& feedback : world.ScriptAnimationFeedback())
        EXPECT_FALSE(feedback.at("footPlacement").at("enabled").get<bool>());
    first["footPlacement"] = false;
    second["footPlacement"] = false;
    // 非法 footLock 不得提前发布前一个组件的足部开关。
    first["footPlacement"] = true;
    second["footLock"] = "Sideways";
    EXPECT_THROW(world.ApplyScriptAnimationRequests(actor, Json::array({first, second})), std::invalid_argument);
    first["footPlacement"] = false;
    second.erase("footLock");
    for (const char* option : {"inertialization", "rootOffset"})
    {
        first[option] = true;
        second[option] = "true";
        EXPECT_THROW(world.ApplyScriptAnimationRequests(actor, Json::array({first, second})),
            std::invalid_argument);
        for (const auto& feedback : world.ScriptAnimationFeedback())
        {
            EXPECT_FALSE(feedback.at("inertialization").get<bool>());
            EXPECT_FALSE(feedback.at("rootOffset").at("enabled").get<bool>());
        }
        second.erase(option);
        world.ApplyScriptAnimationRequests(actor, Json::array({first}));
        const auto enabled = world.ScriptAnimationFeedback().front();
        if (std::string(option) == "inertialization")
            EXPECT_TRUE(enabled.at("inertialization").get<bool>());
        else EXPECT_TRUE(enabled.at("rootOffset").at("enabled").get<bool>());
        first.erase(option);
        world.ApplyScriptAnimationRequests(actor, Json::array({first}));
        for (const auto& feedback : world.ScriptAnimationFeedback())
        {
            EXPECT_FALSE(feedback.at("inertialization").get<bool>());
            EXPECT_FALSE(feedback.at("rootOffset").at("enabled").get<bool>());
        }
    }
}

#ifdef VULKANLEARN_ENABLE_CSHARP_BLUEPRINTS
class ScriptAnimationTest : public testing::Test
{
protected:
    void SetUp() override
    {
        runtime.Initialize(true, VL_SCRIPT_OUTPUT_DIRECTORY, std::string(VL_SCRIPT_OUTPUT_DIRECTORY) + "/ScriptFixtures.dll");
        world = std::make_shared<World>(1, "animation");
        world->SetCamera(std::make_shared<Camera>());
    }
    void TearDown() override { runtime.Shutdown(); }
    void Prepare(Json actors, bool formal = false, bool montage = false)
    {
        if (formal || montage) runtime.SetResourceRoot(FormalResourceRoot().string());
        for (const auto& actor : actors)
            for (const auto& node : actor.at("components"))
                if (node.at("type") == "mesh")
                {
                    WorldMeshObject mesh;
                    mesh.sceneObjectIdentity = ScriptMeshIdentity(actor.at("id"), node);
                    mesh.animation = montage ? FormalMontagePlayer() : formal ? FormalPlayer() : TestPlayer();
                    mesh.skinPose = mesh.animation->Pose(0);
                    world->AddMeshObject(mesh.sceneObjectIdentity, mesh);
                }
        runtime.PrepareWorld(*world, {{"objects", actors}});
        runtime.SynchronizeWorld(world);
    }
    void PrepareMotion(const std::string& mode = "", const std::string& type = "ScriptFixtures.MotionMatchingProbe",
        int ordinaryComponents = 0, bool wall = false, float initialYaw = 0, float turnSpeed = 540,
        bool matchSpeed = true, bool footPlacement = false, float slopeDegrees = 0, bool stairs = false,
        bool inertialization = false, bool rootOffset = false, const std::string& footLock = "Auto")
    {
        auto actor = Actor("owner", type);
        if (!mode.empty()) actor["properties"]["Mode"] = mode;
        actor["position"] = {0, 90, 0};
        actor["rotation"] = {0, initialYaw, 0};
        actor["components"][0]["position"] = {0, -90, 0};
        actor["components"][0]["rotation"] = {0, 180, 0};
        actor["physics"] = {{"motion", "character"}, {"shape", "capsule"}, {"radius", 30}, {"halfHeight", 60}};
        if (type == "Gasp.CharacterBlueprint")
        {
            actor["properties"]["TurnSpeed"] = turnSpeed;
            actor["properties"]["MatchSpeed"] = matchSpeed;
            actor["properties"]["FootPlacement"] = footPlacement;
            actor["properties"]["FootLock"] = footLock;
            actor["properties"]["Inertialization"] = inertialization;
            actor["properties"]["RootOffset"] = rootOffset;
            // 正式控制器要求 SpringArm；直接消费正式 BP 组件，避免夹具漏接父链仍假通过。
            actor["components"] = ReadResourceJson(FormalResourceRoot(),
                "Maps/SC_gasp/Blueprints/BP_gasp_character.json").at("components");
        }
        auto floor = Actor("floor", "ScriptFixtures.PassiveBlueprint");
        floor["components"] = Json::array(); floor["position"] = {0, -50, 0};
        floor["physics"] = {{"shape", "box"}, {"halfExtents", {10000, 50, 10000}}};
        if (slopeDegrees != 0)
        {
            const float radians = slopeDegrees * 3.14159265358979323846f / 180;
            floor["rotation"] = {0, 0, slopeDegrees};
            floor["position"] = {50 * std::sin(radians), -50 * std::cos(radians), 0};
            floor["physics"]["friction"] = 1;
        }
        auto actors = Json::array();
        for (int index = 0; index < ordinaryComponents; ++index)
        {
            auto ordinary = Actor("a-ordinary-" + std::to_string(index), "ScriptFixtures.PassiveBlueprint");
            ordinary["position"] = {1000 + index * 100, 0, 0};
            actors.push_back(std::move(ordinary));
        }
        actors.push_back(std::move(actor));
        if (wall)
        {
            auto obstacle = floor;
            obstacle["id"] = obstacle["name"] = "wall";
            obstacle["position"] = {0, 500, -250};
            obstacle["physics"]["halfExtents"] = {1000, 500, 20};
            actors.push_back(std::move(obstacle));
        }
        actors.push_back(std::move(floor));
        if (stairs)
        {
            const std::array<float, 6> heights{10, 20, 30, 20, 10, 0};
            for (size_t index = 0; index < heights.size(); ++index)
            {
                if (heights[index] == 0) continue;
                auto tread = Actor("tread-" + std::to_string(index), "ScriptFixtures.PassiveBlueprint");
                tread["components"] = Json::array();
                tread["position"] = {0, heights[index] * 0.5f, -200.0f - 80 * index};
                tread["physics"] = {{"shape", "box"}, {"halfExtents", {150, heights[index] * 0.5f, 40}}};
                actors.push_back(std::move(tread));
            }
        }
        Prepare(std::move(actors), true);
        Play();
        world->SetMotionDebugEnabled(true);
    }
    void Play()
    {
        std::string error;
        ASSERT_TRUE(runtime.Control(*world, ScriptPlayState::Playing, error)) << error;
    }
    void Tick(const std::set<std::string>& keys = {}, float dt = 1.0f / 60)
    {
        InputActionState input; input.gameplay = map.Sample(keys, true, true);
        runtime.Tick(world, dt, input, diagnostics);
    }
    const Json& State(const std::string& id = "owner")
    {
        for (const auto& state : world->GetScriptBlueprintStates()) if (state.at("id") == id) return state;
        throw std::runtime_error("Missing actor");
    }
    std::shared_ptr<Animation::SkeletalPlayback> Player(const std::string& identity = "__blueprint/owner/visual")
    {
        for (const auto& mesh : world->GetMeshObjects())
            if (mesh.second.sceneObjectIdentity == identity) return mesh.second.animation;
        throw std::runtime_error("Missing player");
    }
    void TraceMotion(Json& trace, const std::string& phase, int frame)
    {
        const auto view = world->CaptureMotionDebug();
        Json sample{{"phase", phase}, {"frame", frame}, {"reason", view->decisionReason},
            {"searchCount", view->searchCount}, {"queryReady", view->queryReady},
            {"clip", Player()->Asset().clips.at(Player()->SelectedClip()).id}, {"time", Player()->Time()},
            {"position", State()["position"]}, {"velocity", State().value("bodyVelocity", Json::array())},
            {"playing", Player()->IsPlaying()}, {"completed", Player()->Completed()}};
        sample["queryUsesAchievedVelocity"] = view->queryUsesAchievedVelocity;
        sample["actorYaw"] = State()["rotation"][1];
        const auto& model = world->GetMeshObjects().at("__blueprint/owner/visual").model;
        sample["modelForward"] = {model(0, 2), model(1, 2), model(2, 2)};
        sample["queryFeatures"] = view->features;
        sample["queryRootPosition"] = {view->queryRoot(0, 3), view->queryRoot(1, 3), view->queryRoot(2, 3)};
        sample["queryRootForward"] = {view->queryRoot(0, 2), view->queryRoot(1, 2), view->queryRoot(2, 2)};
        sample["queryFutureRoots"] = Json::array();
        for (const auto& point : view->queryFutureRoots)
            sample["queryFutureRoots"].push_back({point.x(), point.y(), point.z()});
        if (view->selected)
            sample["selected"] = {{"clip", view->selected->clipId}, {"time", view->selected->time},
                {"trajectory", view->selected->trajectoryCost}, {"pose", view->selected->poseCost},
                {"policy", view->selected->policyCost}, {"total", view->selected->totalCost}};
        trace.push_back(std::move(sample));
    }
    ScriptRuntimeSubsystem runtime;
    std::shared_ptr<World> world;
    DiagnosticsSubsystem diagnostics;
    GameplayInputMap map;
};

TEST_F(ScriptAnimationTest, StateMachineUsesPriorityStableTiesAndOnlyOneTransition)
{
    auto actor = Actor("owner"); actor["properties"]["Mode"] = "machine";
    Prepare(Json::array({actor})); Play();
    EXPECT_EQ(State()["position"][0], 3); // Wednesday，不能链跳到 Friday 或覆盖为 Thursday。
}

TEST_F(ScriptAnimationTest, NativeClockUsesScaledTimeAndPauseFreezesPose)
{
    Prepare(Json::array({Actor("owner")})); Play();
    std::string error;
    ASSERT_TRUE(runtime.SetTimeScale(0.5f, error));
    Tick({}, 0.2f);
    EXPECT_NEAR(Player()->Time(), 0.2f, 1e-5f); // rate=2，timescale=0.5。
    const auto pose = Player()->Pose(0);
    ASSERT_TRUE(runtime.Control(*world, ScriptPlayState::Paused, error));
    Tick({}, 1);
    EXPECT_EQ(Player()->Pose(0), pose);
    ASSERT_TRUE(runtime.Control(*world, ScriptPlayState::Playing, error));
    Tick({}, 0.1f);
    EXPECT_NEAR(Player()->Time(), 0.3f, 1e-5f);
}

TEST_F(ScriptAnimationTest, ScriptTimelineControlsOnlyTheFocusedPlayback)
{
    Prepare(Json::array({Actor("owner")}));
    Play();
    Tick({}, 0.2f);
    auto owner = Player();
    auto extra = owner->NewInstance();
    WorldMeshObject object;
    object.sceneObjectIdentity = "extra/visual";
    object.animation = extra;
    object.skinPose = extra->Pose(0);
    world->AddMeshObject("extra/visual", object);
    const float extraTime = extra->Time();
    ASSERT_TRUE(world->ControlScriptAnimation(world->GetGeneration(),
        "__blueprint/owner/visual", 0, 0.5f));
    EXPECT_FLOAT_EQ(owner->Time(), 0.5f);
    EXPECT_FALSE(owner->IsPlaying());
    EXPECT_FLOAT_EQ(extra->Time(), extraTime);
    EXPECT_TRUE(extra->IsPlaying());
    EXPECT_FALSE(world->ControlScriptAnimation(world->GetGeneration(), "missing/visual", 0, 0.5f));
    EXPECT_FALSE(world->ControlScriptAnimation(world->GetGeneration() + 1,
        "__blueprint/owner/visual", 0, 0.5f));
    EXPECT_FALSE(world->ControlScriptAnimation(world->GetGeneration(),
        "__blueprint/owner/visual", 0, owner->Duration() + 1));
    EXPECT_FALSE(world->ControlScriptAnimation(world->GetGeneration(),
        "__blueprint/owner/visual", 2, 0));
    ASSERT_TRUE(world->ControlScriptAnimation(world->GetGeneration(),
        "__blueprint/owner/visual", 1, 1.0f));
    EXPECT_TRUE(owner->IsPlaying());
    EXPECT_FLOAT_EQ(owner->Time(), 0.5f);
    EXPECT_FLOAT_EQ(extra->Time(), extraTime);
}

TEST_F(ScriptAnimationTest, StopRecreatesClockAndClearsSubmittedPoseIdentity)
{
    Prepare(Json::array({Actor("owner")})); Play(); Tick({}, 0.2f);
    const auto old = Player()->Pose(0);
    std::string error;
    ASSERT_TRUE(runtime.Control(*world, ScriptPlayState::Editing, error));
    EXPECT_FLOAT_EQ(Player()->Time(), 0);
    EXPECT_NE(Player()->Pose(0)->playbackIdentity, old->playbackIdentity);
    Play(); Tick({}, 0.1f);
    EXPECT_NEAR(Player()->Time(), 0.2f, 1e-5f);
}

TEST_F(ScriptAnimationTest, FeedbackReturnsNativeClockOnFollowingCallback)
{
    auto actor = Actor("owner"); actor["properties"]["Mode"] = "feedback";
    Prepare(Json::array({actor})); Play(); Tick({}, 0.1f); Tick({}, 0.1f);
    EXPECT_NEAR(State()["position"][0].get<float>(), 0.2f, 1e-5f);
    EXPECT_NEAR(Player()->Time(), 0.4f, 1e-5f);
}

TEST_F(ScriptAnimationTest, CallbackExceptionRollsBackPendingClipRequest)
{
    auto actor = Actor("owner"); actor["properties"]["Mode"] = "throw";
    Prepare(Json::array({actor})); Play(); Tick();
    EXPECT_TRUE(State()["failed"]);
    EXPECT_EQ(Player()->Asset().clips[Player()->SelectedClip()].id, "walk");
}

TEST_F(ScriptAnimationTest, UnknownClipFailsOnlyRequestingOwner)
{
    auto actor = Actor("owner"); actor["properties"]["Mode"] = "invalid";
    Prepare(Json::array({actor, Actor("other")})); Play(); Tick();
    EXPECT_TRUE(State()["failed"]);
    EXPECT_FALSE(State("other")["failed"]);
    EXPECT_GT(Player("__blueprint/other/visual")->Time(), 0);
}

TEST_F(ScriptAnimationTest, OnCreateCannotStartGameplayAnimation)
{
    auto actor = Actor("owner"); actor["properties"]["Mode"] = "create";
    EXPECT_THROW(Prepare(Json::array({actor})), std::exception);
}

TEST_F(ScriptAnimationTest, RemovedComponentRejectsOldReferenceAndKeepsSiblingClock)
{
    auto actor = Actor("owner"); actor["properties"]["Mode"] = "remove";
    actor["components"].push_back({{"id", "extra"}, {"type", "mesh"},
        {"modelPath", "Maps/SC_gasp/Meshes/SM_mannequin.json"}});
    Prepare(Json::array({actor})); Play(); auto player = Player(); Tick();
    EXPECT_EQ(Player(), player);
    Tick();
    EXPECT_TRUE(State()["failed"]);
    EXPECT_EQ(world->GetMeshObjects().size(), 1u);
}

TEST_F(ScriptAnimationTest, LiveReloadRejectsBeforeReplacingSkeletalController)
{
    Prepare(Json::array({Actor("owner")})); Play(); Tick();
    auto context = world->GetScriptContext();
    auto player = Player();
    std::string error;
    EXPECT_FALSE(runtime.ReloadAssembly(*world, error));
    EXPECT_NE(error.find("Stop gameplay"), std::string::npos);
    EXPECT_EQ(world->GetScriptContext(), context);
    EXPECT_EQ(Player(), player);
    Tick();
    EXPECT_FALSE(State()["failed"]);
}

TEST_F(ScriptAnimationTest, LegacyLocomotionMovesRunsJumpsFallsAndLandsUsingJoltFeedback)
{
    auto actor = Actor("player", "ScriptFixtures.LegacyLocomotionFixture");
    actor["position"] = {0, 90, 0};
    actor["physics"] = {{"motion", "character"}, {"shape", "capsule"}, {"radius", 30}, {"halfHeight", 60}};
    actor["components"].push_back({{"id", "camera"}, {"type", "camera"}});
    auto floor = Actor("floor", "ScriptFixtures.PassiveBlueprint");
    floor["components"] = Json::array();
    floor["position"] = {0, -50, 0};
    floor["physics"] = {{"shape", "box"}, {"halfExtents", {2000, 50, 2000}}};
    Prepare(Json::array({actor, floor})); Play(); Tick();
    auto player = Player("__blueprint/player/visual");
    EXPECT_EQ(player->Asset().clips[player->SelectedClip()].id, "M_Neutral_Stand_Idle_Loop");
    Tick({"W"});
    EXPECT_EQ(player->Asset().clips[player->SelectedClip()].id, "M_Neutral_Walk_Loop_F");
    Tick({"W", "Left Shift"});
    EXPECT_EQ(player->Asset().clips[player->SelectedClip()].id, "M_Neutral_Run_Loop_F");
    Tick({"Space"});
    EXPECT_EQ(player->Asset().clips[player->SelectedClip()].id, "M_Neutral_Jump_F_Start_Stand_Lfoot");
    bool falling = false, landed = false;
    for (int frame = 0; frame < 140; ++frame)
    {
        Tick();
        const auto clip = player->Asset().clips[player->SelectedClip()].id;
        falling |= clip == "M_Neutral_Jump_Loop_Fall";
        landed |= clip == "M_Neutral_Jump_F_Land_Stand_Light_Lfoot";
    }
    ASSERT_FALSE(State("player")["failed"]);
    EXPECT_TRUE(falling);
    EXPECT_TRUE(landed);
    EXPECT_EQ(player->Asset().clips[player->SelectedClip()].id, "M_Neutral_Stand_Idle_Loop");
    EXPECT_NEAR(State("player")["position"][1].get<float>(), 90, 1);
    EXPECT_LT(State("player")["position"][2].get<float>(), -5);
    EXPECT_TRUE(player->Pose(0)->matrices[0].isIdentity(0.001f));
}

TEST_F(ScriptAnimationTest, LegacyLocomotionStopsWalkAnimationWhenBlockedByWall)
{
    auto actor = Actor("player", "ScriptFixtures.LegacyLocomotionFixture");
    actor["position"] = {0, 90, 0};
    actor["physics"] = {{"motion", "character"}, {"shape", "capsule"}, {"radius", 30}, {"halfHeight", 60}};
    actor["components"].push_back({{"id", "camera"}, {"type", "camera"}});
    auto floor = Actor("floor", "ScriptFixtures.PassiveBlueprint");
    floor["components"] = Json::array(); floor["position"] = {0, -50, 0};
    floor["physics"] = {{"shape", "box"}, {"halfExtents", {2000, 50, 2000}}};
    auto wall = floor; wall["id"] = wall["name"] = "wall";
    wall["position"] = {0, 500, -250};
    wall["physics"]["halfExtents"] = {1000, 500, 20};
    Prepare(Json::array({actor, floor, wall})); Play(); Tick();
    for (int frame = 0; frame < 120; ++frame) Tick({"W"});
    ASSERT_FALSE(State("player")["failed"]);
    EXPECT_GT(State("player")["position"][2].get<float>(), -205);
    auto player = Player("__blueprint/player/visual");
    EXPECT_EQ(player->Asset().clips[player->SelectedClip()].id, "M_Neutral_Stand_Idle_Loop");
}

TEST_F(ScriptAnimationTest, ActualMotionHistoryAndQueryFreezeOnPauseAndResetOnStop)
{
    auto actor = Actor("player", "ScriptFixtures.LegacyLocomotionFixture");
    actor["position"] = {0, 90, 0};
    actor["physics"] = {{"motion", "character"}, {"shape", "capsule"}, {"radius", 30}, {"halfHeight", 60}};
    actor["components"].push_back({{"id", "camera"}, {"type", "camera"}});
    auto floor = Actor("floor", "ScriptFixtures.PassiveBlueprint");
    floor["components"] = Json::array(); floor["position"] = {0, -50, 0};
    floor["physics"] = {{"shape", "box"}, {"halfExtents", {2000, 50, 2000}}};
    Prepare(Json::array({actor, floor})); Play();
    world->SetMotionDebugEnabled(true);
    for (int frame = 0; frame < 40; ++frame) Tick({"W"});
    auto captured = world->CaptureMotionDebug();
    ASSERT_EQ(captured->features.size(), 30u); // 最小夹具只有 root，正式 GASP 为 42 维。
    EXPECT_NEAR(std::abs(captured->features[29]), 200, 0.1);
    EXPECT_NE(captured->summary.find("Query ready"), std::string::npos);
    ASSERT_FALSE(captured->lines.empty());
    const auto savedPosition = captured->lines.front().begin;
    std::string error;
    ASSERT_TRUE(runtime.Control(*world, ScriptPlayState::Paused, error));
    Tick({"W"}, 0.2f);
    const auto paused = world->CaptureMotionDebug();
    EXPECT_EQ(paused->features, captured->features);
    EXPECT_EQ(paused->summary, captured->summary);
    EXPECT_TRUE(paused->lines.front().begin.isApprox(savedPosition));
    ASSERT_TRUE(runtime.Control(*world, ScriptPlayState::Editing, error));
    EXPECT_TRUE(world->CaptureMotionDebug()->features.empty());
    EXPECT_TRUE(world->CaptureMotionDebug()->lines.empty());
    EXPECT_TRUE(captured->lines.front().begin.isApprox(savedPosition));
}

TEST_F(ScriptAnimationTest, CollisionHistoryUsesAchievedPositionWhilePredictionRemainsApproximate)
{
    auto actor = Actor("player", "ScriptFixtures.LegacyLocomotionFixture");
    actor["position"] = {0, 90, 0};
    actor["physics"] = {{"motion", "character"}, {"shape", "capsule"}, {"radius", 30}, {"halfHeight", 60}};
    actor["components"].push_back({{"id", "camera"}, {"type", "camera"}});
    auto floor = Actor("floor", "ScriptFixtures.PassiveBlueprint");
    floor["components"] = Json::array(); floor["position"] = {0, -50, 0};
    floor["physics"] = {{"shape", "box"}, {"halfExtents", {2000, 50, 2000}}};
    auto wall = floor; wall["id"] = wall["name"] = "wall";
    wall["position"] = {0, 500, -250};
    wall["physics"]["halfExtents"] = {1000, 500, 20};
    Prepare(Json::array({actor, floor, wall})); Play();
    world->SetMotionDebugEnabled(true);
    for (int frame = 0; frame < 120; ++frame) Tick({"W"});
    const auto captured = world->CaptureMotionDebug();
    ASSERT_EQ(captured->features.size(), 30u);
    EXPECT_NEAR(captured->features[29], 0, 0.2);
    bool actual = false, predictedThroughWall = false;
    for (const auto& line : captured->lines)
    {
        if (line.color[0] == 0)
        {
            actual = true;
            EXPECT_GT(line.begin.z(), -205);
            EXPECT_GT(line.end.z(), -205);
        }
        if (line.color[0] == 1 && line.end.z() < -300) predictedThroughWall = true;
    }
    EXPECT_TRUE(actual);
    EXPECT_TRUE(predictedThroughWall);
}

TEST_F(ScriptAnimationTest, MotionMatchingBindsFormalStatisticsAndFreezesPublishedSearchValues)
{
    PrepareMotion("move-then-idle");
    auto request = MotionRequest("visual", Json::array({"M_Neutral_Stand_Idle_Loop",
        "M_Neutral_Walk_Loop_F", "M_Neutral_Walk_Start_F_Lfoot", "M_Neutral_Walk_Stop_F_Lfoot",
        "M_Neutral_Stand_Turn_180_L", "M_Neutral_Stand_Turn_180_R"}));
    request["footPlacement"] = true;
    world->ApplyScriptAnimationRequests(State(), Json::array({request}));
    Json trace = Json::array();
    for (int frame = 0; frame < 45; ++frame)
    {
        Tick();
        TraceMotion(trace, "warm-up", frame);
        trace.back()["epoch"] = Player()->Pose(0)->discontinuity;
        trace.back()["footReason"] = Player()->FootPlacement().reason;
        trace.back()["inPlace"] = Player()->InPlace();
    }
    ASSERT_FALSE(State()["failed"]);
    ASSERT_TRUE(Player()->FootPlacement().enabled);
    const auto view = world->CaptureMotionDebug();
    ASSERT_TRUE(view->queryReady) << view->summary << "\n" << trace.dump();
    ASSERT_EQ(view->features.size(), 42u);
    ASSERT_EQ(view->normalizedQuery.size(), 42u);
    EXPECT_EQ(view->channels.size(), 42u);
    EXPECT_EQ(view->databaseIdentity.size(), 64u);
    EXPECT_EQ(view->payloadIdentity.size(), 64u);
    EXPECT_GT(view->searchCount, 0u);
    ASSERT_TRUE(view->selected);
    ASSERT_FALSE(view->topCandidates.empty());
    EXPECT_LE(view->topCandidates.size(), 5u);
    for (const auto& candidate : view->topCandidates)
        EXPECT_NEAR(candidate.totalCost,
            candidate.trajectoryCost + candidate.poseCost + candidate.policyCost, 1e-6);
    ASSERT_FALSE(view->queryFutureRoots.empty());
    const float beforeIntervalZ = State()["position"][2];
    Tick();
    const auto interval = world->CaptureMotionDebug();
    ASSERT_EQ(interval->searchCount, view->searchCount);
    EXPECT_LT(State()["position"][2].get<float>(), beforeIntervalZ);
    EXPECT_EQ(interval->features, view->features);
    EXPECT_TRUE(interval->queryRoot.isApprox(view->queryRoot, 0));
    EXPECT_EQ(interval->queryUsesAchievedVelocity, view->queryUsesAchievedVelocity);
    ASSERT_EQ(interval->queryFutureRoots.size(), view->queryFutureRoots.size());
    for (size_t index = 0; index < view->queryFutureRoots.size(); ++index)
        EXPECT_TRUE(interval->queryFutureRoots[index].isApprox(view->queryFutureRoots[index], 0));
    // 持续移动的摆腿不保证进入锁定；先真实停止并完成 Idle 混合，再验收带锁定的 Pause/Stop。
    for (int frame = 0; frame < 180; ++frame)
    {
        Tick();
        TraceMotion(trace, "settle-idle", frame);
        trace.back()["footReason"] = Player()->FootPlacement().reason;
    }
    ASSERT_FALSE(State()["failed"]);
    ASSERT_TRUE(State().at("grounded").get<bool>());
    const auto& velocity = State().at("bodyVelocity");
    EXPECT_LT(Eigen::Vector2f(velocity[0].get<float>(), velocity[2].get<float>()).norm(), 2);
    ASSERT_EQ(Player()->Asset().clips.at(Player()->SelectedClip()).id, "M_Neutral_Stand_Idle_Loop");
    ASSERT_FALSE(Player()->IsBlending());
    ASSERT_TRUE(Player()->FootPlacement().active) << trace.dump();
    const auto settled = world->CaptureMotionDebug();
    ASSERT_TRUE(settled->queryReady);
    const auto pose = Player()->Pose(0);
    const auto footPlacement = world->ScriptAnimationFeedback().front().at("footPlacement");
    std::string error;
    ASSERT_TRUE(runtime.Control(*world, ScriptPlayState::Paused, error));
    Tick({}, 0.2f);
    EXPECT_EQ(Player()->Pose(0), pose);
    EXPECT_EQ(world->ScriptAnimationFeedback().front().at("footPlacement"), footPlacement);
    const auto paused = world->CaptureMotionDebug();
    EXPECT_EQ(paused->features, settled->features);
    EXPECT_EQ(paused->searchCount, settled->searchCount);
    EXPECT_EQ(paused->summary, settled->summary);
    EXPECT_TRUE(paused->queryRoot.isApprox(settled->queryRoot, 0));
    EXPECT_EQ(paused->queryUsesAchievedVelocity, settled->queryUsesAchievedVelocity);
    ASSERT_EQ(paused->queryFutureRoots.size(), settled->queryFutureRoots.size());
    for (size_t index = 0; index < settled->queryFutureRoots.size(); ++index)
        EXPECT_TRUE(paused->queryFutureRoots[index].isApprox(settled->queryFutureRoots[index], 0));
    ASSERT_TRUE(runtime.Control(*world, ScriptPlayState::Playing, error));
    ASSERT_TRUE(runtime.SetTimeScale(0, error));
    Tick({}, 0.2f);
    EXPECT_EQ(Player()->Pose(0), pose);
    EXPECT_EQ(world->ScriptAnimationFeedback().front().at("footPlacement"), footPlacement);
    EXPECT_EQ(world->CaptureMotionDebug()->searchCount, settled->searchCount);
    EXPECT_EQ(world->CaptureMotionDebug()->features, settled->features);
    ASSERT_TRUE(runtime.Control(*world, ScriptPlayState::Editing, error));
    EXPECT_TRUE(world->CaptureMotionDebug()->databaseIdentity.empty());
    EXPECT_TRUE(world->CaptureMotionDebug()->features.empty());
    EXPECT_TRUE(world->CaptureMotionDebug()->queryFutureRoots.empty());
    EXPECT_FALSE(world->CaptureMotionDebug()->queryUsesAchievedVelocity);
    const auto& stoppedFeet = Player()->FootPlacement();
    EXPECT_FALSE(stoppedFeet.enabled);
    EXPECT_FALSE(stoppedFeet.active);
    EXPECT_FLOAT_EQ(stoppedFeet.pelvisOffset, 0);
    for (const auto& foot : stoppedFeet.feet)
    {
        EXPECT_FALSE(foot.planted);
        EXPECT_TRUE(foot.supportIdentity.empty());
        EXPECT_TRUE(foot.anchor.isZero(0));
    }
    EXPECT_FALSE(view->features.empty()); // 老快照保留值，不回读已重置的实例。
}

TEST_F(ScriptAnimationTest, EmptyMotionFilterFreezesAndRestoringFilterCannotKeepForbiddenClip)
{
    PrepareMotion();
    for (int frame = 0; frame < 45; ++frame) Tick();
    ASSERT_FALSE(State()["failed"]);
    world->ApplyScriptAnimationRequests(State(), Json::array({MotionRequest("visual", Json::array())}));
    const auto pose = Player()->Pose(0);
    for (int frame = 0; frame < 10; ++frame) Tick();
    const auto empty = world->CaptureMotionDebug();
    EXPECT_FALSE(empty->selected);
    EXPECT_EQ(empty->decisionReason, "no-allowed-candidates");
    EXPECT_EQ(Player()->Pose(0), pose);
    world->ApplyScriptAnimationRequests(State(), Json::array({
        MotionRequest("visual", Json::array({"M_Neutral_Walk_Loop_F"}))}));
    Tick();
    const auto restored = world->CaptureMotionDebug();
    ASSERT_TRUE(restored->selected);
    EXPECT_EQ(restored->selected->clipId, "M_Neutral_Walk_Loop_F");
    for (const auto& candidate : restored->topCandidates)
        EXPECT_EQ(candidate.clipId, "M_Neutral_Walk_Loop_F");
}

TEST_F(ScriptAnimationTest, FailedCallbackRollsBackMotionFilterAndClearsOwnerSearchState)
{
    PrepareMotion("throw");
    Tick();
    EXPECT_TRUE(State()["failed"]);
    EXPECT_TRUE(world->CaptureMotionDebug()->databaseIdentity.empty());
    EXPECT_TRUE(world->CaptureMotionDebug()->features.empty());
}

TEST_F(ScriptAnimationTest, TeleportClearsMotionQueryHistoryAndOldTransition)
{
    PrepareMotion();
    Json trace = Json::array();
    for (int frame = 0; frame < 45; ++frame)
    {
        Tick();
        TraceMotion(trace, "warm-up", frame);
        trace.back()["epoch"] = Player()->Pose(0)->discontinuity;
        trace.back()["inPlace"] = Player()->InPlace();
    }
    ASSERT_TRUE(world->CaptureMotionDebug()->queryReady) << world->CaptureMotionDebug()->summary << "\n" << trace.dump();
    auto states = world->GetScriptBlueprintStates();
    for (auto& actor : states)
        if (actor.at("id") == "owner")
        {
            actor["position"][0] = actor["position"][0].get<float>() + 3000;
            world->ApplyScriptBlueprintVisuals(actor);
        }
    world->SetScriptBlueprintStates(states);
    world->TickScriptAnimations(1.0f / 60);
    const auto view = world->CaptureMotionDebug();
    EXPECT_FALSE(view->queryReady);
    EXPECT_TRUE(view->features.empty());
    EXPECT_TRUE(view->queryFutureRoots.empty());
    EXPECT_TRUE(view->queryRoot.isIdentity());
    EXPECT_FALSE(view->queryUsesAchievedVelocity);
    EXPECT_FALSE(Player()->IsBlending());
}

TEST_F(ScriptAnimationTest, LockedNonloopTailReachesCompletionAndFilterCanRevokeIt)
{
    PrepareMotion();
    for (int frame = 0; frame < 45; ++frame) Tick();
    auto request = MotionRequest("visual", Json::array({"M_Neutral_Jump_F_Land_Stand_Light_Lfoot"}));
    request["actionLocked"] = true;
    world->ApplyScriptAnimationRequests(State(), Json::array({request}));
    for (int frame = 0; frame < 150 && !Player()->Completed(); ++frame) Tick();
    EXPECT_TRUE(Player()->Completed());
    EXPECT_EQ(world->CaptureMotionDebug()->decisionReason, "action-locked-tail");
    EXPECT_TRUE(world->CaptureMotionDebug()->queryFutureRoots.empty());
    EXPECT_FALSE(world->CaptureMotionDebug()->queryUsesAchievedVelocity);
    world->ApplyScriptAnimationRequests(State(), Json::array({
        MotionRequest("visual", Json::array({"M_Neutral_Stand_Idle_Loop"}))}));
    Tick();
    ASSERT_TRUE(world->CaptureMotionDebug()->selected);
    EXPECT_EQ(world->CaptureMotionDebug()->selected->clipId, "M_Neutral_Stand_Idle_Loop");
}

TEST_F(ScriptAnimationTest, MotionHistoryBudgetAndDebugFocusPreferMatchingComponent)
{
    PrepareMotion("", "ScriptFixtures.MotionMatchingProbe", 9);
    for (int frame = 0; frame < 45; ++frame) Tick();
    ASSERT_FALSE(State()["failed"]);
    const auto view = world->CaptureMotionDebug();
    EXPECT_EQ(view->componentIdentity, "__blueprint/owner/visual");
    EXPECT_TRUE(view->queryReady);
    EXPECT_EQ(view->features.size(), 42u);
    EXPECT_GT(view->searchCount, 0u);
}

TEST_F(ScriptAnimationTest, ConsoleAnimationPauseDoesNotResumeMatcherOrPlayerOnGameplayTick)
{
    PrepareMotion();
    for (int frame = 0; frame < 45; ++frame) Tick();
    const auto view = world->CaptureMotionDebug();
    ASSERT_TRUE(world->ControlAnimation(world->GetGeneration(), "play", 0));
    const auto pose = Player()->Pose(0);
    for (int frame = 0; frame < 10; ++frame) Tick();
    EXPECT_EQ(Player()->Pose(0), pose);
    EXPECT_FALSE(Player()->IsPlaying());
    EXPECT_EQ(world->CaptureMotionDebug()->searchCount, view->searchCount);
    EXPECT_EQ(world->CaptureMotionDebug()->features, view->features);

    ASSERT_TRUE(world->ControlAnimation(world->GetGeneration(), "play", 1));
    auto request = MotionRequest("visual", Json::array({"M_Neutral_Jump_F_Land_Stand_Light_Lfoot"}));
    request["actionLocked"] = true;
    world->ApplyScriptAnimationRequests(State(), Json::array({request}));
    for (int frame = 0; frame < 150 && !Player()->Completed(); ++frame) Tick();
    ASSERT_TRUE(Player()->Completed());
    ASSERT_FALSE(Player()->IsExplicitlyPaused());
    ASSERT_TRUE(world->ControlAnimation(world->GetGeneration(), "play", 0));
    const auto completedPose = Player()->Pose(0);
    const float completedTime = Player()->Time();
    const auto completedView = world->CaptureMotionDebug();
    world->ApplyScriptAnimationRequests(State(), Json::array({
        MotionRequest("visual", Json::array({"M_Neutral_Stand_Idle_Loop"}))}));
    for (int frame = 0; frame < 10; ++frame) Tick();
    EXPECT_TRUE(Player()->Completed());
    EXPECT_TRUE(Player()->IsExplicitlyPaused());
    EXPECT_FALSE(Player()->IsPlaying());
    EXPECT_FLOAT_EQ(Player()->Time(), completedTime);
    EXPECT_EQ(Player()->Pose(0), completedPose);
    EXPECT_EQ(world->CaptureMotionDebug()->searchCount, completedView->searchCount);
    EXPECT_EQ(world->CaptureMotionDebug()->features, completedView->features);
    ASSERT_TRUE(world->ControlAnimation(world->GetGeneration(), "play", 1));
    Tick();
    const auto resumed = world->CaptureMotionDebug();
    ASSERT_TRUE(resumed->selected);
    EXPECT_EQ(resumed->selected->clipId, "M_Neutral_Stand_Idle_Loop");
    EXPECT_GT(resumed->searchCount, completedView->searchCount);
    EXPECT_FALSE(Player()->IsExplicitlyPaused());
    EXPECT_TRUE(Player()->IsPlaying());
    EXPECT_NE(Player()->Pose(0), completedPose);
}

class ScriptAnimationRateTest : public ScriptAnimationTest, public testing::WithParamInterface<int>
{
protected:
    int Frames(double seconds) const { return static_cast<int>(std::lround(seconds * GetParam())); }
    void Tick(const std::set<std::string>& keys = {})
    {
        ScriptAnimationTest::Tick(keys, 1.0f / GetParam());
    }
    void TraceMotion(Json& trace, const std::string& phase, int frame)
    {
        ScriptAnimationTest::TraceMotion(trace, phase, frame);
        trace.back()["rate"] = GetParam();
        trace.back()["dt"] = 1.0f / GetParam();
    }
    float Yaw() { return State().at("rotation").at(1).get<float>(); }
    Eigen::Matrix4f Model() { return world->GetMeshObjects().at("__blueprint/owner/visual").model; }
    static float AngleDelta(float before, float after) { return std::remainder(after - before, 360.0f); }
    static float Heading(const Eigen::Matrix4f& matrix)
    {
        return std::atan2(matrix(0, 2), matrix(2, 2)) * 180 / 3.14159265358979323846f;
    }
    Eigen::Vector3f JointPosition(const char* name)
    {
        const auto player = Player();
        const auto& joints = player->Asset().mesh.joints;
        for (size_t index = 0; index < joints.size(); ++index)
            if (joints[index].name == name)
                return (Model() * player->Models().at(index)).block<3, 1>(0, 3);
        throw std::runtime_error("Missing formal GASP joint");
    }
    void WarmFormal()
    {
        PrepareMotion("", "Gasp.CharacterBlueprint");
        for (int frame = 0; frame < Frames(1); ++frame) Tick({"W"});
    }
    void ExpectBoundedTurn(const std::set<std::string>& keys, float target, float degrees)
    {
        Json trace = Json::array();
        const Eigen::Vector3f cameraForward = world->GetRenderCamera()->GetForwardVector();
        float travelled = 0;
        int reached = -1;
        for (int frame = 0; frame < Frames(0.6); ++frame)
        {
            const float previousYaw = Yaw(), previousHeading = Heading(Model());
            const auto previousSearch = world->CaptureMotionDebug()->searchCount;
            Tick(keys);
            const float delta = AngleDelta(previousYaw, Yaw());
            travelled += std::abs(delta);
            EXPECT_LE(std::abs(delta), 540.0f / GetParam() + 0.002f) << frame;
            EXPECT_NEAR(AngleDelta(previousHeading, Heading(Model())), delta, 0.002f) << frame;
            EXPECT_LE(travelled, degrees + 0.01f);
            if (degrees == 180) EXPECT_GE(delta, -0.002f); // 半圈固定正向，不随 atan2 的正负零改变。
            if (std::abs(AngleDelta(Yaw(), target)) < 0.002f && reached < 0) reached = frame + 1;
            // 无 Look 输入时检查真实父链的世界镜头方向，而不是只检查 localYaw。
            EXPECT_NEAR(AngleDelta(0, Heading(ScriptComponentMatrix(State(), 2))), 0, 0.002f);
            EXPECT_TRUE(world->GetRenderCamera()->GetForwardVector().isApprox(cameraForward, 0.0001f));
            const auto view = world->CaptureMotionDebug();
            if (view->queryReady && view->searchCount != previousSearch)
                EXPECT_NEAR(AngleDelta(Heading(Model() * Player()->Models().front()), Heading(view->queryRoot)), 0, 0.002f);
            TraceMotion(trace, "bounded-turn", frame);
            const auto pelvis = JointPosition("pelvis");
            trace.back()["pelvisWorld"] = {pelvis.x(), pelvis.y(), pelvis.z()};
        }
        ASSERT_GE(reached, 0);
        EXPECT_NEAR(static_cast<float>(reached) / GetParam(), degrees / 540, 1.0f / GetParam() + 0.0001f);
        EXPECT_NEAR(AngleDelta(Yaw(), target), 0, 0.002f);
        EXPECT_FALSE(State()["failed"]);
        RecordProperty("turnTrace", trace.dump());
    }
    void AuthoritativeTeleport(float yaw, float positionX = 3000)
    {
        auto states = world->GetScriptBlueprintStates();
        for (auto& actor : states)
            if (actor.at("id") == "owner")
            {
                actor["rotation"] = {0, yaw, 0};
                actor["position"][0] = positionX;
                world->ApplyScriptBlueprintVisuals(actor);
                // 走生产 Tick 的物理反馈入口，令真实 CLR 同步新根；不缓存或反射改私有 heading。
                world->GetScriptContext()->physicsUpdates.push_back(actor);
            }
        world->SetScriptBlueprintStates(std::move(states));
    }
};

TEST_P(ScriptAnimationRateTest, FormalGaspNinetyDegreeTurnBoundsActorModelAndCamera)
{
    WarmFormal();
    ExpectBoundedTurn({"D"}, -90, 90);
}

TEST_P(ScriptAnimationRateTest, FormalGaspHalfTurnBoundsActorModelAndUsesStablePositiveDirection)
{
    WarmFormal();
    ExpectBoundedTurn({"S"}, 180, 180);
}

TEST_P(ScriptAnimationRateTest, FormalGaspFrozenPoseWorldFeetAndPelvisFollowContinuousActorTurn)
{
    WarmFormal();
    ASSERT_TRUE(world->ControlAnimation(world->GetGeneration(), "play", 0));
    const auto pose = Player()->Pose(0);
    for (int frame = 0; frame < Frames(0.5); ++frame)
    {
        const float beforeYaw = Yaw();
        const Eigen::Vector3f beforeRoot = ScriptVector(State(), "position");
        const std::array<Eigen::Vector3f, 3> before =
            {JointPosition("pelvis"), JointPosition("foot_l"), JointPosition("foot_r")};
        Tick({"S"});
        const float delta = AngleDelta(beforeYaw, Yaw());
        EXPECT_LE(std::abs(delta), 540.0f / GetParam() + 0.002f);
        const Eigen::AngleAxisf turn(delta * 3.14159265358979323846f / 180, Eigen::Vector3f::UnitY());
        const Eigen::Vector3f afterRoot = ScriptVector(State(), "position");
        const std::array<Eigen::Vector3f, 3> after =
            {JointPosition("pelvis"), JointPosition("foot_l"), JointPosition("foot_r")};
        for (size_t joint = 0; joint < before.size(); ++joint)
        {
            const Eigen::Vector3f oldOffset = before[joint] - beforeRoot;
            const Eigen::Vector3f newOffset = after[joint] - afterRoot;
            EXPECT_TRUE(newOffset.isApprox(turn * oldOffset, 0.001f)) << frame << ":" << joint;
            const float horizontalRadius = Eigen::Vector2f(oldOffset.x(), oldOffset.z()).norm();
            const float bound = 2 * horizontalRadius *
                std::sin(540.0f / GetParam() * 3.14159265358979323846f / 360);
            EXPECT_LE((newOffset - oldOffset).norm(), bound + 0.01f);
        }
        EXPECT_EQ(Player()->Pose(0), pose);
    }
}

TEST_P(ScriptAnimationRateTest, FormalGaspInterruptedTurnImmediatelyTakesNewShortestDirection)
{
    WarmFormal();
    for (int frame = 0; frame < Frames(0.1); ++frame) Tick({"S"});
    const float before = Yaw();
    ASSERT_GT(before, 0);
    ASSERT_LT(before, 90);
    Tick({"D"});
    EXPECT_NEAR(AngleDelta(before, Yaw()), -540.0f / GetParam(), 0.002f);
    for (int frame = 0; frame < Frames(0.5); ++frame) Tick({"D"});
    EXPECT_NEAR(AngleDelta(Yaw(), -90), 0, 0.002f);
}

TEST_P(ScriptAnimationRateTest, FormalGaspWrappedHeadingUsesTwoDegreeShortestArc)
{
    PrepareMotion("", "Gasp.CharacterBlueprint", 0, false, -179);
    for (int frame = 0; frame < Frames(0.7); ++frame) Tick({"W"});
    AuthoritativeTeleport(179);
    Tick({"W"});
    EXPECT_NEAR(AngleDelta(179, Yaw()), 2, 0.002f);
    EXPECT_NEAR(AngleDelta(Yaw(), -179), 0, 0.002f);
}

TEST_P(ScriptAnimationRateTest, FormalGaspPauseFreezesTurnModelAndPose)
{
    WarmFormal();
    for (int frame = 0; frame < Frames(0.1); ++frame) Tick({"S"});
    const float yaw = Yaw();
    const auto model = Model();
    const auto pose = Player()->Pose(0);
    std::string error;
    ASSERT_TRUE(runtime.Control(*world, ScriptPlayState::Paused, error));
    for (int frame = 0; frame < Frames(0.5); ++frame) Tick({"D"});
    EXPECT_FLOAT_EQ(Yaw(), yaw);
    EXPECT_TRUE(Model().isApprox(model, 0));
    EXPECT_EQ(Player()->Pose(0), pose);
    ASSERT_TRUE(runtime.Control(*world, ScriptPlayState::Playing, error));
    Tick({"S"});
    EXPECT_NEAR(AngleDelta(yaw, Yaw()), 540.0f / GetParam(), 0.002f);
}

TEST_P(ScriptAnimationRateTest, FormalGaspZeroTimeScaleFreezesTurnModelAndPose)
{
    WarmFormal();
    for (int frame = 0; frame < Frames(0.1); ++frame) Tick({"S"});
    const float yaw = Yaw();
    const auto model = Model();
    const auto pose = Player()->Pose(0);
    std::string error;
    ASSERT_TRUE(runtime.SetTimeScale(0, error));
    for (int frame = 0; frame < Frames(0.5); ++frame) Tick({"D"});
    EXPECT_FLOAT_EQ(Yaw(), yaw);
    EXPECT_TRUE(Model().isApprox(model, 0));
    EXPECT_EQ(Player()->Pose(0), pose);
    ASSERT_TRUE(runtime.SetTimeScale(1, error));
    Tick({"S"});
    EXPECT_NEAR(AngleDelta(yaw, Yaw()), 540.0f / GetParam(), 0.002f);
}

TEST_P(ScriptAnimationRateTest, FormalGaspStopAndPlayRestartFromAuthoredHeading)
{
    WarmFormal();
    for (int frame = 0; frame < Frames(0.1); ++frame) Tick({"S"});
    ASSERT_GT(Yaw(), 0);
    std::string error;
    ASSERT_TRUE(runtime.Control(*world, ScriptPlayState::Editing, error));
    EXPECT_TRUE(world->GetScriptBlueprintStates().empty());
    for (const auto& actor : world->GetScriptBlueprints())
        if (actor.at("id") == "owner")
            EXPECT_TRUE(Model().isApprox(ScriptComponentMatrix(actor, 0), 0.001f));
    Play();
    EXPECT_FLOAT_EQ(Yaw(), 0);
    Tick({"S"});
    EXPECT_NEAR(Yaw(), 540.0f / GetParam(), 0.002f);
}

TEST_P(ScriptAnimationRateTest, FormalGaspTeleportTurnUsesNewAuthoritativeHeading)
{
    WarmFormal();
    for (int frame = 0; frame < Frames(0.1); ++frame) Tick({"S"});
    AuthoritativeTeleport(-120);
    Tick({"W"});
    EXPECT_NEAR(AngleDelta(-120, Yaw()), 540.0f / GetParam(), 0.002f);
    EXPECT_GT(State()["position"][0].get<float>(), 2999);
}

TEST_F(ScriptAnimationTest, FormalGaspRejectsNonpositiveTurnSpeedAtCreation)
{
    EXPECT_THROW(PrepareMotion("", "Gasp.CharacterBlueprint", 0, false, 0, 0), std::exception);
}

TEST_F(ScriptAnimationTest, FormalGaspSpringArmOrbitsWithLookInputIndependentlyOfActorYaw)
{
    PrepareMotion("", "Gasp.CharacterBlueprint", 0, false, 35);
    Tick();
    const float actorYaw = State()["rotation"][1].get<float>();
    // 正式鼠标轴使世界 yaw 从 35 到 65，pitch 从 -12 到 -27；不驱动角色朝向。
    InputActionState input;
    input.gameplay = map.Sample({}, true, true, -200, 100);
    runtime.Tick(world, 1.0f / 60, input, diagnostics);
    ASSERT_FALSE(State()["failed"]);
    EXPECT_NEAR(State()["rotation"][1].get<float>(), actorYaw, 0.001f);
    const float radians = 3.14159265358979323846f / 180;
    const Eigen::Vector3f pivot(State()["position"][0].get<float>(),
        State()["position"][1].get<float>(), State()["position"][2].get<float>());
    const Eigen::Vector3f backward(std::sin(65 * radians) * std::cos(-27 * radians),
        -std::sin(-27 * radians), std::cos(65 * radians) * std::cos(-27 * radians));
    EXPECT_TRUE(world->GetRenderCamera()->GetPosition().isApprox(pivot + 450 * backward, 0.0001f));
    EXPECT_TRUE(world->GetRenderCamera()->GetForwardVector().isApprox(-backward, 0.0001f));
}

TEST_F(ScriptAnimationTest, FormalGaspSpringArmRetractsToSphereCenterAtRearWall)
{
    // yaw=180 时后向为 -Z，既有 z=-250、半厚 20 的墙正好位于相机路径上。
    PrepareMotion("", "Gasp.CharacterBlueprint", 0, true, 180);
    Tick();
    ASSERT_FALSE(State()["failed"]);
    const auto camera = world->GetRenderCamera()->GetPosition();
    EXPECT_NEAR(camera.z(), -218, 0.1f); // -250+20+12，不再重复扣球半径或旧控制器的 2 cm。
    const float pivotY = State()["position"][1].get<float>();
    EXPECT_NEAR(camera.y(), pivotY + 218 * std::tan(12 * 3.14159265358979323846f / 180), 0.1f);
}

struct SourceContactCurve
{
    std::vector<float> times, values;
    float Sample(float time) const
    {
        const auto next = std::upper_bound(times.begin(), times.end(), time);
        if (next == times.begin()) return values.front();
        if (next == times.end()) return values.back();
        const size_t index = static_cast<size_t>(next - times.begin());
        const float alpha = (time - times[index - 1]) / (times[index] - times[index - 1]);
        return values[index - 1] + alpha * (values[index] - values[index - 1]);
    }
};

struct RootTravelInterval
{
    float begin = 0, end = 0;
    Eigen::Matrix3f component = Eigen::Matrix3f::Identity();
    double achievedDistance = 0;
    size_t traceIndex = 0;
};

struct RootCycleTravelMeasure
{
    double stableSeconds = 0, sourceSeconds = 0, ankleSeconds = 0, ankleDistance = 0, rateIntegral = 0;
    double achievedDistance = 0, sourceDistance = 0;
    int stableSamples = 0, ankleSamples = 0;
    std::array<bool, 120> sourceBins{};
    double TravelErrorSpeed() const { return std::abs(sourceDistance - achievedDistance) / stableSeconds; }
    Json Summary() const
    {
        return {{"stableSamples", stableSamples}, {"stableSeconds", stableSeconds}, {"sourceSeconds", sourceSeconds},
            {"achievedDistanceCm", achievedDistance}, {"sourceRootDistanceCm", sourceDistance},
            {"rootTravelErrorCmPerSecond", stableSeconds > 0 ? TravelErrorSpeed() : 0},
            {"curveFilteredAnkleSamples", ankleSamples}, {"curveFilteredAnkleSeconds", ankleSeconds},
            {"curveFilteredAnkleDistanceCm", ankleDistance},
            {"curveFilteredAnkleMeanSpeedCmPerSecond", ankleSeconds > 0 ? ankleDistance / ankleSeconds : 0},
            {"meanRate", stableSeconds > 0 ? rateIntegral / stableSeconds : 1},
            {"sourcePhaseCoverage", static_cast<double>(std::count(sourceBins.begin(), sourceBins.end(), true)) / sourceBins.size()}};
    }
};

TEST_F(ScriptAnimationTest, FormalGaspSpeedMatchingAlignsRootCycleTravelWithAchievedWalkAndRun)
{
    constexpr float dt = 1.0f / 60;
    constexpr float contactThreshold = 0.99f;
    const std::array<std::string, 3> loops = {"M_Neutral_Walk_Loop_F", "M_Neutral_Run_Loop_F", "M_Neutral_Sprint_Loop_F"};
    std::map<std::string, std::array<SourceContactCurve, 2>> contacts;
    for (const auto& clip : loops)
    {
        std::ifstream input(FormalResourceRoot() / "Maps/SC_gasp/Source" / (clip + ".json"));
        const auto sidecar = Json::parse(input);
        for (size_t foot = 0; foot < 2; ++foot)
            for (const auto& curve : sidecar.at("floatCurveKeys"))
                if (curve.at("name") == (foot == 0 ? "contact_l" : "contact_r"))
                    contacts[clip][foot] = {curve.at("times").get<std::vector<float>>(),
                        curve.at("values").get<std::vector<float>>()};
        for (const auto& curve : contacts.at(clip))
        {
            ASSERT_GE(curve.times.size(), 2u);
            ASSERT_EQ(curve.times.size(), curve.values.size());
        }
    }
    Json evidence{{"frameRate", 60}, {"secondsPerMode", 8}, {"warmupSeconds", 3},
        {"rootTravelOracle", "independent imported SampleModels root positions with continuous cycle displacement"},
        {"contactThreshold", contactThreshold}, {"contactSource", "formal Source/<clip>.json floatCurveKeys"},
        {"ankleDiagnostic", "foot_l/r are ankles; UE FootPlacement speed curves are not binary stance ground truth; no foot-slip acceptance claim"}};
    std::array<std::map<std::string, RootCycleTravelMeasure>, 4> measures;
    std::array<std::map<std::string, int>, 4> clipFrames;
    std::array<int, 4> feedbackMismatch{};
    for (int mode = 0; mode < 4; ++mode)
    {
        const bool run = mode >= 2, enabled = mode % 2 != 0;
        const std::string label = std::string(run ? "run" : "walk") + (enabled ? "-on" : "-off");
        if (mode != 0)
        {
            runtime.Shutdown();
            world = std::make_shared<World>(1, "animation");
            world->SetCamera(std::make_shared<Camera>());
            runtime.Initialize(true, VL_SCRIPT_OUTPUT_DIRECTORY, std::string(VL_SCRIPT_OUTPUT_DIRECTORY) + "/ScriptFixtures.dll");
        }
        PrepareMotion("", "Gasp.CharacterBlueprint", 0, false, 0, 540, enabled);
        const auto player = Player();
        std::array<size_t, 2> footIndices{};
        for (size_t foot = 0; foot < 2; ++foot)
        {
            const std::string name = foot == 0 ? "foot_l" : "foot_r";
            const auto& joints = player->Asset().mesh.joints;
            const auto found = std::find_if(joints.begin(), joints.end(),
                [&name](const Animation::Joint& joint) { return joint.name == name; });
            ASSERT_NE(found, joints.end());
            footIndices[foot] = static_cast<size_t>(found - joints.begin());
        }
        Json trace = Json::array();
        std::map<std::string, std::vector<RootTravelInterval>> rootIntervals;
        std::string previousClip;
        float previousTime = 0;
        bool previousStable = false;
        Eigen::Vector3f previousActor = Eigen::Vector3f::Zero();
        std::array<float, 2> previousContact{};
        std::array<Eigen::Vector3f, 2> previousFeet{};
        for (int frame = 0; frame < 8 * 60; ++frame)
        {
            Tick(run ? std::set<std::string>{"W", "Left Shift"} : std::set<std::string>{"W"}, dt);
            const auto& clip = player->Asset().clips.at(player->SelectedClip()).id;
            const auto& model = world->GetMeshObjects().at("__blueprint/owner/visual").model;
            const Eigen::Vector3f actorPosition = ScriptVector(State(), "position");
            const auto speed = world->ScriptAnimationFeedback().front().at("motion").at("speedMatching");
            feedbackMismatch[mode] += speed.at("enabled").get<bool>() != enabled;
            const bool stable = frame >= 3 * 60 && player->IsLooping() && !player->IsBlending() && contacts.count(clip);
            std::array<Eigen::Vector3f, 2> feet;
            std::array<float, 2> contact{};
            Json sample{{"frame", frame}, {"clip", clip}, {"time", player->Time()}, {"rate", player->Rate()},
                {"stable", stable}, {"blending", player->IsBlending()}, {"speedMatching", speed},
                {"actorPosition", State().at("position")}, {"actualVelocity", State().at("bodyVelocity")}};
            for (size_t foot = 0; foot < 2; ++foot)
            {
                feet[foot] = (model * player->Models().at(footIndices[foot])).block<3, 1>(0, 3);
                if (contacts.count(clip)) contact[foot] = contacts.at(clip)[foot].Sample(player->Time());
                sample[foot == 0 ? "left" : "right"] = {{"contact", contact[foot]},
                    {"world", {feet[foot].x(), feet[foot].y(), feet[foot].z()}}};
            }
            ++clipFrames[mode][clip];
            // 验收比较同一稳态区间的真实胶囊行程和原根采样行程，不读取倍率算法的速度预算。
            // contact 是 UE FootPlacement 速度曲线；踝部曲线筛选漂移仅诊断，不代表足底支撑真值。
            if (stable && previousStable && clip == previousClip)
            {
                auto& value = measures[mode][clip];
                ++value.stableSamples;
                value.stableSeconds += dt;
                float sourceDelta = player->Time() - previousTime;
                if (sourceDelta < 0) sourceDelta += player->Duration();
                value.sourceSeconds += sourceDelta;
                value.rateIntegral += player->Rate() * dt;
                const Eigen::Vector3f actorDelta = actorPosition - previousActor;
                const double achievedDistance = Eigen::Vector2f(actorDelta.x(), actorDelta.z()).norm();
                value.achievedDistance += achievedDistance;
                rootIntervals[clip].push_back({previousTime, player->Time(), model.block<3, 3>(0, 0),
                    achievedDistance, trace.size()});
                const size_t bin = static_cast<size_t>(player->Time() / player->Duration() * value.sourceBins.size());
                value.sourceBins.at(std::min(bin, value.sourceBins.size() - 1)) = true;
                for (size_t foot = 0; foot < 2; ++foot)
                    if (contact[foot] >= contactThreshold && previousContact[foot] >= contactThreshold)
                    {
                        const Eigen::Vector3f displacement = feet[foot] - previousFeet[foot];
                        value.ankleDistance += Eigen::Vector2f(displacement.x(), displacement.z()).norm();
                        value.ankleSeconds += dt;
                        ++value.ankleSamples;
                    }
            }
            trace.push_back(std::move(sample));
            previousStable = stable;
            previousClip = clip;
            previousTime = player->Time();
            previousActor = actorPosition;
            previousContact = contact;
            previousFeet = feet;
        }
        for (const auto& intervals : rootIntervals)
            for (const auto& clip : player->Asset().clips)
            {
                if (clip.id != intervals.first) continue;
                std::vector<float> times{0, clip.raw.duration};
                for (const auto& interval : intervals.second)
                {
                    times.push_back(interval.begin);
                    times.push_back(interval.end);
                }
                const auto roots = Animation::SampleModels(player->Asset().mesh, clip, times);
                const Eigen::Vector3f cycle = roots[1].front().block<3, 1>(0, 3) -
                    roots[0].front().block<3, 1>(0, 3);
                for (size_t index = 0; index < intervals.second.size(); ++index)
                {
                    const auto& interval = intervals.second[index];
                    Eigen::Vector3f rootDelta = roots[3 + 2 * index].front().block<3, 1>(0, 3) -
                        roots[2 + 2 * index].front().block<3, 1>(0, 3);
                    // 循环归零只改变采样相位；补一周期原根位移，避免把跨界当成倒退。
                    const bool wrapped = interval.end < interval.begin;
                    if (wrapped) rootDelta += cycle;
                    const Eigen::Vector3f worldDelta = interval.component * rootDelta;
                    const double sourceDistance = Eigen::Vector2f(worldDelta.x(), worldDelta.z()).norm();
                    measures[mode].at(clip.id).sourceDistance += sourceDistance;
                    trace.at(interval.traceIndex)["rootTravel"] = {{"achievedDistanceCm", interval.achievedDistance},
                        {"sourceRootDistanceCm", sourceDistance}, {"cycleWrapped", wrapped},
                        {"sourceRootWorldDelta", {worldDelta.x(), worldDelta.y(), worldDelta.z()}}};
                }
            }
        Json summary = Json::object();
        for (const auto& entry : measures[mode]) summary[entry.first] = entry.second.Summary();
        evidence[label] = {{"clipFrames", clipFrames[mode]}, {"clips", summary},
            {"feedbackMismatch", feedbackMismatch[mode]}, {"failed", State().at("failed")}, {"trace", std::move(trace)}};
    }
    // 失败也完整留存四段输入和所有片段覆盖；高频 trace 仅写 GTest XML，不刷终端。
    RecordProperty("speedMatchQuality", evidence.dump());
    for (int mode = 0; mode < 4; ++mode)
    {
        SCOPED_TRACE(mode);
        EXPECT_EQ(feedbackMismatch[mode], 0);
        const std::string expected = loops[mode >= 2 ? 1 : 0];
        ASSERT_TRUE(measures[mode].count(expected));
        const auto& value = measures[mode].at(expected);
        const auto formal = FormalPlayer();
        for (const auto& clip : formal->Asset().clips)
            if (clip.id == expected) EXPECT_GE(value.sourceSeconds, clip.raw.duration);
        EXPECT_GE(static_cast<double>(std::count(value.sourceBins.begin(), value.sourceBins.end(), true)) /
            value.sourceBins.size(), 0.95);
        ASSERT_GT(value.stableSeconds, 0);
        EXPECT_FALSE(evidence.at(std::string(mode >= 2 ? "run" : "walk") +
            (mode % 2 ? "-on" : "-off")).at("failed").get<bool>());
    }
    const auto& walkOff = measures[0].at(loops[0]);
    const auto& walkOn = measures[1].at(loops[0]);
    const auto& runOff = measures[2].at(loops[1]);
    const auto& runOn = measures[3].at(loops[1]);
    // 只验收原根周期与实际行程的配速；局部足部形变和 FootPlacement 曲线不保证脚底锁定。
    EXPECT_LT(walkOff.TravelErrorSpeed(), 2);
    EXPECT_LT(walkOn.TravelErrorSpeed(), 2);
    EXPECT_NEAR(runOff.TravelErrorSpeed(), 50, 2);
    EXPECT_LT(runOn.TravelErrorSpeed(), 2);
    EXPECT_NEAR(walkOff.rateIntegral / walkOff.stableSeconds, 1, 0.0001);
    EXPECT_NEAR(runOff.rateIntegral / runOff.stableSeconds, 1, 0.0001);
    EXPECT_NEAR(runOn.rateIntegral / runOn.stableSeconds, 0.9, 0.01);
}

struct FootReferenceOracle
{
    size_t joint = 0;
    std::array<Eigen::Vector3f, 2> local;
    std::vector<std::pair<size_t, size_t>> soleVertices;
};

std::array<FootReferenceOracle, 2> BuildFootReferences(const Animation::SkeletonMesh& mesh, Json& evidence)
{
    std::array<FootReferenceOracle, 2> result;
    evidence = Json::array();
    for (size_t side = 0; side < 2; ++side)
    {
        const std::string suffix = side == 0 ? "_l" : "_r";
        size_t foot = mesh.joints.size(), ball = mesh.joints.size();
        for (size_t joint = 0; joint < mesh.joints.size(); ++joint)
        {
            if (mesh.joints[joint].name == "foot" + suffix) foot = joint;
            if (mesh.joints[joint].name == "ball" + suffix) ball = joint;
        }
        if (foot == mesh.joints.size() || ball == mesh.joints.size())
            throw std::runtime_error("Formal foot/ball bones are required by the independent oracle");
        result[side].joint = foot;
        Json references = Json::array();
        for (size_t point = 0; point < 2; ++point)
        {
            Eigen::Vector3f bind = mesh.joints[point == 0 ? ball : foot].restModel.block<3, 1>(0, 3);
            bind.y() = mesh.joints.front().restModel(1, 3);
            result[side].local[point] = (mesh.joints[foot].restModel.inverse() * bind.homogeneous()).head<3>();
            references.push_back({bind.x(), bind.y(), bind.z()});
        }
        float minimumY = std::numeric_limits<float>::max();
        std::vector<std::pair<size_t, size_t>> candidates;
        for (size_t section = 0; section < mesh.sections.size(); ++section)
            for (size_t vertex = 0; vertex < mesh.sections[section].vertices.size(); ++vertex)
            {
                const auto& skin = mesh.sections[section];
                float footWeight = 0;
                for (size_t influence = skin.influenceOffsets[vertex]; influence < skin.influenceOffsets[vertex + 1]; ++influence)
                    if (skin.influences[influence].joint == foot || skin.influences[influence].joint == ball)
                        footWeight += skin.influences[influence].weight;
                if (footWeight < 0.5f) continue;
                const Eigen::Vector3f bind = (skin.meshToModel * skin.vertices[vertex].position.homogeneous()).head<3>();
                minimumY = std::min(minimumY, bind.y());
                candidates.emplace_back(section, vertex);
            }
        Json vertices = Json::array();
        double maximumProxyDistance = 0;
        for (const auto& candidate : candidates)
        {
            const auto& skin = mesh.sections[candidate.first];
            const Eigen::Vector3f bind = (skin.meshToModel * skin.vertices[candidate.second].position.homogeneous()).head<3>();
            if (bind.y() > minimumY + 1) continue;
            result[side].soleVertices.push_back(candidate);
            const Eigen::Vector3f local = (mesh.joints[foot].restModel.inverse() * bind.homogeneous()).head<3>();
            maximumProxyDistance = std::max(maximumProxyDistance, static_cast<double>(
                std::min((local - result[side].local[0]).norm(), (local - result[side].local[1]).norm())));
            vertices.push_back({{"section", candidate.first}, {"vertex", candidate.second},
                {"bindModel", {bind.x(), bind.y(), bind.z()}}, {"footLocal", {local.x(), local.y(), local.z()}}});
        }
        evidence.push_back({{"side", side == 0 ? "left" : "right"}, {"referencesBallHeel", references},
            {"soleSelection", "foot+ball positive weight sum >=0.5; all vertices within 1cm of bind minimum Y"},
            {"bindSoleMinimumY", minimumY}, {"maximumBindVertexNearestProxyDistanceCm", maximumProxyDistance},
            {"vertices", std::move(vertices)}});
    }
    return result;
}

struct SourceStanceOracle
{
    float duration = 0;
    std::array<std::vector<int>, 2> pivot, window;
    std::array<int, 2> stanceBins{};
    size_t Bin(float time) const
    {
        return std::min(pivot[0].size() - 1, static_cast<size_t>(time / duration * pivot[0].size()));
    }
};

SourceStanceOracle BuildSourceStanceOracle(const Animation::SkeletalAsset& asset, const Animation::Clip& clip,
    const std::array<FootReferenceOracle, 2>& feet, Json& evidence)
{
    SourceStanceOracle result;
    result.duration = clip.raw.duration;
    const size_t count = static_cast<size_t>(std::lround(result.duration * 240));
    const float step = result.duration / count;
    std::vector<float> times;
    for (size_t sample = 0; sample <= count; ++sample) times.push_back(step * sample);
    times.back() = result.duration;
    const auto models = Animation::SampleModels(asset.mesh, clip, times);
    const Eigen::Vector3f cycle = models.back().front().block<3, 1>(0, 3) - models.front().front().block<3, 1>(0, 3);
    Json sides = Json::array();
    for (size_t side = 0; side < 2; ++side)
    {
        std::array<std::vector<Eigen::Vector3f>, 2> positions;
        std::array<float, 2> minimumY{std::numeric_limits<float>::max(), std::numeric_limits<float>::max()};
        for (size_t point = 0; point < 2; ++point)
            for (size_t sample = 0; sample <= count; ++sample)
            {
                const Eigen::Vector3f position = (models[sample][feet[side].joint] *
                    feet[side].local[point].homogeneous()).head<3>();
                positions[point].push_back(position);
                minimumY[point] = std::min(minimumY[point], position.y());
            }
        Json samples = Json::array();
        std::array<int, 2> speed40Bins{}, speed80Bins{}, candidateBins{};
        std::array<std::vector<bool>, 2> candidate;
        result.pivot[side].assign(count, -1);
        result.window[side].assign(count, -1);
        int previousPivot = 0;
        bool previousStance = false;
        for (size_t sample = 0; sample < count; ++sample)
        {
            std::array<float, 2> speed{};
            std::array<bool, 2> stance{};
            Json points = Json::array();
            for (size_t point = 0; point < 2; ++point)
            {
                // 原根位移连续延拓跨周期导数；标签不读取运行时锁脚、探针或最终姿态。
                const Eigen::Vector3f before = sample == 0 ? (positions[point][count - 1] - cycle).eval() : positions[point][sample - 1];
                const Eigen::Vector3f velocity = (positions[point][sample + 1] - before) / (2 * step);
                speed[point] = Eigen::Vector2f(velocity.x(), velocity.z()).norm();
                // 只用水平低速与贴近周期最低点；240 Hz 垂直差分在触地瞬间抖动明显，
                // 以它作硬门会把手部接触切碎，不能代表支撑相位。
                stance[point] = speed[point] <= 40 && positions[point][sample].y() - minimumY[point] <= 3;
                speed40Bins[point] += speed[point] <= 40;
                speed80Bins[point] += speed[point] <= 80;
                candidateBins[point] += stance[point];
                const auto& position = positions[point][sample];
                points.push_back({{"world", {position.x(), position.y(), position.z()}},
                    {"horizontalSpeedCmPerSecond", speed[point]}, {"verticalSpeedCmPerSecond", velocity.y()},
                    {"heightAboveCycleMinimumCm", position.y() - minimumY[point]}, {"candidate", stance[point]}});
            }
            candidate[0].push_back(stance[0]);
            candidate[1].push_back(stance[1]);
            // 支撑点跟随脚掌滚动：更低（更贴近地面）的参考点是当前接触点；
            // 1 cm 内视为等高，保留上一帧支点，避免左右脚速度接近时逐帧切换。
            if (stance[0] || stance[1])
            {
                const float height0 = positions[0][sample].y(), height1 = positions[1][sample].y();
                if (stance[0] && stance[1])
                    previousPivot = (!previousStance || height0 + 1 < height1) ? (height0 <= height1 ? 0 : 1) :
                        (height1 + 1 < height0 ? 1 : previousPivot);
                else previousPivot = stance[0] ? 0 : 1;
                result.pivot[side][sample] = previousPivot;
            }
            previousStance = stance[0] || stance[1];
            samples.push_back({{"time", times[sample]}, {"pointsBallHeel", points}});
        }
        int window = 0;
        Json windows = Json::array();
        for (size_t begin = 0; begin < count;)
        {
            const bool supported = candidate[0][begin] || candidate[1][begin];
            size_t end = begin + 1;
            while (end < count && (candidate[0][end] || candidate[1][end]) == supported) ++end;
            if (supported && (end - begin) * step >= 0.05f)
            {
                for (size_t index = begin; index < end; ++index) result.window[side][index] = window;
                // 触地与离地各留两个 60 Hz 渲染帧（240 Hz 采样为 8 个 bin）作过渡；
                // 落点/抬脚以及 bounded pelvis 的收敛不属于稳态支撑，但仍计入 stanceBins
                // 与覆盖统计，不按 solver planted 状态筛样本。
                const size_t boundary = std::min<size_t>(8, (end - begin) / 3);
                for (size_t index = begin; index < begin + boundary; ++index) result.pivot[side][index] = -1;
                for (size_t index = end - boundary; index < end; ++index) result.pivot[side][index] = -1;
                result.stanceBins[side] += static_cast<int>(end - begin);
                windows.push_back({{"id", window}, {"beginSeconds", begin * step},
                    {"endSeconds", end * step}});
                ++window;
            }
            else for (size_t index = begin; index < end; ++index) result.pivot[side][index] = -1;
            begin = end;
        }
        for (size_t sample = 0; sample < count; ++sample)
        {
            samples[sample]["pivot"] = result.pivot[side][sample];
            samples[sample]["window"] = result.window[side][sample];
        }
        sides.push_back({{"side", side == 0 ? "left" : "right"}, {"stanceBins", result.stanceBins[side]},
            {"stanceSecondsPerCycle", result.stanceBins[side] * step}, {"windowCount", window},
            {"windows", windows}, {"horizontalSpeed40BinsBallHeel", speed40Bins},
            {"horizontalSpeed80BinsBallHeel", speed80Bins}, {"candidateBinsBallHeel", candidateBins},
            {"samples", std::move(samples)}});
    }
    evidence = {{"duration", result.duration}, {"sampleRate", 240}, {"rootCycle", {cycle.x(), cycle.y(), cycle.z()}},
        {"rule", "horizontal speed<=40cm/s and height<=cycle minimum+3cm; contiguous any-point support >=0.05s; pivot is the lower reference with 1cm equal-height hysteresis; up to two 60Hz render intervals at each window edge are transition and excluded from slip/drift"},
        {"contactCurvesUsed", false},
        {"classificationScope", "fixed geometric low-motion proxy; 40cm/s is an engineering source-phase definition, not measured human/mesh contact ground truth"},
        {"sides", std::move(sides)}};
    return result;
}

float AnalyticTreadHeight(const Eigen::Vector3f& point, bool stairs)
{
    if (!stairs || std::abs(point.x()) > 150) return 0;
    const std::array<float, 6> heights{10, 20, 30, 20, 10, 0};
    float height = 0;
    for (size_t index = 0; index < heights.size(); ++index)
        if (point.z() <= -160.0f - 80 * index && point.z() >= -240.0f - 80 * index)
            height = std::max(height, heights[index]);
    return height;
}

struct FixedStanceMeasure
{
    double path = 0, seconds = 0, drift = 0, penetration = 0, supportError = 0;
    int samples = 0, previousWindow = -1, previousPivot = -1;
    Eigen::Vector3f previous = Eigen::Vector3f::Zero(), first = Eigen::Vector3f::Zero();
    std::vector<bool> covered;
    Json Summary(int expected) const
    {
        return {{"samples", samples}, {"seconds", seconds}, {"pathCm", path},
            {"meanSpeedCmPerSecond", seconds > 0 ? path / seconds : 0},
            {"maximumWindowDriftCm", drift}, {"maximumPenetrationCm", penetration},
            {"maximumSupportGapCm", supportError},
            {"stancePhaseCoverage", expected > 0 ? static_cast<double>(std::count(covered.begin(), covered.end(), true)) / expected : 0}};
    }
};

TEST_F(ScriptAnimationTest, FormalGaspFootPlacementDynamicQualityUsesFrozenRawRootStanceWindows)
{
    constexpr float dt = 1.0f / 60;
    // 诊断开关只选择 production Match 的锁模式，不改变任何通过阈值。
    const char* requestedLock = std::getenv("VL_FOOT_LOCK");
    const std::string footLock = requestedLock ? requestedLock : "Auto";
    const auto formal = FormalPlayer();
    Json referenceEvidence, sourceEvidence = Json::object();
    const auto feet = BuildFootReferences(formal->Asset().mesh, referenceEvidence);
    std::map<std::string, SourceStanceOracle> oracles;
    const std::array<std::string, 2> loopIds{"M_Neutral_Walk_Loop_F", "M_Neutral_Run_Loop_F"};
    // 先冻结全部源标签，再产生任一 off/on 输出，禁止从 planted 或“误差小”反向选择样本。
    for (const auto& clip : formal->Asset().clips)
        if (clip.id == loopIds[0] || clip.id == loopIds[1])
            oracles.emplace(clip.id, BuildSourceStanceOracle(formal->Asset(), clip, feet, sourceEvidence[clip.id]));
    RecordProperty("footPlacementFrozenSourceOracle", Json{{"references", referenceEvidence}, {"clips", sourceEvidence}}.dump());
    struct Baseline { Json actor, motion; size_t clip; float time; std::vector<Eigen::Matrix4f> preIk; };
    std::vector<Baseline> baseline;
    std::array<std::array<FixedStanceMeasure, 2>, 6> measures;
    Json evidence{{"frameRate", 60}, {"matchSpeedBothGroups", true}, {"warmupSeconds", 3},
        {"footLock", footLock}, {"walkRunSeconds", 10}, {"stairsSeconds", 6},
        {"stairsHeightsCm", {10, 20, 30, 20, 10, 0}},
        {"stairsDepthCm", 80}, {"stairsStartZ", -160}, {"edgeSamplesExcluded", false},
        {"oracleLimit", "ball and ankle-under-heel bind proxies; mesh sole vertices are separate diagnostics"},
        {"source", sourceEvidence}, {"references", referenceEvidence}};
    std::array<double, 6> sourceSeconds{}, maximumMeshPenetration{}, maximumPreIkDifference{};
    std::array<int, 6> mismatches{}, unclassifiedFrames{}, edgeSamples{}, failedFrames{};
    std::array<std::array<bool, 6>, 6> treadCoverage{};
    for (int mode = 0; mode < 6; ++mode)
    {
        const bool enabled = mode % 2 != 0, run = mode / 2 == 1, stairs = mode / 2 == 2;
        const std::string label = std::string(stairs ? "stairs" : run ? "run" : "walk") + (enabled ? "-on" : "-off");
        if (mode != 0)
        {
            runtime.Shutdown();
            world = std::make_shared<World>(1, "animation");
            world->SetCamera(std::make_shared<Camera>());
            runtime.Initialize(true, VL_SCRIPT_OUTPUT_DIRECTORY, std::string(VL_SCRIPT_OUTPUT_DIRECTORY) + "/ScriptFixtures.dll");
        }
        if (!enabled) baseline.clear();
        PrepareMotion("", "Gasp.CharacterBlueprint", 0, false, 0, 540, true, enabled, 0, stairs,
            false, false, footLock);
        const auto player = Player();
        const auto& oracle = oracles.at(loopIds[run ? 1 : 0]);
        for (auto& measure : measures[mode]) measure.covered.assign(oracle.pivot[0].size(), false);
        Json trace = Json::array();
        float previousTime = 0;
        std::string previousClip;
        bool previousStable = false;
        int stableHold = 0;
        int cycle = 0;
        const int frames = (stairs ? 6 : 10) * 60;
        for (int frame = 0; frame < frames; ++frame)
        {
            Tick(run ? std::set<std::string>{"W", "Left Shift"} : std::set<std::string>{"W"}, dt);
            const auto& component = world->GetMeshObjects().at("__blueprint/owner/visual").model;
            const auto& clip = player->Asset().clips.at(player->SelectedClip()).id;
            Json actor{{"position", State().at("position")}, {"rotation", State().at("rotation")},
                {"velocity", State().at("bodyVelocity")}, {"grounded", State().at("grounded")}};
            const auto feedback = world->ScriptAnimationFeedback().front();
            failedFrames[mode] += State().at("failed").get<bool>();
            Json motion = feedback.at("motion");
            const auto view = world->CaptureMotionDebug();
            motion["features"] = view->features; motion["normalizedQuery"] = view->normalizedQuery;
            if (!enabled) baseline.push_back({actor, motion, player->SelectedClip(), player->Time(), player->PreIkModels()});
            else
            {
                const auto& off = baseline.at(frame);
                mismatches[mode] += actor != off.actor || motion != off.motion || player->SelectedClip() != off.clip || player->Time() != off.time;
                for (size_t joint = 0; joint < player->PreIkModels().size(); ++joint)
                    maximumPreIkDifference[mode] = std::max(maximumPreIkDifference[mode],
                        static_cast<double>((player->PreIkModels()[joint] - off.preIk[joint]).cwiseAbs().maxCoeff()));
            }
            const bool expectedClip = clip == loopIds[run ? 1 : 0];
            // 台阶从静止起步；前 1.5s 用于加速到稳态，支撑统计不把加速滑步当作台阶质量。
            const bool measurement = stairs ? frame >= 90 : frame >= 180;
            // 片段切换/混合后的首帧脚部历史来自旧片段，至少要稳态 8 帧后才纳入支撑统计。
            if (expectedClip && !player->IsBlending()) ++stableHold; else stableHold = 0;
            const bool stable = measurement && stableHold >= 8;
            unclassifiedFrames[mode] += measurement && !stable;
            if (clip == previousClip && player->Time() < previousTime) ++cycle;
            if (stable && previousStable && clip == previousClip)
            {
                float delta = player->Time() - previousTime;
                if (delta < 0) delta += oracle.duration;
                sourceSeconds[mode] += delta;
                const size_t firstBin = oracle.Bin(previousTime), lastBin = oracle.Bin(player->Time());
                for (size_t side = 0; side < 2; ++side)
                    for (size_t step = 0; step <= oracle.pivot[side].size(); ++step)
                    {
                        const size_t bin = (firstBin + step) % oracle.pivot[side].size();
                        if (oracle.window[side][bin] >= 0) measures[mode][side].covered[bin] = true;
                        if (bin == lastBin) break;
                    }
            }
            Json sample{{"frame", frame}, {"clip", clip}, {"time", player->Time()}, {"stable", stable},
                {"actor", actor}, {"footPlacement", feedback.at("footPlacement")}, {"failed", State().at("failed")}};
            const auto actorPosition = ScriptVector(State(), "position");
            for (size_t tread = 0; tread < 6; ++tread)
                if (stairs && actorPosition.z() <= -180.0f - 80 * tread && actorPosition.z() >= -220.0f - 80 * tread)
                    treadCoverage[mode][tread] = true;
            for (size_t side = 0; side < 2; ++side)
            {
                const size_t bin = oracle.Bin(player->Time());
                const int pivot = stable ? oracle.pivot[side][bin] : -1;
                const int window = pivot < 0 ? -1 : oracle.window[side][bin] + cycle * 1000;
                auto& measure = measures[mode][side];
                Json points = Json::array();
                std::array<Eigen::Vector3f, 2> worldPoints;
                for (size_t point = 0; point < 2; ++point)
                {
                    worldPoints[point] = (component * player->Models()[feet[side].joint] * feet[side].local[point].homogeneous()).head<3>();
                    const auto& position = worldPoints[point];
                    const float gap = position.y() - AnalyticTreadHeight(position, stairs);
                    bool edge = false;
                    if (stairs)
                        for (int boundary = 0; boundary <= 6; ++boundary)
                            edge |= std::abs(position.z() - (-160 - 80 * boundary)) <= 5;
                    edgeSamples[mode] += edge;
                    points.push_back({{"world", {position.x(), position.y(), position.z()}},
                        {"analyticGapCm", gap}, {"edgeBand5Cm", edge}});
                }
                if (pivot >= 0)
                {
                    const auto& point = worldPoints[pivot];
                    // 穿入只对当前窗口的支撑参考点记账；摆动腿和另一参考点保留在 points 证据中，
                    // 不把“未强迫贴地”的摆动点算作支撑穿入。
                    measure.penetration = std::max(measure.penetration,
                        static_cast<double>(AnalyticTreadHeight(point, stairs) - point.y()));
                    measure.supportError = std::max(measure.supportError, static_cast<double>(std::abs(
                        point.y() - AnalyticTreadHeight(point, stairs))));
                    if (window == measure.previousWindow && pivot == measure.previousPivot)
                    {
                        const Eigen::Vector3f delta = point - measure.previous;
                        measure.path += Eigen::Vector2f(delta.x(), delta.z()).norm();
                        measure.seconds += dt; ++measure.samples;
                        const Eigen::Vector3f drift = point - measure.first;
                        measure.drift = std::max(measure.drift, static_cast<double>(Eigen::Vector2f(drift.x(), drift.z()).norm()));
                    }
                    else measure.first = point;
                    measure.previous = point;
                }
                measure.previousWindow = window; measure.previousPivot = pivot;
                double meshPenetration = 0;
                for (const auto& vertex : feet[side].soleVertices)
                {
                    const auto& section = player->Asset().mesh.sections[vertex.first];
                    const Eigen::Vector3f position = (component * Animation::BlendSkinMatrix(section, vertex.second,
                        player->Pose(vertex.first)->matrices) * section.vertices[vertex.second].position.homogeneous()).head<3>();
                    meshPenetration = std::max(meshPenetration, static_cast<double>(AnalyticTreadHeight(position, stairs) - position.y()));
                }
                if (measurement) maximumMeshPenetration[mode] = std::max(maximumMeshPenetration[mode], meshPenetration);
                sample[side == 0 ? "left" : "right"] = {{"sourceBin", bin}, {"sourcePivot", pivot},
                    {"sourceWindow", window}, {"referencesBallHeel", points}, {"meshSolePenetrationCm", meshPenetration}};
            }
            trace.push_back(std::move(sample));
            previousStable = stable; previousTime = player->Time(); previousClip = clip;
        }
        Json feetSummary = Json::array();
        for (size_t side = 0; side < 2; ++side) feetSummary.push_back(measures[mode][side].Summary(oracle.stanceBins[side]));
        evidence[label] = {{"feet", feetSummary}, {"sourceSeconds", sourceSeconds[mode]},
            {"unclassifiedFrames", unclassifiedFrames[mode]}, {"edgeSamples", edgeSamples[mode]},
            {"treadsCrossed", treadCoverage[mode]}, {"maximumMeshSolePenetrationCm", maximumMeshPenetration[mode]},
            {"offOnStateMismatches", mismatches[mode]}, {"maximumPreIkDifference", maximumPreIkDifference[mode]},
            {"failedFrames", failedFrames[mode]},
            {"trace", std::move(trace)}};
    }
    RecordProperty("footPlacementDynamicQuality", evidence.dump());
    for (int mode = 0; mode < 6; ++mode)
    {
        SCOPED_TRACE(mode);
        const bool stairs = mode / 2 == 2, enabled = mode % 2 != 0;
        const auto& oracle = oracles.at(loopIds[mode / 2 == 1 ? 1 : 0]);
        EXPECT_EQ(failedFrames[mode], 0);
        if (enabled)
        {
            EXPECT_EQ(mismatches[mode], 0);
            EXPECT_DOUBLE_EQ(maximumPreIkDifference[mode], 0);
        }
        if (stairs)
            for (bool crossed : treadCoverage[mode]) EXPECT_TRUE(crossed);
        else EXPECT_GE(sourceSeconds[mode], oracle.duration);
        for (size_t side = 0; side < 2; ++side)
        {
            const auto& measure = measures[mode][side];
            EXPECT_GT(oracle.stanceBins[side], 0);
            EXPECT_GT(measure.samples, 0);
            if (!stairs)
                EXPECT_GE(measure.Summary(oracle.stanceBins[side]).at("stancePhaseCoverage").get<double>(), 0.95);
            if (enabled)
            {
                EXPECT_EQ(measure.samples, measures[mode - 1][side].samples);
                EXPECT_DOUBLE_EQ(measure.seconds, measures[mode - 1][side].seconds);
                EXPECT_LE(measure.drift, 3);
                EXPECT_LE(measure.penetration, 1);
                if (stairs) EXPECT_LE(measure.supportError, 1);
                else
                {
                    const auto& off = measures[mode - 1][side];
                    EXPECT_LE(measure.path, off.path * 0.5);
                    ASSERT_GT(measure.seconds, 0);
                    ASSERT_GT(off.seconds, 0);
                    EXPECT_LE(measure.path / measure.seconds, off.path / off.seconds * 0.5);
                }
            }
        }
    }
}

TEST_F(ScriptAnimationTest, FormalGaspFootLockSelectionPublishesNativeMode)
{
    PrepareMotion("", "Gasp.CharacterBlueprint", 0, false, 0, 540, true, true, 0, false, false, false,
        "Ankle");
    Tick();
    EXPECT_TRUE(Player()->FootPlacement().enabled);
    EXPECT_EQ(Player()->FootPlacementLockType(), Animation::FootLockType::PivotAroundAnkle);
    EXPECT_EQ(Player()->FootPlacement().lockMode, "ankle");
}

TEST_F(ScriptAnimationTest, FootPlacementProbesSameFrameLateTickSupportTransform)
{
    constexpr float dt = 1.0f / 60, slopeDegrees = 18;
    const float radians = slopeDegrees * 3.14159265358979323846f / 180;
    const Eigen::Vector3f normal(-std::sin(radians), std::cos(radians), 0);
    auto actor = Actor("owner", "ScriptFixtures.PassiveBlueprint");
    actor["position"] = {0, 90, 0};
    actor["components"][0]["position"] = {0, -90, 0};
    actor["components"][0]["rotation"] = {0, 180, 0};
    actor["physics"] = {{"motion", "character"}, {"shape", "capsule"}, {"radius", 30}, {"halfHeight", 60}};
    auto support = Actor("late-support", "ScriptFixtures.SpringArmLateMoverBlueprint");
    support["components"] = Json::array();
    support["position"] = {50 * std::sin(radians), -50 * std::cos(radians), 0};
    support["rotation"] = {0, 0, slopeDegrees};
    support["physics"] = {{"motion", "static"}, {"shape", "box"}, {"halfExtents", {10000, 50, 10000}}};
    Prepare(Json::array({actor, support}), true);
    Play();
    auto request = MotionRequest("visual", Json::array({"M_Neutral_Stand_Idle_Loop"}));
    request["footPlacement"] = true;
    world->ApplyScriptAnimationRequests(State(), Json::array({request}));
    for (int frame = 0; frame < 4; ++frame)
    {
        SCOPED_TRACE(frame);
        Tick({}, dt);
        ASSERT_FALSE(State().at("failed").get<bool>());
        ASSERT_TRUE(State().at("grounded").get<bool>());
        const auto& supportPosition = State("late-support").at("position");
        const Eigen::Vector3f center(supportPosition[0].get<float>(),
            supportPosition[1].get<float>(), supportPosition[2].get<float>());
        EXPECT_NEAR(center.x(), 50 * std::sin(radians) + 200 * dt * (frame + 1), 0.001f);
        // 箱体上表面的解析平面独立于 solver。LateTick 每帧向右移 3.33 cm，
        // 旧 Jolt 位姿的命中点距当前平面约 1.03 cm，不能混入同帧足部结果。
        const Eigen::Vector3f planePoint = center + normal * 50;
        for (const auto& foot : Player()->FootPlacement().feet)
        {
            ASSERT_TRUE(foot.grounded);
            ASSERT_TRUE(foot.heelGrounded);
            EXPECT_EQ(foot.heelSupportIdentity, "late-support");
            EXPECT_NEAR(normal.dot(foot.groundPoint - planePoint), 0, 0.02f);
            EXPECT_NEAR(normal.dot(foot.heelGroundPoint - planePoint), 0, 0.02f);
        }
    }
}

TEST_F(ScriptAnimationTest, MotionDebugHistoryReplaysAndDoesNotAppendWhileFrozen)
{
    PrepareMotion("", "ScriptFixtures.MotionMatchingProbe");
    std::vector<std::shared_ptr<const Animation::MotionDebugView>> live;
    for (int frame = 0; frame < 6; ++frame)
    {
        Tick();
        live.push_back(world->CaptureMotionDebug());
    }
    ASSERT_GT(live.back()->replayFrames, 0);
    const int frames = live.back()->replayFrames;
    ASSERT_GE(frames, 4);
    const int target = frames - 3;
    const auto expected = live.at(static_cast<size_t>(target));
    world->SetMotionDebugReplayFrame(target);
    EXPECT_EQ(world->MotionDebugReplayFrame(), target);
    const auto replay = world->CaptureMotionDebug();
    ASSERT_TRUE(replay->replayActive);
    EXPECT_EQ(replay->replayFrame, target);
    EXPECT_EQ(replay->replayFrames, frames);
    EXPECT_EQ(replay->searchCount, expected->searchCount);
    EXPECT_EQ(replay->features, expected->features);
    ASSERT_EQ(replay->selected.has_value(), expected->selected.has_value());
    if (replay->selected)
        EXPECT_EQ(replay->selected->clipId, expected->selected->clipId);
    // 回放期间不再追加历史，拖动只返回同一帧的冻结副本。
    const int frozenFrames = replay->replayFrames;
    Tick();
    const auto repeated = world->CaptureMotionDebug();
    EXPECT_TRUE(repeated->replayActive);
    EXPECT_EQ(repeated->replayFrames, frozenFrames);
    world->SetMotionDebugReplayFrame(-1);
    const auto resumed = world->CaptureMotionDebug();
    EXPECT_FALSE(resumed->replayActive);
    EXPECT_GE(resumed->replayFrames, frozenFrames);
}

TEST_F(ScriptAnimationTest, FormalGaspFootPlacementFitsStaticSlopeWithoutChangingPreIkOrMotion)
{
    constexpr float dt = 1.0f / 60;
    constexpr int warmupFrames = 120, totalFrames = 240;
    constexpr float slopeDegrees = 18;
    const float radians = slopeDegrees * 3.14159265358979323846f / 180;
    const Eigen::Vector3f planeNormal(-std::sin(radians), std::cos(radians), 0);
    struct Baseline
    {
        Json actor, motion;
        float time = 0;
        size_t clip = 0;
        std::vector<Eigen::Matrix4f> preIk;
    };
    std::vector<Baseline> baseline;
    std::array<double, 2> gapSum{}, angleSum{}, maximumGap{}, maximumAngle{};
    std::array<int, 2> sampleCount{};
    Json evidence{{"frameRate", 60}, {"slopeDegrees", slopeDegrees}, {"warmupSeconds", 2},
        {"steadySeconds", 2}, {"planeNormal", {planeNormal.x(), planeNormal.y(), planeNormal.z()}},
        {"soleOracle", "independent rest-ball projection onto rest-root horizontal plane; no heel or mesh-sole claim"}};
    for (int mode = 0; mode < 2; ++mode)
    {
        if (mode != 0)
        {
            runtime.Shutdown();
            world = std::make_shared<World>(1, "animation");
            world->SetCamera(std::make_shared<Camera>());
            runtime.Initialize(true, VL_SCRIPT_OUTPUT_DIRECTORY,
                std::string(VL_SCRIPT_OUTPUT_DIRECTORY) + "/ScriptFixtures.dll");
        }
        PrepareMotion("", "Gasp.CharacterBlueprint", 0, false, 0, 540, false, mode != 0, slopeDegrees);
        const auto player = Player();
        const auto& joints = player->Asset().mesh.joints;
        std::array<size_t, 2> footIndices{};
        std::array<Eigen::Vector3f, 2> soleLocal{}, upLocal{};
        // 独立从正式绑定骨架构造 ball 参考点，不读取 solver 的目标或 targetError 作为真值。
        for (size_t side = 0; side < 2; ++side)
        {
            const std::string suffix = side == 0 ? "_l" : "_r";
            size_t foot = joints.size(), ball = joints.size();
            for (size_t joint = 0; joint < joints.size(); ++joint)
            {
                if (joints[joint].name == "foot" + suffix) foot = joint;
                if (joints[joint].name == "ball" + suffix) ball = joint;
            }
            ASSERT_LT(foot, joints.size()); ASSERT_LT(ball, joints.size());
            footIndices[side] = foot;
            Eigen::Vector3f reference = joints[ball].restModel.block<3, 1>(0, 3);
            reference.y() = joints.front().restModel(1, 3);
            soleLocal[side] = (joints[foot].restModel.inverse() * reference.homogeneous()).head<3>();
            upLocal[side] = joints[foot].restModel.block<3, 3>(0, 0).transpose() * Eigen::Vector3f::UnitY();
        }
        Json trace = Json::array();
        int activeSamples = 0, groundedSamples = 0, mismatches = 0;
        double maximumPreIkDifference = 0;
        for (int frame = 0; frame < totalFrames; ++frame)
        {
            Tick({}, dt);
            const auto& component = world->GetMeshObjects().at("__blueprint/owner/visual").model;
            const auto feedback = world->ScriptAnimationFeedback().front();
            const auto motionView = world->CaptureMotionDebug();
            const Json actor{{"position", State().at("position")}, {"rotation", State().at("rotation")},
                {"velocity", State().at("bodyVelocity")}, {"grounded", State().at("grounded")}};
            Json motion = feedback.at("motion");
            motion["features"] = motionView->features;
            motion["normalizedQuery"] = motionView->normalizedQuery;
            double preIkDifference = 0;
            if (mode == 0)
                baseline.push_back({actor, motion, player->Time(), player->SelectedClip(), player->PreIkModels()});
            else
            {
                const auto& previous = baseline.at(frame);
                if (actor != previous.actor || motion != previous.motion ||
                    player->Time() != previous.time || player->SelectedClip() != previous.clip) ++mismatches;
                for (size_t joint = 0; joint < joints.size(); ++joint)
                    preIkDifference = std::max(preIkDifference, static_cast<double>(
                        (player->PreIkModels()[joint] - previous.preIk[joint]).cwiseAbs().maxCoeff()));
                maximumPreIkDifference = std::max(maximumPreIkDifference, preIkDifference);
            }
            Json sample{{"frame", frame}, {"steady", frame >= warmupFrames}, {"actor", actor},
                {"motion", motion}, {"time", player->Time()}, {"clipIndex", player->SelectedClip()},
                {"footPlacement", feedback.at("footPlacement")}, {"preIkDifference", preIkDifference},
                {"failed", State().at("failed")}};
            for (size_t side = 0; side < 2; ++side)
            {
                const Eigen::Matrix4f finalFoot = component * player->Models()[footIndices[side]];
                const Eigen::Matrix4f preFoot = component * player->PreIkModels()[footIndices[side]];
                const Eigen::Vector3f sole = (finalFoot * soleLocal[side].homogeneous()).head<3>();
                const Eigen::Vector3f preSole = (preFoot * soleLocal[side].homogeneous()).head<3>();
                const Eigen::Vector3f up = (finalFoot.block<3, 3>(0, 0) * upLocal[side]).normalized();
                const double gap = planeNormal.dot(sole);
                const double angle = std::acos(std::clamp(up.dot(planeNormal), -1.0f, 1.0f)) *
                    180 / 3.14159265358979323846;
                sample[side == 0 ? "left" : "right"] = {
                    {"sole", {sole.x(), sole.y(), sole.z()}}, {"preSole", {preSole.x(), preSole.y(), preSole.z()}},
                    {"up", {up.x(), up.y(), up.z()}}, {"signedPlaneGapCm", gap}, {"normalAngleDegrees", angle}};
                if (frame >= warmupFrames)
                {
                    ++sampleCount[mode];
                    gapSum[mode] += std::abs(gap); angleSum[mode] += angle;
                    maximumGap[mode] = std::max(maximumGap[mode], std::abs(gap));
                    maximumAngle[mode] = std::max(maximumAngle[mode], angle);
                }
            }
            if (frame >= warmupFrames)
            {
                groundedSamples += State().at("grounded").get<bool>();
                activeSamples += player->FootPlacement().active;
            }
            trace.push_back(std::move(sample));
        }
        evidence[mode == 0 ? "off" : "on"] = {{"trace", std::move(trace)}, {"samples", sampleCount[mode]},
            {"groundedFrames", groundedSamples}, {"activeFrames", activeSamples}, {"mismatches", mismatches},
            {"maximumPreIkDifference", maximumPreIkDifference}, {"maximumGapCm", maximumGap[mode]},
            {"maximumNormalAngleDegrees", maximumAngle[mode]},
            {"meanAbsoluteGapCm", gapSum[mode] / sampleCount[mode]},
            {"meanNormalAngleDegrees", angleSum[mode] / sampleCount[mode]}};
    }
    // 先保留全部失败证据，再断言固定阈值；脚底误差不能靠放宽阈值隐藏。
    RecordProperty("footPlacementSlope", evidence.dump());
    EXPECT_EQ(evidence.at("on").at("mismatches"), 0);
    EXPECT_DOUBLE_EQ(evidence.at("on").at("maximumPreIkDifference").get<double>(), 0);
    EXPECT_EQ(evidence.at("off").at("groundedFrames"), totalFrames - warmupFrames);
    EXPECT_EQ(evidence.at("on").at("groundedFrames"), totalFrames - warmupFrames);
    EXPECT_EQ(evidence.at("on").at("activeFrames"), totalFrames - warmupFrames);
    EXPECT_LE(maximumGap[1], 1);
    EXPECT_LE(maximumAngle[1], 5);
    EXPECT_LT(gapSum[1], gapSum[0] * 0.3);
    EXPECT_LT(angleSum[1], angleSum[0] * 0.3);
    EXPECT_FALSE(evidence.at("off").at("trace").back().at("failed").get<bool>());
    EXPECT_FALSE(evidence.at("on").at("trace").back().at("failed").get<bool>());
}

TEST_P(ScriptAnimationRateTest, FormalGaspControllerPublishesNativeSelectionsForGroundedAndAirborneActions)
{
    RecordProperty("frameRate", GetParam());
    PrepareMotion("", "Gasp.CharacterBlueprint", 0, false, 0, 540, true, true);
    Json trace = Json::array();
    for (int frame = 0; frame < Frames(2.0 / 3); ++frame)
    {
        Tick();
        TraceMotion(trace, "idle", frame);
    }
    std::set<std::string> selected;
    bool started = false, stopped = false, turned = false;
    int steadyWalkFrames = 0, steadyIdleFrames = 0, steadyReverseFrames = 0, steadyRunFrames = 0;
    int steadyRunStopFrames = 0;
    for (int frame = 0; frame < Frames(5); ++frame)
    {
        Tick({"W"});
        const auto view = world->CaptureMotionDebug();
        if (view->selected)
        {
            selected.insert(view->selected->clipId);
            started |= view->selected->clipId == "M_Neutral_Walk_Start_F_Lfoot";
        }
        if (frame >= Frames(4) && Player()->Asset().clips.at(Player()->SelectedClip()).id == "M_Neutral_Walk_Loop_F")
            ++steadyWalkFrames;
        TraceMotion(trace, "start-walk", frame);
    }
    for (int frame = 0; frame < Frames(3); ++frame)
    {
        Tick();
        const auto view = world->CaptureMotionDebug();
        stopped |= view->selected && view->selected->clipId == "M_Neutral_Walk_Stop_F_Lfoot";
        if (frame >= Frames(2) && Player()->Asset().clips.at(Player()->SelectedClip()).id == "M_Neutral_Stand_Idle_Loop")
            ++steadyIdleFrames;
        TraceMotion(trace, "stop", frame);
    }
    for (int frame = 0; frame < Frames(3); ++frame)
    {
        Tick({"W"});
        TraceMotion(trace, "walk-before-reverse", frame);
    }
    for (int frame = 0; frame < Frames(4); ++frame)
    {
        Tick({"S"});
        const auto view = world->CaptureMotionDebug();
        turned |= view->selected && (view->selected->clipId == "M_Neutral_Stand_Turn_180_L" ||
            view->selected->clipId == "M_Neutral_Stand_Turn_180_R");
        if (frame >= Frames(3) && Player()->Asset().clips.at(Player()->SelectedClip()).id == "M_Neutral_Walk_Loop_F")
            ++steadyReverseFrames;
        TraceMotion(trace, "reverse", frame);
    }
    for (int frame = 0; frame < Frames(5); ++frame)
    {
        Tick({"S", "Left Shift"});
        const auto clip = Player()->Asset().clips.at(Player()->SelectedClip()).id;
        if (frame >= Frames(4) && (clip == "M_Neutral_Run_Loop_F" || clip == "M_Neutral_Sprint_Loop_F"))
            ++steadyRunFrames;
        TraceMotion(trace, "run", frame);
    }
    for (int frame = 0; frame < Frames(3); ++frame)
    {
        Tick();
        if (frame >= Frames(2) && Player()->Asset().clips.at(Player()->SelectedClip()).id == "M_Neutral_Stand_Idle_Loop")
            ++steadyRunStopFrames;
        TraceMotion(trace, "stop-after-run", frame);
    }
    Tick({"Space"});
    bool jumped = false, fell = false, landed = false;
    int airborneFrames = 0;
    EXPECT_TRUE(Player()->FootPlacement().enabled);
    if (!State().at("grounded").get<bool>())
    {
        ++airborneFrames;
        EXPECT_FALSE(Player()->FootPlacement().active);
    }
    for (int frame = 0; frame < Frames(2.5); ++frame)
    {
        Tick();
        if (!State().at("grounded").get<bool>())
        {
            ++airborneFrames;
            EXPECT_FALSE(Player()->FootPlacement().active) << "airborne frame " << frame;
        }
        const auto clip = Player()->Asset().clips.at(Player()->SelectedClip()).id;
        jumped |= clip == "M_Neutral_Jump_F_Start_Stand_Lfoot";
        fell |= clip == "M_Neutral_Jump_Loop_Fall";
        landed |= clip == "M_Neutral_Jump_F_Land_Stand_Light_Lfoot";
        TraceMotion(trace, "jump-land", frame);
    }
    ASSERT_FALSE(State()["failed"]);
    EXPECT_FALSE(selected.empty());
    EXPECT_TRUE(started);
    EXPECT_TRUE(stopped);
    EXPECT_TRUE(turned);
    // 动作片段曾经入选不足以验收：完整输入持续数秒后，末一秒必须稳定进入循环/待机。
    EXPECT_EQ(steadyWalkFrames, GetParam()) << "walking did not settle to WalkLoop";
    EXPECT_EQ(steadyIdleFrames, GetParam()) << "stopping did not settle to Idle";
    EXPECT_EQ(steadyReverseFrames, GetParam()) << "reverse did not settle to WalkLoop";
    EXPECT_EQ(steadyRunFrames, GetParam()) << "running did not settle to RunLoop/SprintLoop";
    EXPECT_EQ(steadyRunStopFrames, GetParam()) << "stopping after run did not settle to Idle";
    EXPECT_TRUE(jumped);
    EXPECT_TRUE(fell);
    EXPECT_TRUE(landed);
    EXPECT_GT(airborneFrames, 0);
    EXPECT_TRUE(Player()->InPlace());
    EXPECT_TRUE(Player()->Pose(0)->matrices.front().isIdentity(0.001f));
    std::string selectedClips;
    for (const auto& id : selected) selectedClips += id + ";";
    RecordProperty("groundedSelectedClips", selectedClips);
    RecordProperty("actionTrace", trace.dump());
}

TEST_P(ScriptAnimationRateTest, FormalGaspMotionMatchingSettlesToIdleWhenActualMotionIsBlocked)
{
    RecordProperty("frameRate", GetParam());
    PrepareMotion("", "Gasp.CharacterBlueprint", 0, true, 0, 540, true, true);
    for (int frame = 0; frame < Frames(2.0 / 3); ++frame) Tick();
    Json trace = Json::array();
    int steadyIdleFrames = 0;
    float finalSpeed = 0;
    for (int frame = 0; frame < Frames(5); ++frame)
    {
        Tick({"W"});
        const auto& velocity = State().at("bodyVelocity");
        finalSpeed = Eigen::Vector2f(velocity[0].get<float>(), velocity[2].get<float>()).norm();
        if (frame >= Frames(4) && Player()->Asset().clips.at(Player()->SelectedClip()).id == "M_Neutral_Stand_Idle_Loop")
            ++steadyIdleFrames;
        TraceMotion(trace, "blocked-walk", frame);
    }
    EXPECT_FALSE(State()["failed"]);
    EXPECT_TRUE(Player()->FootPlacement().enabled);
    EXPECT_GT(State()["position"][2].get<float>(), -205);
    EXPECT_LT(finalSpeed, 2);
    EXPECT_EQ(steadyIdleFrames, GetParam()) << "blocked character kept selecting a moving action";
    const auto view = world->CaptureMotionDebug();
    EXPECT_TRUE(view->queryUsesAchievedVelocity);
    ASSERT_FALSE(view->queryFutureRoots.empty());
    for (const auto& point : view->queryFutureRoots)
        EXPECT_NEAR((point - view->queryRoot.block<3, 1>(0, 3)).norm(), 0, 0.01f);
    RecordProperty("actionTrace", trace.dump());
}

INSTANTIATE_TEST_SUITE_P(FormalFrameRates, ScriptAnimationRateTest,
    testing::Values(30, 60, 120), testing::PrintToStringParamName());

TEST_F(ScriptAnimationTest, CurveValueMatchesHurdleSidecarWithinHalfCentimeter)
{
    auto actor = Actor("owner", "ScriptFixtures.MontageProbe");
    actor["properties"]["Mode"] = "curve";
    Prepare(Json::array({actor}), true, true);
    Play();
    Tick();
    ASSERT_FALSE(State()["failed"]);
    const auto& position = State()["position"];
    // linear sidecar 在窗口 EndTime 的插值与 UE 参考值 34.098671 / 90.908951 的差 < 0.5 cm。
    EXPECT_NEAR(position[0].get<float>(), 34.098671f, 0.5f);
    EXPECT_NEAR(position[1].get<float>(), 90.908951f, 0.5f);
    EXPECT_NEAR(std::abs(position[1].get<float>() - position[0].get<float>()), 56.810280f, 1.0f);
    EXPECT_FLOAT_EQ(position[2].get<float>(), 110.0f); // 超出末键 clamp 到末值。
}

TEST_F(ScriptAnimationTest, CurveValueClampsBothEndsAndTreatsSingleKeyAsConstant)
{
    auto actor = Actor("owner", "ScriptFixtures.MontageProbe");
    actor["properties"]["Mode"] = "curve-clamp";
    Prepare(Json::array({actor}), true, true);
    Play();
    Tick();
    ASSERT_FALSE(State()["failed"]);
    EXPECT_FLOAT_EQ(State()["position"][0].get<float>(), -75.0f); // 早于首键 clamp 到首值。
    EXPECT_FLOAT_EQ(State()["position"][1].get<float>(), 1.0f);  // 单键曲线为常量。
}

TEST_F(ScriptAnimationTest, CurveValueRejectsMissingClipCurveAndNonFiniteTime)
{
    auto actor = Actor("owner", "ScriptFixtures.MontageProbe");
    actor["properties"]["Mode"] = "curve-errors";
    Prepare(Json::array({actor}), true, true);
    Play();
    Tick();
    ASSERT_FALSE(State()["failed"]);
    // 缺 clip/曲线必须是 ArgumentException；NaN/Inf 必须是 ArgumentOutOfRangeException。
    EXPECT_EQ(State()["position"][0].get<float>(), 4);
}

TEST_F(ScriptAnimationTest, MontageCatalogExposesSectionsNotifiesAndWindowsAndRejectsUnknownId)
{
    auto actor = Actor("owner", "ScriptFixtures.MontageProbe");
    actor["properties"]["Mode"] = "montage-info";
    Prepare(Json::array({actor}), true, true);
    Play();
    Tick();
    ASSERT_FALSE(State()["failed"]);
    const auto& position = State()["position"];
    EXPECT_EQ(position[0].get<int>(), 1);   // sections
    EXPECT_EQ(position[1].get<int>(), 6);   // notifies
    EXPECT_EQ(position[2].get<int>(), 104); // 4 warp windows + 未知 id 抛错的 +100。
}

TEST_F(ScriptAnimationTest, PlayMontageRejectsUnsupportedExecutionWithDiagnostics)
{
    auto actor = Actor("owner", "ScriptFixtures.MontageProbe");
    actor["properties"]["Mode"] = "montage-reject";
    Prepare(Json::array({actor}), true, true);
    Play();
    ProcessLog().Clear();
    Tick();
    ASSERT_FALSE(State()["failed"]);
    // 未知 montage、缺/多 target、非 Linear、loop、非法 rate 仍拒绝；
    // 多 section 与 Static provider 已由原生执行层支持（见 animationScriptData/播放器测试）。
    EXPECT_EQ(State()["position"][0].get<float>(), 6);
    const auto logs = ProcessLog().Freeze();
    std::string messages;
    for (const auto& entry : logs->entries) messages += entry.message + "\n";
    EXPECT_NE(messages.find("EaseInOut"), std::string::npos);
    EXPECT_NE(messages.find("requires warp target"), std::string::npos);
    EXPECT_NE(messages.find("does not declare warp target"), std::string::npos);
    EXPECT_NE(messages.find("looping"), std::string::npos);
}

TEST_F(ScriptAnimationTest, PlayMontageAcceptsWarpTargetRotationOverload)
{
    auto actor = Actor("owner", "ScriptFixtures.MontageProbe");
    actor["properties"]["Mode"] = "montage-rotation";
    Prepare(Json::array({actor}), true, true);
    Play();
    Tick();
    ASSERT_FALSE(State()["failed"]);
    // C# 新增 WarpTarget 重载不抛异常；请求在下一动画 tick 生效。
    EXPECT_EQ(State()["position"][0].get<float>(), 0);
    Tick();
    EXPECT_EQ(Player()->MontageId(), "AM_M_Neutral_Traversal_Hurdle_1_0_stand_F_V2_Lfoot");
}

TEST_F(ScriptAnimationTest, PlayMontagePublishesMontageRangeInAnimationState)
{
    auto actor = Actor("owner", "ScriptFixtures.MontageProbe");
    actor["properties"]["Mode"] = "feedback";
    Prepare(Json::array({actor}), true, true);
    Play();
    Tick();
    ASSERT_FALSE(State()["failed"]);
    const auto& position = State()["position"];
    EXPECT_EQ(position[0].get<int>(), 1);
    EXPECT_NEAR(position[1].get<float>(), 1.666667f, 1e-4f);
    EXPECT_NEAR(position[2].get<float>(), 1.666667f, 1e-4f);
    EXPECT_NEAR(Player()->Duration(), 1.666667f, 1e-4f);
    EXPECT_EQ(Player()->MontageId(), "AM_M_Neutral_Traversal_Hurdle_1_0_stand_F_V2_Lfoot");
}

TEST_F(ScriptAnimationTest, PlayMontageStartTimeOverloadSkipsPreamble)
{
    auto actor = Actor("owner", "ScriptFixtures.MontageProbe");
    actor["properties"]["Mode"] = "montage-start";
    Prepare(Json::array({actor}), true, true);
    Play();
    // BeginPlay 的请求在 Play 内装配并停在入口：初始 seek 不补发 0.877 之前的事件。
    auto player = Player();
    EXPECT_FLOAT_EQ(player->Time(), 0.877f);
    EXPECT_FLOAT_EQ(player->PlaybackRangeStart(), 0.877f);
    EXPECT_NEAR(player->PlaybackRangeEnd(), 1.666667f, 1e-4f);
    EXPECT_TRUE(player->FiredNotifies().empty());
    EXPECT_TRUE(player->ActiveNotifyStates().empty());

    Tick();
    ASSERT_FALSE(State()["failed"]);
    const auto& position = State()["position"];
    EXPECT_EQ(position[0].get<int>(), 3); // 负/NaN/+Inf 都在 C# 入口按 ArgumentOutOfRange 拒绝
    EXPECT_NEAR(position[1].get<float>(), 0.877f, 1e-5f); // AnimationState.MontageRange.X
    EXPECT_NEAR(position[2].get<float>(), 0.877f, 1e-5f); // 初始 seek 后的原生时钟
    EXPECT_EQ(player->MontageId(), "AM_M_Neutral_Traversal_Hurdle_1_0_stand_F_V2_Lfoot");
    // 本帧只允许入口之后的事件；Catch 的前摇 notify（0.877 之前）不得补发。
    for (const auto& notify : player->FiredNotifies())
        EXPECT_GE(notify.time, 0.877f - 1e-4f) << notify.name << ":" << notify.phase;
}

TEST_F(ScriptAnimationTest, NotifyFeedbackParsesNativeBeginEvent)
{
    auto actor = Actor("owner", "ScriptFixtures.MontageProbe");
    actor["properties"]["Mode"] = "notify";
    Prepare(Json::array({actor}), true, true);
    Play();
    for (int frame = 0; frame < 4; ++frame) Tick();
    ASSERT_FALSE(State()["failed"]);
    const auto& position = State()["position"];
    EXPECT_GT(position[0].get<int>(), 0);
    EXPECT_FLOAT_EQ(position[1].get<float>(), 0.0f);
    EXPECT_EQ(position[2].get<int>(), 1); // "begin"
}

#endif
}
