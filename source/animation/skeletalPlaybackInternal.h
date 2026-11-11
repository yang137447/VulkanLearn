#pragma once

// Helpers shared by the SkeletalPlayback translation units.
//
// These were file-local (anonymous namespace) in the monolithic
// skeletalPlayback.cpp. They are inline here so every TU sees one definition
// instead of owning a private copy; the split only moves definitions, it does
// not change evaluation order or math.

#include "skeletalPlayback.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <stdexcept>
#include <vector>

#include <ozz/animation/offline/raw_animation_utils.h>
#include <ozz/animation/runtime/blending_job.h>

namespace VL::Animation
{
inline float ReadLane(ozz::math::SimdFloat4 vector, size_t lane)
{
    float values[4];
    ozz::math::StorePtrU(vector, values);
    return values[lane];
}

inline Eigen::Vector3f RootTranslation(const Clip& clip, float time)
{
    return SampleRootMotionTranslation(clip, time);
}

inline Eigen::Quaternionf RootRotation(const Clip& clip, float time)
{
    ozz::math::Transform root;
    if (clip.raw.tracks.empty() ||
        !ozz::animation::offline::SampleTrack(clip.raw.tracks[0], time, &root))
        return Eigen::Quaternionf::Identity();
    return Eigen::Quaternionf(root.rotation.w, root.rotation.x, root.rotation.y,
        root.rotation.z).normalized();
}

inline float YawFromQuaternion(const Eigen::Quaternionf& q)
{
    return std::atan2(2.0f * (q.w() * q.y() + q.x() * q.z()),
        1.0f - 2.0f * (q.y() * q.y() + q.x() * q.x()));
}

inline void WriteLane(ozz::math::SimdFloat4& vector, size_t lane, float value)
{
    float values[4];
    ozz::math::StorePtrU(vector, values);
    values[lane] = value;
    vector = ozz::math::simd_float4::LoadPtrU(values);
}

inline std::vector<JointKinematics> ReadKinematics(const LocalPose& locals, size_t jointCount)
{
    std::vector<JointKinematics> result(jointCount);
    for (size_t joint = 0; joint < jointCount; ++joint)
    {
        const auto& source = locals[joint / 4];
        const size_t lane = joint % 4;
        auto& output = result[joint];
        output.translation = {ReadLane(source.translation.x, lane), ReadLane(source.translation.y, lane),
            ReadLane(source.translation.z, lane)};
        output.rotation = Eigen::Quaternionf(ReadLane(source.rotation.w, lane), ReadLane(source.rotation.x, lane),
            ReadLane(source.rotation.y, lane), ReadLane(source.rotation.z, lane)).normalized();
        output.scale = {ReadLane(source.scale.x, lane), ReadLane(source.scale.y, lane), ReadLane(source.scale.z, lane)};
    }
    return result;
}

inline LocalPose WriteKinematics(LocalPose result, const std::vector<JointKinematics>& joints)
{
    for (size_t joint = 0; joint < joints.size(); ++joint)
    {
        auto& target = result[joint / 4];
        const size_t lane = joint % 4;
        const auto& value = joints[joint];
        WriteLane(target.translation.x, lane, value.translation.x());
        WriteLane(target.translation.y, lane, value.translation.y());
        WriteLane(target.translation.z, lane, value.translation.z());
        WriteLane(target.rotation.x, lane, value.rotation.x());
        WriteLane(target.rotation.y, lane, value.rotation.y());
        WriteLane(target.rotation.z, lane, value.rotation.z());
        WriteLane(target.rotation.w, lane, value.rotation.w());
        WriteLane(target.scale.x, lane, value.scale.x());
        WriteLane(target.scale.y, lane, value.scale.y());
        WriteLane(target.scale.z, lane, value.scale.z());
    }
    return result;
}

inline std::vector<JointKinematics> MeasureKinematics(const LocalPose& pose, const LocalPose& future,
    size_t jointCount, float step)
{
    auto result = ReadKinematics(pose, jointCount);
    const auto next = ReadKinematics(future, jointCount);
    for (size_t joint = 0; joint < jointCount; ++joint)
    {
        result[joint].linearVelocity = (next[joint].translation - result[joint].translation) / step;
        result[joint].angularVelocity = RotationLog(next[joint].rotation * result[joint].rotation.conjugate()) / step;
        result[joint].logScaleVelocity =
            (next[joint].scale.array().log() - result[joint].scale.array().log()).matrix() / step;
    }
    return result;
}

inline LocalPose Crossfade(const LocalPose& source, LocalPose target, float alpha, const SkeletonMesh& mesh)
{
    LocalPose mixed(target.size());
    ozz::animation::BlendingJob::Layer layers[2];
    layers[0].transform = ozz::make_span(source);
    layers[0].weight = 1 - alpha;
    layers[1].transform = ozz::make_span(target);
    layers[1].weight = alpha;
    ozz::animation::BlendingJob blend;
    blend.layers = ozz::make_span(layers);
    blend.rest_pose = mesh.runtime->joint_rest_poses();
    blend.output = ozz::make_span(mixed);
    if (!blend.Run()) throw std::runtime_error("Animation local pose blending failed");
    return mixed;
}
}
