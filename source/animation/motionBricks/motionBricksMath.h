#pragma once

#include "motionBricksContract.h"

#include <Eigen/Core>
#include <Eigen/Geometry>

#include <array>
#include <cstdint>
#include <string>
#include <vector>

namespace VL::Animation::MotionBricks
{
// Position in meters and a unit quaternion, matching Warp's wp.transform. The SOMA
// model space is Y-up with XZ as the ground plane. VulkanLearn converts to/from
// centimeters at the engine boundary; this module deliberately stays in model units.
struct Transform
{
    Eigen::Vector3f position = Eigen::Vector3f::Zero();
    Eigen::Quaternionf rotation = Eigen::Quaternionf::Identity();
};

using Pose = std::vector<Transform>;
using PoseFrames = std::vector<Pose>;

Transform Compose(const Transform& parent, const Transform& local);
Transform Inverse(const Transform& value);
Transform Blend(const Transform& from, const Transform& to, float alpha);
Transform Normalized(Transform value);

// The SOMA model's 30 joints mapped onto a runtime skeleton that may have extra
// joints (fingers, helpers). modelToPose[modelJoint] is the pose joint index;
// poseToModel[poseJoint] is the model joint or -1. A mapped model joint always has
// a mapped parent, mirroring MotionBricksSOMAEngine::_bind_pose_skeleton().
struct SkeletonBinding
{
    std::vector<std::string> jointNames;
    std::vector<int> parentIndices;
    Pose referenceLocalPose;
    std::vector<int> modelToPose;
    std::vector<int> poseToModel;
    int rightLegJoint = -1;
    int leftLegJoint = -1;
};

SkeletonBinding BindSomaSkeleton(const ModelContract& contract,
    std::vector<std::string> jointNames,
    std::vector<int> parentIndices,
    Pose referenceLocalPose);

struct EncodeConfig
{
    std::vector<int> allowedTokenCounts{8, 9, 10, 11, 12, 13, 14};
    // Ground direction the last context pose is turned to face, in model space
    // (x, z). The default faces +X. It must be finite and non-zero.
    Eigen::Vector2f headingAlignment = Eigen::Vector2f(1.0f, 0.0f);
    // Rigid transform from model space to runtime space. SOMA's model is meters/Y-up;
    // a centimeter engine applies scale outside this rigid transform.
    Transform modelToRuntime;
};

struct EncodedInputs
{
    Transform alignment;
    std::array<std::array<float, 5>, kContextAndTargetPoses> globalRootValues{};
    std::array<std::array<float, 4>, kContextAndTargetPoses> localRootValues{};
    std::array<std::array<float, 267>, kContextAndTargetPoses> localPoses{};
    std::array<int32_t, kContextAndTargetPoses> hasGlobalRootValues{};
    std::array<int32_t, kContextAndTargetPoses> hasLocalRootValues{};
    std::array<int32_t, kContextAndTargetPoses> hasLocalPoses{};
    std::array<int32_t, kMaxTokenCount - kMinTokenCount + 1> allowedPredNumTokens{};
};

// context holds exactly four poses in time order, ending at the current pose.
// targetPoses holds one to four poses in time order, starting at the pose to reach.
// Both are local poses in runtime space; this function composes them into model space.
EncodedInputs EncodeSomaInputs(const SkeletonBinding& binding,
    const std::array<Pose, kContextPoses>& context,
    const std::vector<Pose>& targetPoses,
    const EncodeConfig& config);

struct DecodedMotion
{
    PoseFrames frames;
    std::vector<std::array<float, 4>> footContacts;
};

// prediction is the model's [64, 366] output. tokenCount selects how many valid
// frames remain after the three boundary frames at each end. Rotations are decoded
// from the global 6D rotations at offset 92; joint positions are taken from the
// binding's reference local pose, exactly like the official SOMA engine.
DecodedMotion DecodeSomaOutput(const SkeletonBinding& binding,
    const Transform& alignment,
    const Transform& modelToRuntime,
    const std::vector<std::array<float, 366>>& prediction,
    int tokenCount);

struct EndpointDeltas
{
    std::vector<Transform> start;
    std::vector<Transform> end;
};

// Start deltas move the first generated pose to contextLast, end deltas move the
// last generated pose to targetFirst.
EndpointDeltas ComputeEndpointDeltas(const SkeletonBinding& binding,
    const Pose& contextLast, const Pose& targetFirst, const PoseFrames& generated);

// Fades the start correction out and the end correction in. Root positions blend
// linearly; rotations use weighted slerps.
void ApplyEndpointCorrection(const SkeletonBinding& binding, PoseFrames& generated,
    const EndpointDeltas& deltas);

void CorrectEndpoints(const SkeletonBinding& binding, const Pose& contextLast,
    const Pose& targetFirst, PoseFrames& generated);

Pose ComputeWorldPose(const SkeletonBinding& binding, const Pose& local,
    const Transform& rootTransform = {});
PoseFrames ComputeWorldPoses(const SkeletonBinding& binding, const PoseFrames& local,
    const Transform& rootTransform = {});

// Samples between the two nearest frames. Times outside the animation clamp. The
// root gets rootOffset pre-multiplied after blending, matching AnimationBuffer.sample().
Pose SamplePose(const PoseFrames& frames, float time, float sampleRate = kSampleRate,
    const Transform& rootOffset = {});
std::array<float, 4> SampleFootContacts(
    const std::vector<std::array<float, 4>>& contacts, float time,
    float sampleRate = kSampleRate);
}
