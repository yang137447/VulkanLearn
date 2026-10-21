#include "mesh/loader/animation/assimpSkeletalImporter.h"
#include "animation/skeletalPlayback.h"
#include "animation/motionDatabase.h"

#include <cmath>
#include <cstring>
#include <limits>
#include <memory>
#include <stdexcept>
#include <assimp/scene.h>
#include <gtest/gtest.h>

namespace
{
using VL::Animation::AssimpSkeletalImporter;

class SkeletalImport : public ::testing::Test
{
protected:
    void SetUp() override
    {
        scene = std::make_unique<aiScene>();
        scene->mMetaData = aiMetadata::Alloc(7);
        scene->mMetaData->Set(0, "UnitScaleFactor", 1.0);
        scene->mMetaData->Set(1, "UpAxis", 2);
        scene->mMetaData->Set(2, "UpAxisSign", 1);
        scene->mMetaData->Set(3, "FrontAxis", 1);
        scene->mMetaData->Set(4, "FrontAxisSign", -1);
        scene->mMetaData->Set(5, "CoordAxis", 0);
        scene->mMetaData->Set(6, "CoordAxisSign", 1);
        scene->mRootNode = new aiNode("wrapper");
        scene->mRootNode->mNumChildren = 2;
        scene->mRootNode->mChildren = new aiNode*[2];
        auto* root = new aiNode("root");
        scene->mRootNode->mChildren[0] = root;
        root->mParent = scene->mRootNode;
        root->mNumChildren = 9;
        root->mChildren = new aiNode*[9];
        for (int joint = 0; joint < 9; ++joint)
        {
            auto* node = new aiNode("joint" + std::to_string(joint));
            node->mParent = root;
            node->mTransformation.c4 = static_cast<float>((joint + 1) * 10);
            root->mChildren[joint] = node;
        }
        auto* owner = new aiNode("mesh");
        scene->mRootNode->mChildren[1] = owner;
        owner->mParent = scene->mRootNode;
        owner->mNumMeshes = 1;
        owner->mMeshes = new unsigned[1]{0};
        scene->mNumMeshes = 1;
        scene->mMeshes = new aiMesh*[1]{new aiMesh()};
        auto& mesh = *scene->mMeshes[0];
        mesh.mName = "weightedQuad";
        mesh.mNumVertices = 4;
        mesh.mVertices = new aiVector3D[4]{{0, 0, 0}, {1, 0, 0}, {0, 1, 0}, {1, 1, 0}};
        mesh.mNormals = new aiVector3D[4]{{0, 0, 1}, {0, 0, 1}, {0, 0, 1}, {0, 0, 1}};
        mesh.mTextureCoords[0] = new aiVector3D[4]{{0, 0, 0}, {1, 0, 0}, {0, 1, 0}, {1, 1, 0}};
        mesh.mNumUVComponents[0] = 2;
        mesh.mNumFaces = 2;
        mesh.mFaces = new aiFace[2];
        mesh.mFaces[0].mNumIndices = mesh.mFaces[1].mNumIndices = 3;
        mesh.mFaces[0].mIndices = new unsigned[3]{2, 0, 1};
        mesh.mFaces[1].mIndices = new unsigned[3]{2, 1, 3};
        mesh.mNumBones = 9;
        mesh.mBones = new aiBone*[9];
        for (int joint = 0; joint < 9; ++joint)
        {
            auto* bone = new aiBone();
            mesh.mBones[joint] = bone;
            bone->mName = "joint" + std::to_string(joint);
            bone->mOffsetMatrix.c4 = static_cast<float>(-(joint + 1) * 10);
            bone->mNumWeights = 4;
            bone->mWeights = new aiVertexWeight[4];
            for (unsigned vertex = 0; vertex < 4; ++vertex)
            {
                bone->mWeights[vertex] = {vertex, joint == static_cast<int>(vertex) ? 0.2f : 0.1f};
            }
        }
        scene->mNumAnimations = 1;
        scene->mAnimations = new aiAnimation*[1]{new aiAnimation()};
        auto& animation = *scene->mAnimations[0];
        animation.mDuration = 30;
        animation.mTicksPerSecond = 30;
        animation.mNumChannels = 1;
        animation.mChannels = new aiNodeAnim*[1]{new aiNodeAnim()};
        auto& channel = *animation.mChannels[0];
        channel.mNodeName = "root";
        channel.mNumPositionKeys = 2;
        channel.mPositionKeys = new aiVectorKey[2]{{0, {0, 0, 0}}, {30, {0, -100, 0}}};
    }

