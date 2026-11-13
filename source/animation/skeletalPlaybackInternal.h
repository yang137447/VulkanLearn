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

// UE AAT_LocalSpaceBase：delta = additive * inverse(reference)，结果 = base * delta。
// 权重按 UE 的 BlendFromIdentityAndAccumulate 语义把增量向 identity 收缩：
// translation *= w、rotation = slerp(identity, delta, w)、scale = 1 + (delta-1)*w。
inline LocalPose ApplyLocalSpaceAdditive(const LocalPose& base, const LocalPose& additive,
    const LocalPose& reference, float alpha, size_t jointCount)
{
    if (alpha <= 0 || jointCount == 0) return base;
    auto target = ReadKinematics(base, jointCount);
    const auto add = ReadKinematics(additive, jointCount);
    const auto ref = ReadKinematics(reference, jointCount);
    const Eigen::Vector3f identityScale = Eigen::Vector3f::Ones();
    for (size_t joint = 0; joint < jointCount; ++joint)
    {
        const Eigen::Quaternionf referenceInverse = ref[joint].rotation.conjugate().normalized();
        const Eigen::Vector3f referenceTranslationInverse = -(referenceInverse * ref[joint].translation);
        const Eigen::Vector3f referenceScaleInverse =
            ref[joint].scale.cwiseAbs().cwiseMax(Eigen::Vector3f::Constant(1e-6f)).cwiseInverse();
        const Eigen::Quaternionf deltaRotation =
            (add[joint].rotation * referenceInverse).normalized();
        const Eigen::Vector3f deltaTranslation =
            add[joint].translation + add[joint].rotation * referenceTranslationInverse;
        const Eigen::Vector3f deltaScale = add[joint].scale.cwiseProduct(referenceScaleInverse);
        // 逐通道向 identity 收缩；旋转用球面插值，避免平移/缩放被旋转耦合。
        const Eigen::Quaternionf weightedRotation =
            Eigen::Quaternionf::Identity().slerp(alpha, deltaRotation).normalized();
        const Eigen::Vector3f weightedTranslation = deltaTranslation * alpha;
        const Eigen::Vector3f weightedScale =
            identityScale + (deltaScale - identityScale) * alpha;
        target[joint].translation += target[joint].rotation * weightedTranslation;
        target[joint].rotation = (target[joint].rotation * weightedRotation).normalized();
        target[joint].scale = target[joint].scale.cwiseProduct(weightedScale);
    }
    return WriteKinematics(base, target);
}

// 骨骼 mask 分层：只对 mask 命中的关节把 base 向 overlay 混合；未命中关节保持原值。
inline LocalPose ApplyLayeredPose(const LocalPose& base, const LocalPose& overlay,
    const std::vector<uint8_t>& mask, float alpha, size_t jointCount)
{
    if (alpha <= 0 || jointCount == 0) return base;
    auto target = ReadKinematics(base, jointCount);
    const auto layer = ReadKinematics(overlay, jointCount);
    const float weight = std::clamp(alpha, 0.0f, 1.0f);
    for (size_t joint = 0; joint < jointCount; ++joint)
    {
        if (joint >= mask.size() || mask[joint] == 0) continue;
        target[joint].translation =
            target[joint].translation * (1.0f - weight) + layer[joint].translation * weight;
        target[joint].rotation =
            target[joint].rotation.slerp(weight, layer[joint].rotation).normalized();
        target[joint].scale =
            target[joint].scale * (1.0f - weight) + layer[joint].scale * weight;
    }
    return WriteKinematics(base, target);
}

// UE AAT_RotationOffsetMeshSpace：先把基础姿态的局部旋转累乘成 mesh-space 旋转，
// 在 mesh 空间按 identity→delta 的权重累加，再按修改后的父链转回局部。
// delta = additiveMesh * inverse(referenceMesh)；平移/缩放不受影响。
inline LocalPose ApplyMeshSpaceRotationAdditive(const LocalPose& base, const LocalPose& additive,
    const LocalPose& reference, float alpha, const std::vector<Joint>& joints)
{
    if (alpha <= 0 || joints.empty()) return base;
    const size_t jointCount = joints.size();
    auto kinematics = ReadKinematics(base, jointCount);
    const auto add = ReadKinematics(additive, jointCount);
    const auto ref = ReadKinematics(reference, jointCount);
    std::vector<Eigen::Quaternionf> baseMesh(jointCount);
    std::vector<Eigen::Quaternionf> addMesh(jointCount);
    std::vector<Eigen::Quaternionf> refMesh(jointCount);
    std::vector<Eigen::Quaternionf> resultMesh(jointCount);
    for (size_t joint = 0; joint < jointCount; ++joint)
    {
        const int parent = joints[joint].parent;
        baseMesh[joint] = (parent >= 0 ? baseMesh[static_cast<size_t>(parent)] * kinematics[joint].rotation
            : kinematics[joint].rotation).normalized();
        addMesh[joint] = (parent >= 0 ? addMesh[static_cast<size_t>(parent)] * add[joint].rotation
            : add[joint].rotation).normalized();
        refMesh[joint] = (parent >= 0 ? refMesh[static_cast<size_t>(parent)] * ref[joint].rotation
            : ref[joint].rotation).normalized();
    }
    const float weight = std::clamp(alpha, 0.0f, 1.0f);
    for (size_t joint = 0; joint < jointCount; ++joint)
    {
        const Eigen::Quaternionf delta =
            (addMesh[joint] * refMesh[joint].conjugate()).normalized();
        const Eigen::Quaternionf weighted =
            Eigen::Quaternionf::Identity().slerp(weight, delta).normalized();
        resultMesh[joint] = (baseMesh[joint] * weighted).normalized();
    }
    for (size_t joint = 0; joint < jointCount; ++joint)
    {
        const int parent = joints[joint].parent;
        kinematics[joint].rotation = (parent >= 0
            ? resultMesh[static_cast<size_t>(parent)].conjugate() * resultMesh[joint]
            : resultMesh[joint]).normalized();
    }
    return WriteKinematics(base, kinematics);
}

// BlendProfile：weights 是每关节原始权重（未列出的骨骼 1.0），最终权重 = alpha * weight。
inline LocalPose ApplyLayeredWeights(const LocalPose& base, const LocalPose& overlay,
    const std::vector<float>& weights, float alpha, size_t jointCount)
{
    if (alpha <= 0 || jointCount == 0) return base;
    auto target = ReadKinematics(base, jointCount);
    const auto layer = ReadKinematics(overlay, jointCount);
    for (size_t joint = 0; joint < jointCount; ++joint)
    {
        const float weight = std::clamp(
            alpha * (joint < weights.size() ? weights[joint] : 1.0f), 0.0f, 1.0f);
        if (weight <= 0) continue;
        target[joint].translation =
            target[joint].translation * (1.0f - weight) + layer[joint].translation * weight;
        target[joint].rotation =
            target[joint].rotation.slerp(weight, layer[joint].rotation).normalized();
        target[joint].scale =
            target[joint].scale * (1.0f - weight) + layer[joint].scale * weight;
    }
    return WriteKinematics(base, target);
}
}
