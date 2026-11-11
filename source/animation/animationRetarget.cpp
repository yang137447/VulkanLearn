#include "animationRetarget.h"

#include <cmath>
#include <stdexcept>
#include <unordered_set>

namespace VL::Animation
{
namespace
{
struct JointTrs
{
    Eigen::Vector3f translation = Eigen::Vector3f::Zero();
    Eigen::Quaternionf rotation = Eigen::Quaternionf::Identity();
    Eigen::Vector3f scale = Eigen::Vector3f::Ones();
};

float ReadLane(ozz::math::SimdFloat4 value, size_t lane)
{
    float lanes[4];
    ozz::math::StorePtrU(value, lanes);
    return lanes[lane];
}

void WriteLane(ozz::math::SimdFloat4& value, size_t lane, float component)
{
    float lanes[4];
    ozz::math::StorePtrU(value, lanes);
    lanes[lane] = component;
    value = ozz::math::simd_float4::LoadPtrU(lanes);
}

JointTrs ReadJoint(const LocalPose& pose, size_t index)
{
    const auto& soa = pose.at(index / 4);
    const size_t lane = index % 4;
    JointTrs joint;
    joint.translation = {ReadLane(soa.translation.x, lane), ReadLane(soa.translation.y, lane),
        ReadLane(soa.translation.z, lane)};
    joint.rotation = Eigen::Quaternionf(ReadLane(soa.rotation.w, lane), ReadLane(soa.rotation.x, lane),
        ReadLane(soa.rotation.y, lane), ReadLane(soa.rotation.z, lane)).normalized();
    joint.scale = {ReadLane(soa.scale.x, lane), ReadLane(soa.scale.y, lane),
        ReadLane(soa.scale.z, lane)};
    return joint;
}

void WriteJoint(LocalPose& pose, size_t index, const JointTrs& joint)
{
    auto& soa = pose.at(index / 4);
    const size_t lane = index % 4;
    WriteLane(soa.translation.x, lane, joint.translation.x());
    WriteLane(soa.translation.y, lane, joint.translation.y());
    WriteLane(soa.translation.z, lane, joint.translation.z());
    WriteLane(soa.rotation.x, lane, joint.rotation.x());
    WriteLane(soa.rotation.y, lane, joint.rotation.y());
    WriteLane(soa.rotation.z, lane, joint.rotation.z());
    WriteLane(soa.rotation.w, lane, joint.rotation.w());
    WriteLane(soa.scale.x, lane, joint.scale.x());
    WriteLane(soa.scale.y, lane, joint.scale.y());
    WriteLane(soa.scale.z, lane, joint.scale.z());
}

int FindJoint(const SkeletonMesh& mesh, const std::string& name)
{
    for (size_t index = 0; index < mesh.joints.size(); ++index)
        if (mesh.joints[index].name == name) return static_cast<int>(index);
    return -1;
}

std::vector<Eigen::Quaternionf> ModelRotations(
    const LocalPose& pose, const std::vector<int>& parents)
{
    std::vector<Eigen::Quaternionf> result(parents.size());
    for (size_t index = 0; index < parents.size(); ++index)
    {
        const auto local = ReadJoint(pose, index).rotation;
        result[index] = parents[index] < 0 ? local :
            (result[static_cast<size_t>(parents[index])] * local).normalized();
    }
    return result;
}

void ValidateRetargetPose(const RetargetPose& pose, const SkeletonMesh& mesh)
{
    if (pose.name.empty())
        throw std::invalid_argument("Retarget pose requires a nonempty name");
    std::unordered_set<int> joints;
    for (const auto& offset : pose.rotationOffsets)
    {
        const int index = FindJoint(mesh, offset.joint);
        if (index < 0)
            throw std::invalid_argument("Retarget pose references an unknown joint: " + offset.joint);
        // locomotion 根的 TRS 消费由播放器拥有；校准只调整根下的骨骼，不暗改根运动基底。
        if (index == 0)
            throw std::invalid_argument("Retarget pose cannot rotate locomotion root: " + offset.joint);
        if (!joints.insert(index).second)
            throw std::invalid_argument("Retarget pose contains a duplicate joint: " + offset.joint);
        if (!offset.rotation.coeffs().allFinite() ||
            std::abs(offset.rotation.norm() - 1.0f) > 0.0001f)
            throw std::invalid_argument("Retarget pose requires finite unit quaternions: " + offset.joint);
    }
}

RetargetPose ParseRetargetPose(const nlohmann::json& json, const std::string& path)
{
    if (!json.is_object() || !json.contains("name") || !json.at("name").is_string() ||
        json.at("name").get<std::string>().empty() || !json.contains("rotationOffsets") ||
        !json.at("rotationOffsets").is_array())
        throw std::invalid_argument("Retarget pose requires name and rotationOffsets: " + path);
    for (const auto& item : json.items())
        if (item.key() != "name" && item.key() != "rotationOffsets")
            throw std::invalid_argument("Unknown retarget pose field: " + path + "." + item.key());

    RetargetPose pose;
    pose.name = json.at("name").get<std::string>();
    for (const auto& value : json.at("rotationOffsets"))
    {
        if (!value.is_object() || value.size() != 2 || !value.contains("joint") ||
            !value.at("joint").is_string() || value.at("joint").get<std::string>().empty() ||
            !value.contains("rotation") || !value.at("rotation").is_array() ||
            value.at("rotation").size() != 4)
            throw std::invalid_argument("Retarget offset requires joint and rotation [x,y,z,w]: " + path);
        Eigen::Vector4f xyzw;
        for (size_t lane = 0; lane < 4; ++lane)
        {
            const auto& component = value.at("rotation").at(lane);
            if (!component.is_number())
                throw std::invalid_argument("Retarget rotation must contain numbers: " + path);
            xyzw[static_cast<int>(lane)] = component.get<float>();
        }
        if (!xyzw.allFinite() || std::abs(xyzw.norm() - 1.0f) > 0.0001f)
            throw std::invalid_argument("Retarget rotation must be a finite unit quaternion: " + path);
        pose.rotationOffsets.push_back({value.at("joint").get<std::string>(),
            Eigen::Quaternionf(xyzw.w(), xyzw.x(), xyzw.y(), xyzw.z())});
    }
    return pose;
}

LocalPose MakeReferencePose(const LocalPose& rest, const SkeletonMesh& mesh,
    const RetargetPose& calibration)
{
    LocalPose result = rest;
    for (const auto& offset : calibration.rotationOffsets)
    {
        const size_t index = static_cast<size_t>(FindJoint(mesh, offset.joint));
        JointTrs value = ReadJoint(rest, index);
        // 右乘表示原始 bind 骨轴中的局部偏移；父级影响随后通过模型空间累乘统一传播。
        value.rotation = (value.rotation * offset.rotation).normalized();
        WriteJoint(result, index, value);
    }
    return result;
}

} // namespace

void RetargetProfile::Validate(const SkeletonMesh& source, const SkeletonMesh& target) const
{
    if (source.joints.empty() || target.joints.empty() || !source.runtime || !target.runtime ||
        source.runtime->num_joints() != static_cast<int>(source.joints.size()) ||
        target.runtime->num_joints() != static_cast<int>(target.joints.size()))
        throw std::invalid_argument("Retarget profile requires nonempty source and target skeletons");
    if (sourceRoot.empty() || targetRoot.empty())
        throw std::invalid_argument("Retarget profile requires sourceRoot and targetRoot");

    if (FindJoint(source, sourceRoot) < 0 || FindJoint(target, targetRoot) < 0)
        throw std::invalid_argument("Retarget profile root is missing from its skeleton");
    const int sourceRootIndex = FindJoint(source, sourceRoot);
    const int targetRootIndex = FindJoint(target, targetRoot);
    // 播放器的根轨道、in-place 与根运动均固定使用索引 0；不接受第二棵树。
    if (sourceRootIndex != 0 || targetRootIndex != 0)
        throw std::invalid_argument("Retarget profile roots must be joint zero");
    for (const SkeletonMesh* mesh : {&source, &target})
        for (size_t index = 0; index < mesh->joints.size(); ++index)
        {
            const int parent = mesh->joints[index].parent;
            if ((index == 0 && parent != -1) ||
                (index > 0 && (parent < 0 || parent >= static_cast<int>(index))))
                throw std::invalid_argument("Retarget requires a single topologically ordered skeleton");
        }

    std::unordered_set<int> sources;
    std::unordered_set<int> targets;
    for (const RetargetJoint& joint : joints)
    {
        const int sourceIndex = FindJoint(source, joint.source);
        const int targetIndex = FindJoint(target, joint.target);
        if (sourceIndex < 0 || targetIndex < 0)
            throw std::invalid_argument("Retarget profile references an unknown joint: " +
                joint.source + " -> " + joint.target);
        if (!sources.insert(sourceIndex).second || !targets.insert(targetIndex).second)
            throw std::invalid_argument("Retarget profile contains a duplicate joint mapping");
    }
    if (joints.empty())
        throw std::invalid_argument("Retarget profile requires at least one joint mapping");
    bool rootMapped = false;
    for (const RetargetJoint& joint : joints)
    {
        rootMapped = rootMapped ||
            (joint.source == sourceRoot && joint.target == targetRoot);
    }
    if (!rootMapped)
        throw std::invalid_argument("Retarget profile must map sourceRoot to targetRoot");
    ValidateRetargetPose(sourcePose, source);
    ValidateRetargetPose(targetPose, target);
}

RetargetProfile ParseRetargetProfile(const nlohmann::json& json, std::string_view basePath)
{
    if (!json.is_object())
        throw std::invalid_argument("Retarget profile must be an object: " + std::string(basePath));
    if (json.value("format", "") != "vl.retarget.v1")
        throw std::invalid_argument("Retarget profile format must be vl.retarget.v1: " +
            std::string(basePath));

    RetargetProfile profile;
    auto required = [&](const char* key) {
        if (!json.contains(key) || !json.at(key).is_string() || json.at(key).get<std::string>().empty())
            throw std::invalid_argument("Retarget profile requires nonempty " + std::string(key) +
                ": " + std::string(basePath));
        return json.at(key).get<std::string>();
    };
    profile.sourceSkeleton = required("sourceSkeleton");
    profile.targetSkeleton = required("targetSkeleton");
    profile.sourceRoot = required("sourceRoot");
    profile.targetRoot = required("targetRoot");
    if (!json.contains("joints") || !json.at("joints").is_array() || json.at("joints").empty())
        throw std::invalid_argument("Retarget profile requires a nonempty joints array: " +
            std::string(basePath));
    for (const auto& value : json.at("joints"))
    {
        if (!value.is_object() || !value.contains("source") || !value.contains("target") ||
            !value.at("source").is_string() || !value.at("target").is_string() ||
            value.at("source").get<std::string>().empty() || value.at("target").get<std::string>().empty())
            throw std::invalid_argument("Retarget profile joints require source and target names: " +
                std::string(basePath));
        profile.joints.push_back({value.at("source").get<std::string>(),
            value.at("target").get<std::string>()});
    }
    if (json.contains("sourcePose"))
        profile.sourcePose = ParseRetargetPose(json.at("sourcePose"), std::string(basePath) + ".sourcePose");
    if (json.contains("targetPose"))
        profile.targetPose = ParseRetargetPose(json.at("targetPose"), std::string(basePath) + ".targetPose");
    return profile;
}

CompiledRetarget::CompiledRetarget(const SkeletonMesh& source, const SkeletonMesh& target,
    const RetargetProfile& profile)
{
    profile.Validate(source, target);
    const auto sourceRestSpan = source.runtime->joint_rest_poses();
    sourceRest.assign(sourceRestSpan.begin(), sourceRestSpan.end());
    const auto targetRestSpan = target.runtime->joint_rest_poses();
    const LocalPose targetRest(targetRestSpan.begin(), targetRestSpan.end());
    targetReference = MakeReferencePose(targetRest, target, profile.targetPose);
    for (const auto& joint : source.joints) sourceParents.push_back(joint.parent);
    for (const auto& joint : target.joints) targetParents.push_back(joint.parent);
    sourceReference = MakeReferencePose(sourceRest, source, profile.sourcePose);
    sourceReferenceRotations = ModelRotations(sourceReference, sourceParents);
    targetReferenceRotations = ModelRotations(targetReference, targetParents);
    targetToSource.assign(target.joints.size(), -1);
    for (const RetargetJoint& mapping : profile.joints)
        targetToSource[static_cast<size_t>(FindJoint(target, mapping.target))] =
            FindJoint(source, mapping.source);
}

LocalPose CompiledRetarget::Apply(const LocalPose& sourcePose) const
{
    if (sourcePose.size() != sourceRest.size())
        throw std::invalid_argument("Retarget source pose does not match source skeleton");
    LocalPose result = targetReference;
    const auto sourceRotations = ModelRotations(sourcePose, sourceParents);
    std::vector<Eigen::Quaternionf> targetRotations(targetParents.size());
    for (size_t targetIndex = 0; targetIndex < targetParents.size(); ++targetIndex)
    {
        const int sourceIndex = targetToSource[targetIndex];
        const Eigen::Quaternionf parentTargetRotation =
            targetParents[targetIndex] < 0
            ? Eigen::Quaternionf::Identity()
            : targetRotations[static_cast<size_t>(targetParents[targetIndex])];
        JointTrs targetValue = ReadJoint(targetReference, targetIndex);
        if (sourceIndex >= 0)
        {
            const size_t sourceJoint = static_cast<size_t>(sourceIndex);
            // 左乘校准参考的模型空间增量；source 恰为校准姿态时，输出 target 校准姿态。
            // bind/inverse bind 不参与校准改写；未映射骨保留校准后的局部旋转并继承父骨。
            const Eigen::Quaternionf desired = (sourceRotations[sourceJoint] *
                sourceReferenceRotations[sourceJoint].conjugate() *
                targetReferenceRotations[targetIndex]).normalized();
            targetValue.rotation = (parentTargetRotation.conjugate() * desired).normalized();
            // 根只传模型空间位移差，单位同为厘米；不按身高放大根运动。
            // 其它平移/缩放保留目标绑定值，避免复制源角色的骨长。
            if (targetIndex == 0)
                targetValue.translation += ReadJoint(sourcePose, sourceJoint).translation -
                    ReadJoint(sourceRest, sourceJoint).translation;
        }
        targetRotations[targetIndex] =
            (parentTargetRotation * targetValue.rotation).normalized();
        WriteJoint(result, targetIndex, targetValue);
    }
    return result;
}

LocalPose RetargetLocalPose(const SkeletonMesh& source, const SkeletonMesh& target,
    const RetargetProfile& profile, const LocalPose& sourcePose)
{
    return CompiledRetarget(source, target, profile).Apply(sourcePose);
}
}
