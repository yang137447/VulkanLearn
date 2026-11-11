#include "slopeWarping.h"

#include <algorithm>
#include <cmath>
#include <limits>

namespace VL::Animation
{
namespace
{
constexpr float Pi = 3.14159265358979323846f;
constexpr float DegreesToRadians = Pi / 180.0f;
constexpr float RadiansToDegrees = 180.0f / Pi;

float ReadLane(ozz::math::SimdFloat4 vector, size_t lane)
{
    float values[4];
    ozz::math::StorePtrU(vector, values);
    return values[lane];
}

void WriteLane(ozz::math::SimdFloat4& vector, size_t lane, float value)
{
    float values[4];
    ozz::math::StorePtrU(vector, values);
    values[lane] = value;
    vector = ozz::math::simd_float4::LoadPtrU(values);
}

Eigen::Vector3f Translation(const LocalPose& locals, size_t joint)
{
    const auto& value = locals[joint / 4].translation;
    const size_t lane = joint % 4;
    return {ReadLane(value.x, lane), ReadLane(value.y, lane), ReadLane(value.z, lane)};
}

Eigen::Quaternionf Rotation(const LocalPose& locals, size_t joint)
{
    const auto& value = locals[joint / 4].rotation;
    const size_t lane = joint % 4;
    return Eigen::Quaternionf(ReadLane(value.w, lane), ReadLane(value.x, lane),
        ReadLane(value.y, lane), ReadLane(value.z, lane)).normalized();
}

void SetTranslation(LocalPose& locals, size_t joint, const Eigen::Vector3f& translation)
{
    auto& value = locals[joint / 4].translation;
    const size_t lane = joint % 4;
    WriteLane(value.x, lane, translation.x());
    WriteLane(value.y, lane, translation.y());
    WriteLane(value.z, lane, translation.z());
}

void SetRotation(LocalPose& locals, size_t joint, const Eigen::Quaternionf& rotation)
{
    auto& value = locals[joint / 4].rotation;
    const size_t lane = joint % 4;
    WriteLane(value.x, lane, rotation.x());
    WriteLane(value.y, lane, rotation.y());
    WriteLane(value.z, lane, rotation.z());
    WriteLane(value.w, lane, rotation.w());
}

Eigen::Vector3f Position(const Eigen::Matrix4f& matrix)
{
    return matrix.block<3, 1>(0, 3);
}

Eigen::Vector3f TransformPoint(const Eigen::Matrix4f& matrix, const Eigen::Vector3f& point)
{
    return (matrix * point.homogeneous()).head<3>();
}

bool TryFindJoint(const SkeletonMesh& mesh, const char* name, size_t& index)
{
    for (size_t joint = 0; joint < mesh.joints.size(); ++joint)
    {
        if (mesh.joints[joint].name == name)
        {
            index = joint;
            return true;
        }
    }
    return false;
}

bool FiniteNormal(const Eigen::Vector3f& normal)
{
    return normal.allFinite() && normal.norm() > 0.0001f;
}
}

SlopeWarpingSolver::SlopeWarpingSolver(const SkeletonMesh& mesh)
{
    if (mesh.joints.empty())
    {
        missingJoint = "root";
        state.reason = "missing-joint:root";
        return;
    }
    // 导入器把骨架根放在首关节；仍按 parent=-1 解析，避免测试/离线夹具顺序不同。
    root = 0;
    for (size_t joint = 0; joint < mesh.joints.size(); ++joint)
    {
        if (mesh.joints[joint].parent < 0)
        {
            root = joint;
            break;
        }
    }
    if (!TryFindJoint(mesh, "pelvis", pelvis))
    {
        missingJoint = "pelvis";
        state.reason = "missing-joint:pelvis";
        return;
    }
    for (const char* name : {"foot_l", "foot_r"})
    {
        size_t foot = 0;
        if (!TryFindJoint(mesh, name, foot))
        {
            missingJoint = name;
            state.reason = std::string("missing-joint:") + name;
            return;
        }
        feet.push_back(foot);
    }
    valid = true;
}

void SlopeWarpingSolver::SetEnabled(bool value)
{
    if (state.enabled == value) return;
    state.enabled = value;
    if (value)
    {
        Reset(valid ? "warming-up" : "");
    }
    else
    {
        Reset("disabled");
        state.enabled = false;
    }
}

void SlopeWarpingSolver::SetParameters(const SlopeWarpingParameters& value)
{
    parameters.weight = std::clamp(value.weight, 0.0f, 1.0f);
    parameters.maxSlopeDegrees = std::clamp(value.maxSlopeDegrees, 0.0f, 89.0f);
    parameters.pelvisHeightOffset = std::clamp(value.pelvisHeightOffset, -200.0f, 200.0f);
    parameters.footCompensation = std::clamp(value.footCompensation, 0.0f, 2.0f);
    parameters.responseSpeed = std::clamp(value.responseSpeed, 0.0f, 120.0f);
    parameters.maxPelvisOffset = std::clamp(value.maxPelvisOffset, 0.0f, 200.0f);
    parameters.flatAngleDegrees = std::clamp(value.flatAngleDegrees, 0.0f, 15.0f);
}

void SlopeWarpingSolver::Reset(const char* reason)
{
    const bool enabled = state.enabled;
    const std::string requested = reason ? reason : "";
    state = {};
    state.enabled = enabled;
    state.reason = valid ? requested : std::string("missing-joint:") + missingJoint;
}

LocalPose SlopeWarpingSolver::Solve(const SkeletonMesh& mesh, const LocalPose& locals,
    const std::vector<Eigen::Matrix4f>& models, const Eigen::Matrix4f& component,
    bool grounded, const std::string& owner, float deltaSeconds,
    const GroundProbe& probe, bool& changed)
{
    changed = false;
    if (!state.enabled)
    {
        state.active = false;
        state.weight = 0;
        state.reason = "disabled";
        return locals;
    }
    if (!valid)
    {
        state.active = false;
        state.reason = std::string("missing-joint:") + missingJoint;
        return locals;
    }
    if (!grounded)
    {
        state.active = false;
        state.weight = 0;
        state.reason = "airborne";
        return locals;
    }
    if (!(deltaSeconds > 0) || !std::isfinite(deltaSeconds))
    {
        state.active = false;
        state.reason = "paused";
        return locals;
    }
    if (deltaSeconds > 0.25f)
    {
        state.active = false;
        state.weight = 0;
        state.reason = "large-delta";
        return locals;
    }
    if (models.size() != mesh.joints.size() || locals.size() != (mesh.joints.size() + 3) / 4)
    {
        state.active = false;
        state.reason = "invalid-pose";
        return locals;
    }

    // 先查骨盆附近，失败再查双脚。GroundProbe 已由调用方过滤 owner/动态支撑。
    FootGroundHit hit;
    const Eigen::Vector3f pelvisWorld = TransformPoint(component, Position(models[pelvis]));
    hit = probe.ProbeGround(owner, pelvisWorld, Eigen::Vector3f(0, -300, 0));
    if (!(hit.hit && hit.supportIdentity.size() > 0 && FiniteNormal(hit.normal)))
    {
        hit = {};
        for (size_t foot : feet)
        {
            const Eigen::Vector3f footWorld = TransformPoint(component, Position(models[foot]));
            const FootGroundHit candidate = probe.ProbeGround(
                owner, footWorld + Eigen::Vector3f(0, 30, 0), Eigen::Vector3f(0, -90, 0));
            if (candidate.hit && candidate.supportIdentity.size() > 0 && FiniteNormal(candidate.normal))
            {
                hit = candidate;
                break;
            }
        }
    }
    if (!hit.hit || !FiniteNormal(hit.normal))
    {
        state.active = false;
        state.weight = 0;
        state.reason = "no-support";
        return locals;
    }

    const Eigen::Vector3f normal = hit.normal.normalized();
    const Eigen::Matrix3f componentLinear = component.block<3, 3>(0, 0);
    const float componentScale = componentLinear.col(0).norm();
    if (!(componentScale > 0.0001f) || !componentLinear.allFinite())
    {
        state.active = false;
        state.reason = "invalid-component";
        return locals;
    }
    const Eigen::Matrix3f componentRotation = componentLinear / componentScale;
    const Eigen::Vector3f up = (componentRotation * Eigen::Vector3f::UnitY()).normalized();
    const float dot = std::clamp(up.dot(normal), -1.0f, 1.0f);
    const float slopeAngle = std::acos(dot);
    state.slopeAngleDegrees = slopeAngle * RadiansToDegrees;
    state.groundNormal = normal;
    state.supportIdentity = hit.supportIdentity;

    const float flatAngle = parameters.flatAngleDegrees * DegreesToRadians;
    if (slopeAngle <= flatAngle)
    {
        state.active = false;
        state.weight = 0;
        state.reason = "flat";
        return locals;
    }
    if (parameters.weight <= 0)
    {
        state.active = false;
        state.weight = 0;
        state.reason = "weight-zero";
        return locals;
    }
    const Eigen::Vector3f axis = up.cross(normal);
    if (axis.norm() < 0.0001f)
    {
        state.active = false;
        state.reason = "degenerate-normal";
        return locals;
    }

    const float maxSlope = parameters.maxSlopeDegrees * DegreesToRadians;
    const float targetAngle = std::min(slopeAngle, maxSlope);
    state.targetSlopeAngleDegrees = targetAngle * RadiansToDegrees;
    const float response = parameters.responseSpeed > 0
        ? 1.0f - std::exp(-parameters.responseSpeed * deltaSeconds)
        : 1.0f;
    state.weight += (parameters.weight - state.weight) * response;
    if (state.weight < 0.0001f) state.weight = 0;
    if (state.weight > 0.9999f) state.weight = 1;
    const float appliedAngle = targetAngle * state.weight;
    state.appliedSlopeAngleDegrees = appliedAngle * RadiansToDegrees;
    if (appliedAngle <= 0.000001f)
    {
        state.active = false;
        state.reason = "no-op";
        return locals;
    }

    const Eigen::Matrix3f worldRotation =
        Eigen::AngleAxisf(appliedAngle, axis.normalized()).toRotationMatrix();
    const Eigen::Matrix3f localRotation =
        componentRotation.transpose() * worldRotation * componentRotation;
    const Eigen::Quaternionf localQuaternion(localRotation);

    LocalPose solved = locals;
    const Eigen::Vector3f rootTranslation = Translation(solved, root);
    const Eigen::Quaternionf rootRotation = Rotation(solved, root);
    SetRotation(solved, root, (localQuaternion * rootRotation).normalized());

    // 旋转绕 root 自身位置发生；foot 补偿只写 root 的局部平移，所以 Actor/胶囊
    // 与 rootMotionDelta 完全不变。
    const Eigen::Vector3f rootModelOrigin = Position(models[root]);
    float lowestDistance = std::numeric_limits<float>::max();
    for (size_t foot : feet)
    {
        const Eigen::Vector3f local = rootModelOrigin +
            localRotation * (Position(models[foot]) - rootModelOrigin);
        const Eigen::Vector3f world = TransformPoint(component, local);
        lowestDistance = std::min(lowestDistance, normal.dot(world - hit.point));
    }

    Eigen::Vector3f worldOffset = Eigen::Vector3f::Zero();
    if (std::isfinite(lowestDistance))
        worldOffset -= normal * (lowestDistance * parameters.footCompensation * state.weight);
    worldOffset += Eigen::Vector3f::UnitY() * (parameters.pelvisHeightOffset * state.weight);
    if (worldOffset.norm() > parameters.maxPelvisOffset)
        worldOffset = worldOffset.normalized() * parameters.maxPelvisOffset;
    if (!worldOffset.allFinite()) worldOffset.setZero();
    const Eigen::Vector3f localOffset = componentRotation.transpose() * worldOffset / componentScale;
    SetTranslation(solved, root, rootTranslation + localOffset);

    state.pelvisOffset = worldOffset.y();
    state.active = true;
    state.reason = "active";
    changed = true;
    return solved;
}
}