    std::unique_ptr<aiScene> scene;
};

TEST_F(SkeletalImport, RetainsUnweightedParentAndConvertsCentimetersToYUp)
{
    const auto mesh = AssimpSkeletalImporter::BuildMesh(*scene, "root");
    ASSERT_EQ(mesh.joints.size(), 10u);
    EXPECT_EQ(mesh.joints[0].name, "root");
    EXPECT_EQ(mesh.joints[1].parent, 0);
    EXPECT_NEAR(mesh.joints[1].restModel(1, 3), 10, 0.0001f);
    EXPECT_NEAR(mesh.sections[0].maxBindSkinErrorCm, 0, 0.0001f);
}

TEST_F(SkeletalImport, PreservesNineInfluencesThroughMikkVertexExpansion)
{
    const auto mesh = AssimpSkeletalImporter::BuildMesh(*scene, "root");
    const auto& section = mesh.sections[0];
    const std::vector<uint32_t> expectedSource{2, 0, 1, 2, 1, 3};
    const std::vector<uint32_t> expectedIndices{2, 1, 0, 5, 4, 3};
    EXPECT_EQ(section.sourceVertices, expectedSource);
    EXPECT_EQ(section.indices, expectedIndices);
    EXPECT_EQ(section.maxInfluences, 9u);
    ASSERT_EQ(section.influences.size(), 54u);
    for (size_t vertex = 0; vertex < 6; ++vertex)
    {
        ASSERT_EQ(section.influenceOffsets[vertex + 1] - section.influenceOffsets[vertex], 9u);
        for (uint32_t joint = 0; joint < 9; ++joint)
        {
            const auto& influence = section.influences[section.influenceOffsets[vertex] + joint];
            EXPECT_EQ(influence.joint, joint + 1);
            EXPECT_FLOAT_EQ(influence.weight, joint == expectedSource[vertex] ? 0.2f : 0.1f);
        }
    }
}

TEST_F(SkeletalImport, MissingTracksUseBindPoseAndRootTravelIsPreserved)
{
    const auto mesh = AssimpSkeletalImporter::BuildMesh(*scene, "root");
    const auto clip = AssimpSkeletalImporter::BuildClip(*scene, mesh, "walk");
    EXPECT_EQ(clip.restFallbackJoints.size(), 9u);
    const auto frames = VL::Animation::SampleModels(mesh, clip, {1, 0.5f, 0});
    EXPECT_NEAR(frames[0][0](2, 3), 100, 0.001f);
    EXPECT_NEAR(frames[1][1](1, 3), 10, 0.001f);
    EXPECT_NEAR(frames[1][1](2, 3), 50, 0.001f);
    EXPECT_NEAR(frames[2][9](1, 3), 90, 0.001f);
}

TEST_F(SkeletalImport, MotionFeaturesIncludeRootTravelInJointVelocity)
{
    const auto mesh = AssimpSkeletalImporter::BuildMesh(*scene, "root");
    const auto clip = AssimpSkeletalImporter::BuildClip(*scene, mesh, "walk");
    VL::Animation::FeatureSchema schema;
    schema.joints = {"joint0"};
    const auto values = VL::Animation::ExtractClipFeatures(mesh, clip, schema, 0.25f, true);
    ASSERT_EQ(values.size(), 30u);
    EXPECT_NEAR(values[1], -40, 0.002f);
    EXPECT_NEAR(values[21], 60, 0.002f);
    EXPECT_NEAR(values[24], 0, 0.002f);
    EXPECT_NEAR(values[25], 10, 0.002f);
    EXPECT_NEAR(values[26], 0, 0.002f);
    EXPECT_NEAR(values[29], 100, 0.01f);
}

TEST_F(SkeletalImport, NonloopDatabaseExcludesIncompleteWindows)
{
    const auto mesh = AssimpSkeletalImporter::BuildMesh(*scene, "root");
    std::vector<VL::Animation::Clip> clips;
    clips.push_back(AssimpSkeletalImporter::BuildClip(*scene, mesh, "walk"));
    VL::Animation::FeatureSchema schema;
    schema.joints = {"root"};
    schema.trajectoryTimes = {-0.2f, 0, 0.4f};
    const auto database = VL::Animation::BuildMotionDatabase(mesh, clips, schema, {{0, false}});
    ASSERT_FALSE(database.frames.empty());
    for (const auto& frame : database.frames)
    {
        EXPECT_GE(frame.time, 0.2f);
        EXPECT_LE(frame.time, 0.6f);
    }
    EXPECT_THROW(VL::Animation::ExtractClipFeatures(mesh, clips[0], schema, 0, false), std::invalid_argument);
}

TEST_F(SkeletalImport, LoopExtensionAccumulatesRotationAndNegativeCycles)
{
    auto& channel = *scene->mAnimations[0]->mChannels[0];
    channel.mNumRotationKeys = 2;
    channel.mRotationKeys = new aiQuatKey[2]{{0, aiQuaternion()}, {30, aiQuaternion(0, 0, 0, 1)}};
    const auto mesh = AssimpSkeletalImporter::BuildMesh(*scene, "root");
    const auto clip = AssimpSkeletalImporter::BuildClip(*scene, mesh, "turn");
    const auto negative = VL::Animation::SampleMotionPose(mesh, clip, -1, true).front();
    const auto twice = VL::Animation::SampleMotionPose(mesh, clip, 2, true).front();
    EXPECT_NEAR(negative(2, 3), 100, 0.01f);
    EXPECT_NEAR(negative(0, 0), -1, 0.001f);
    EXPECT_NEAR(twice(2, 3), 0, 0.01f);
    EXPECT_NEAR(twice(0, 0), 1, 0.001f);
}

TEST_F(SkeletalImport, MotionDatabaseUsesStableMemberOrderAndFrozenPopulationStatistics)
{
    const auto mesh = AssimpSkeletalImporter::BuildMesh(*scene, "root");
    std::vector<VL::Animation::Clip> clips;
    clips.push_back(AssimpSkeletalImporter::BuildClip(*scene, mesh, "first"));
    scene->mAnimations[0]->mChannels[0]->mPositionKeys[1].mValue.y = -300;
    clips.push_back(AssimpSkeletalImporter::BuildClip(*scene, mesh, "second"));
    VL::Animation::FeatureSchema schema;
    schema.joints = {"joint0"};
    const auto database = VL::Animation::BuildMotionDatabase(mesh, clips, schema, {{1, true}, {0, true}});
    EXPECT_EQ(database.clipIds, (std::vector<std::string>{"second", "first"}));
    EXPECT_EQ(database.frames.size(), 60u);
    EXPECT_DOUBLE_EQ(database.scales[25], 1);
    EXPECT_NEAR(database.means[29], 200, 0.01);
    EXPECT_NEAR(database.scales[29], 100, 0.01);
    const auto serialized = VL::Animation::MotionDatabaseJson(database);
    EXPECT_EQ(serialized, VL::Animation::MotionDatabaseJson(
        VL::Animation::BuildMotionDatabase(mesh, clips, schema, {{1, true}, {0, true}})));
    const auto restored = VL::Animation::ParseMotionDatabase(serialized);
    EXPECT_EQ(restored.Normalize(database.frames[0].features), database.Normalize(database.frames[0].features));
    EXPECT_THROW(VL::Animation::BuildMotionDatabase(mesh, clips, schema, {{0, true}, {0, false}}),
        std::invalid_argument);
}

TEST_F(SkeletalImport, ConvertsRotationAndScaleWithTheSameBasis)
{
    auto& channel = *scene->mAnimations[0]->mChannels[0];
    channel.mNumRotationKeys = 1;
    channel.mRotationKeys = new aiQuatKey[1]{{0, aiQuaternion(
        std::sqrt(0.5f), 0, 0, std::sqrt(0.5f))}};
    channel.mNumScalingKeys = 1;
    channel.mScalingKeys = new aiVectorKey[1]{{0, {2, 3, 4}}};
    const auto mesh = AssimpSkeletalImporter::BuildMesh(*scene, "root");
    const auto clip = AssimpSkeletalImporter::BuildClip(*scene, mesh, "turn");
    const auto frame = VL::Animation::SampleModels(mesh, clip, {0})[0];
    const Eigen::Vector4f point = frame[0] * Eigen::Vector4f(1, 0, 0, 1);
    EXPECT_NEAR(point.x(), 0, 0.001f);
    EXPECT_NEAR(point.z(), -2, 0.001f);
    EXPECT_NEAR((frame[0].block<3, 1>(0, 1).norm()), 4, 0.001f);
    EXPECT_NEAR((frame[0].block<3, 1>(0, 2).norm()), 3, 0.001f);
}

TEST_F(SkeletalImport, MissingComponentInAuthoredTrackUsesBindTranslation)
{
    const auto mesh = AssimpSkeletalImporter::BuildMesh(*scene, "root");
    auto& animation = *scene->mAnimations[0];
    auto* rootChannel = animation.mChannels[0];
    delete[] animation.mChannels;
    animation.mNumChannels = 2;
    animation.mChannels = new aiNodeAnim*[2]{rootChannel, new aiNodeAnim()};
    auto& child = *animation.mChannels[1];
    child.mNodeName = "joint0";
    child.mNumRotationKeys = 1;
    child.mRotationKeys = new aiQuatKey[1]{{0, aiQuaternion()}};
    const auto clip = AssimpSkeletalImporter::BuildClip(*scene, mesh, "partial-trs");
    EXPECT_EQ(clip.restFallbackJoints.size(), 8u);
    const auto frames = VL::Animation::SampleModels(mesh, clip, {0.5f});
    EXPECT_NEAR(frames[0][1](1, 3), 10, 0.001f);
    EXPECT_NEAR(frames[0][1](2, 3), 50, 0.001f);
}

TEST_F(SkeletalImport, RejectsUnnormalizedWeightsInsteadOfRepairing)
{
    scene->mMeshes[0]->mBones[0]->mWeights[0].mWeight = 0.8f;
    EXPECT_THROW(AssimpSkeletalImporter::BuildMesh(*scene, "root"), std::runtime_error);
}

TEST_F(SkeletalImport, RejectsOutOfRangeVertexWeight)
{
    scene->mMeshes[0]->mBones[0]->mWeights[0].mVertexId = 4;
    EXPECT_THROW(AssimpSkeletalImporter::BuildMesh(*scene, "root"), std::runtime_error);
}

TEST_F(SkeletalImport, RejectsInvalidInverseBind)
{
    scene->mMeshes[0]->mBones[0]->mOffsetMatrix.c4 += 1;
    EXPECT_THROW(AssimpSkeletalImporter::BuildMesh(*scene, "root"), std::runtime_error);
}

TEST_F(SkeletalImport, RejectsDuplicatePhysicalNames)
{
    scene->mRootNode->mChildren[0]->mChildren[1]->mName = "joint0";
    EXPECT_THROW(AssimpSkeletalImporter::BuildMesh(*scene, "root"), std::runtime_error);
}

TEST_F(SkeletalImport, RejectsUnknownAnimatedJoint)
{
    const auto mesh = AssimpSkeletalImporter::BuildMesh(*scene, "root");
    scene->mAnimations[0]->mChannels[0]->mNodeName = "unknown";
    EXPECT_THROW(AssimpSkeletalImporter::BuildClip(*scene, mesh, "invalid"), std::runtime_error);
}

TEST_F(SkeletalImport, RejectsAnimationParentMismatch)
{
    const auto mesh = AssimpSkeletalImporter::BuildMesh(*scene, "root");
    scene->mRootNode->mChildren[0]->mChildren[0]->mParent = scene->mRootNode;
    EXPECT_THROW(AssimpSkeletalImporter::BuildClip(*scene, mesh, "invalid"), std::runtime_error);
}

TEST_F(SkeletalImport, RejectsMissingFbxAxisPolicy)
{
    scene->mMetaData->Set(0, "UnitScaleFactor", 100.0);
    EXPECT_THROW(AssimpSkeletalImporter::BuildMesh(*scene, "root"), std::runtime_error);
}

TEST_F(SkeletalImport, RejectsNonFiniteAnimationValues)
{
    const auto mesh = AssimpSkeletalImporter::BuildMesh(*scene, "root");
    scene->mAnimations[0]->mChannels[0]->mPositionKeys[0].mValue.x =
        std::numeric_limits<float>::quiet_NaN();
    EXPECT_THROW(AssimpSkeletalImporter::BuildClip(*scene, mesh, "invalid"), std::runtime_error);
}

TEST_F(SkeletalImport, RejectsDuplicateKeyTimes)
{
    const auto mesh = AssimpSkeletalImporter::BuildMesh(*scene, "root");
    scene->mAnimations[0]->mChannels[0]->mPositionKeys[1].mTime = 0;
    EXPECT_THROW(AssimpSkeletalImporter::BuildClip(*scene, mesh, "invalid"), std::runtime_error);
}

TEST_F(SkeletalImport, RejectsInvalidSamplingTime)
{
    const auto mesh = AssimpSkeletalImporter::BuildMesh(*scene, "root");
    const auto clip = AssimpSkeletalImporter::BuildClip(*scene, mesh, "walk");
    EXPECT_THROW(VL::Animation::SampleModels(mesh, clip, {-0.1f}), std::invalid_argument);
    EXPECT_THROW(VL::Animation::SampleModels(mesh, clip, {1.1f}), std::invalid_argument);
    EXPECT_THROW(VL::Animation::SampleModels(mesh, clip,
        {std::numeric_limits<float>::quiet_NaN()}), std::invalid_argument);
}

TEST_F(SkeletalImport, AcceptsAssimpFloatUnitMetadata)
{
    scene->mMetaData->Set(0, "UnitScaleFactor", 1.0f);
    EXPECT_NO_THROW(AssimpSkeletalImporter::BuildMesh(*scene, "root"));
}

TEST_F(SkeletalImport, RejectsPerspectiveInverseBind)
{
    scene->mMeshes[0]->mBones[0]->mOffsetMatrix.d1 = 0.5f;
    EXPECT_THROW(AssimpSkeletalImporter::BuildMesh(*scene, "root"), std::runtime_error);
}

TEST_F(SkeletalImport, RejectsPartialAnimationIntervalInsteadOfChangingExtrapolation)
{
    const auto mesh = AssimpSkeletalImporter::BuildMesh(*scene, "root");
    scene->mAnimations[0]->mChannels[0]->mPositionKeys[0].mTime = 1;
    EXPECT_THROW(AssimpSkeletalImporter::BuildClip(*scene, mesh, "partial"), std::runtime_error);
}

TEST_F(SkeletalImport, RejectsNonUnitAnimationRotation)
{
    const auto mesh = AssimpSkeletalImporter::BuildMesh(*scene, "root");
    auto& channel = *scene->mAnimations[0]->mChannels[0];
    channel.mNumRotationKeys = 1;
    channel.mRotationKeys = new aiQuatKey[1]{{0, aiQuaternion(2, 0, 0, 0)}};
    EXPECT_THROW(AssimpSkeletalImporter::BuildClip(*scene, mesh, "invalid"), std::runtime_error);
}

TEST_F(SkeletalImport, RejectsVirtualBoneAsPhysicalInput)
{
    scene->mRootNode->mChildren[0]->mChildren[0]->mName = "VB virtual";
    EXPECT_THROW(AssimpSkeletalImporter::BuildMesh(*scene, "root"), std::runtime_error);
}

TEST_F(SkeletalImport, SkinPaletteUsesAllNineInfluencesAndDeformedBounds)
{
    auto mesh = AssimpSkeletalImporter::BuildMesh(*scene, "root");
    std::vector<Eigen::Matrix4f> models;
    for (const auto& joint : mesh.joints)
    {
        Eigen::Matrix4f transformed = joint.restModel;
        transformed(2, 3) += 50;
        models.push_back(transformed);
    }
    const auto& section = mesh.sections.front();
    const auto pose = VL::Animation::BuildSkinPose(section, models, 7);
    Eigen::Vector3f expectedMin = Eigen::Vector3f::Constant(std::numeric_limits<float>::max());
    Eigen::Vector3f expectedMax = -expectedMin;
    for (size_t i = 0; i < section.vertices.size(); ++i)
    {
        const auto matrix = VL::Animation::BlendSkinMatrix(section, i, pose->matrices);
        const Eigen::Vector3f p = (matrix * section.vertices[i].position.homogeneous()).head<3>();
        EXPECT_NEAR(p.z(), section.vertices[i].position.z() + 50, 0.001);
        EXPECT_TRUE((p.array() >= pose->boundsMin.array()).all());
        EXPECT_TRUE((p.array() <= pose->boundsMax.array()).all());
        expectedMin = expectedMin.cwiseMin(p);
        expectedMax = expectedMax.cwiseMax(p);
    }
    EXPECT_TRUE(pose->boundsMin.isApprox(expectedMin, 1e-6f));
    EXPECT_TRUE(pose->boundsMax.isApprox(expectedMax, 1e-6f));
    EXPECT_EQ(pose->discontinuity, 7u);
    const auto words = VL::Animation::PackSkinInfluences(section);
    EXPECT_EQ(words[1] - words[0], 18u);
    EXPECT_EQ(words.size(), section.vertices.size() + 1 + 2 * section.influences.size());
    for (size_t v = 0; v < section.vertices.size(); ++v)
    {
        Eigen::Matrix4f gpuLayout = Eigen::Matrix4f::Zero();
        for (uint32_t i = words[v]; i < words[v + 1]; i += 2)
        {
            float weight;
            std::memcpy(&weight, &words[i + 1], 4);
            gpuLayout += weight * pose->matrices.at(words[i]);
        }
        EXPECT_TRUE(gpuLayout.isApprox(VL::Animation::BlendSkinMatrix(section, v, pose->matrices), 1e-6f));
    }
}

TEST_F(SkeletalImport, PlaybackPauseSeekLoopAndFrozenPose)
{
    auto asset = std::make_shared<VL::Animation::SkeletalAsset>();
    asset->mesh = AssimpSkeletalImporter::BuildMesh(*scene, "root");
    asset->clips.push_back(AssimpSkeletalImporter::BuildClip(*scene, asset->mesh, "walk"));
    VL::Animation::SkeletalPlayback player(asset);
    const auto initial = player.Pose(0);
    player.Tick(0.5f);
    const auto moving = player.Pose(0);
    EXPECT_NEAR(moving->matrices[0](2, 3), 50, 0.001);
    EXPECT_NEAR(initial->matrices[0](2, 3), 0, 0.001);
    player.SetPlaying(false);
    player.Tick(0.25f);
    EXPECT_EQ(player.Pose(0), moving);
    player.Seek(1);
    EXPECT_NEAR(player.Pose(0)->matrices[0](2, 3), 100, 0.001);
    EXPECT_NE(player.Pose(0)->discontinuity, moving->discontinuity);
    player.SetPlaying(true);
    player.Tick(0.25f);
    EXPECT_FLOAT_EQ(player.Time(), 0.25f);
    EXPECT_THROW(player.Seek(-1), std::invalid_argument);
    EXPECT_THROW(player.Tick(std::numeric_limits<float>::infinity()), std::invalid_argument);
    EXPECT_THROW(player.Select(2), std::out_of_range);
}

TEST(SkinRenderHistory, DroppedSnapshotsDoNotAdvanceHistory)
{
    VL::Animation::SubmittedSkinPose history;
    auto submitted = std::make_shared<VL::Animation::SkinPose>();
    submitted->discontinuity = 3;
    auto skipped = std::make_shared<VL::Animation::SkinPose>(*submitted);
    auto current = std::make_shared<VL::Animation::SkinPose>(*submitted);
    Eigen::Matrix4f model = Eigen::Matrix4f::Identity();
    model(0, 3) = 4;
    history.Commit(submitted, model, 1);
    EXPECT_EQ(&history.Previous(*current), submitted.get());
    EXPECT_NE(&history.Previous(*current), skipped.get());
    EXPECT_FLOAT_EQ(history.PreviousModel(*current, Eigen::Matrix4f::Identity())(0, 3), 4);
    current->discontinuity = 4;
    EXPECT_EQ(&history.Previous(*current), current.get());
    EXPECT_TRUE(history.PreviousModel(*current, Eigen::Matrix4f::Identity()).isIdentity());
    history.Commit(current, Eigen::Matrix4f::Identity(), 2);
    EXPECT_EQ(history.pose, current);
}

TEST_F(SkeletalImport, InPlaceRemovesRootTravelRotationAndScale)
{
    auto& channel = *scene->mAnimations[0]->mChannels[0];
    channel.mNumRotationKeys = 1;
    channel.mRotationKeys = new aiQuatKey[1]{{0, aiQuaternion(std::sqrt(0.5f), 0, 0, std::sqrt(0.5f))}};
    channel.mNumScalingKeys = 1;
    channel.mScalingKeys = new aiVectorKey[1]{{0, {2, 2, 2}}};
    auto asset = std::make_shared<VL::Animation::SkeletalAsset>();
    asset->mesh = AssimpSkeletalImporter::BuildMesh(*scene, "root");
    asset->clips.push_back(AssimpSkeletalImporter::BuildClip(*scene, asset->mesh, "walk"));
    VL::Animation::SkeletalPlayback player(asset);
    player.Play("walk", true, 1, true);
    player.Tick(0.5f);
    const auto& matrices = player.Pose(0)->matrices;
    EXPECT_TRUE(matrices.front().isIdentity(0.00001f));
    for (const auto& matrix : matrices)
    {
        // 根保留 RAW 精度；子骨仍经过 ozz 四元数压缩。分别限制线性误差和厘米误差，
        // 避免把不同量纲混为一个矩阵容差；0.01 cm 严于正式导入的 0.1 cm 门限。
        EXPECT_TRUE((matrix.block<3, 3>(0, 0).isIdentity(0.0001f))) << matrix;
        EXPECT_LT((matrix.block<3, 1>(0, 3).norm()), 0.01f) << matrix;
    }
}

TEST_F(SkeletalImport, OneShotFinishesOnceAtEndAndReportsCompletion)
{
    auto asset = std::make_shared<VL::Animation::SkeletalAsset>();
    asset->mesh = AssimpSkeletalImporter::BuildMesh(*scene, "root");
    asset->clips.push_back(AssimpSkeletalImporter::BuildClip(*scene, asset->mesh, "jump"));
    VL::Animation::SkeletalPlayback player(asset);
    player.Play("jump", false, 2, false);
    player.Tick(0.25f);
    EXPECT_FLOAT_EQ(player.Time(), 0.5f);
    EXPECT_FALSE(player.Completed());
    player.Tick(1);
    EXPECT_FLOAT_EQ(player.Time(), 1);
    EXPECT_TRUE(player.Completed());
    EXPECT_FALSE(player.IsPlaying());
    const auto end = player.Pose(0);
    EXPECT_NEAR(end->matrices[0](2, 3), 100, 0.001f);
    player.Tick(1);
    EXPECT_EQ(player.Pose(0), end);
}

TEST_F(SkeletalImport, InvalidPlayKeepsAllActivePlaybackValues)
{
    auto asset = std::make_shared<VL::Animation::SkeletalAsset>();
    asset->mesh = AssimpSkeletalImporter::BuildMesh(*scene, "root");
    asset->clips.push_back(AssimpSkeletalImporter::BuildClip(*scene, asset->mesh, "walk"));
    VL::Animation::SkeletalPlayback player(asset);
    player.Tick(0.25f);
    const auto pose = player.Pose(0);
    EXPECT_THROW(player.Play("missing", false, 1, true), std::invalid_argument);
    EXPECT_THROW(player.Play("walk", false, 0, true), std::invalid_argument);
    EXPECT_THROW(player.Play("walk", false, std::numeric_limits<float>::infinity(), true), std::invalid_argument);
    EXPECT_EQ(player.Pose(0), pose);
    EXPECT_FLOAT_EQ(player.Time(), 0.25f);
    EXPECT_FALSE(player.InPlace());
    EXPECT_FLOAT_EQ(player.Rate(), 1);
}

TEST_F(SkeletalImport, NewInstanceSharesAssetButHasIndependentClockAndHistory)
{
    auto asset = std::make_shared<VL::Animation::SkeletalAsset>();
    asset->mesh = AssimpSkeletalImporter::BuildMesh(*scene, "root");
    asset->clips.push_back(AssimpSkeletalImporter::BuildClip(*scene, asset->mesh, "walk"));
    VL::Animation::SkeletalPlayback player(asset);
    player.Tick(0.25f);
    auto clone = player.NewInstance();
    EXPECT_EQ(&player.Asset(), &clone->Asset());
    EXPECT_FLOAT_EQ(clone->Time(), 0);
    EXPECT_NE(clone->Pose(0)->playbackIdentity, player.Pose(0)->playbackIdentity);
    clone->Tick(0.5f);
    EXPECT_FLOAT_EQ(player.Time(), 0.25f);
    VL::Animation::SubmittedSkinPose history;
    history.Commit(player.Pose(0), Eigen::Matrix4f::Identity(), 1);
    EXPECT_EQ(&history.Previous(*clone->Pose(0)), clone->Pose(0).get());
}

TEST(SkinRenderHistory, ReappearingDrawResetsHistoryAfterMissingASubmission)
{
    VL::Animation::SubmittedSkinPose history;
    auto pose = std::make_shared<VL::Animation::SkinPose>();
    history.Commit(pose, Eigen::Matrix4f::Identity(), 8);
    history.RetainIfSubmittedAt(8);
    EXPECT_EQ(history.pose, pose);
    history.RetainIfSubmittedAt(9);
    EXPECT_FALSE(history.pose);
}
}
