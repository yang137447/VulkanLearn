#include "motionBricksSkeleton.h"

#include <ozz/animation/offline/raw_skeleton.h>
#include <ozz/animation/offline/skeleton_builder.h>

#include <cmath>
#include <algorithm>
#include <fstream>
#include <functional>
#include <stdexcept>
#include <string>
#include <nlohmann/json.hpp>

namespace VL::Animation::MotionBricks
{
float PoseHeading(const SkeletonBinding& binding, const Pose& pose)
{
    const Pose world = ComputeWorldPose(binding, pose);
    const Eigen::Vector3f across = world[binding.rightLegJoint].position -
        world[binding.leftLegJoint].position;
    return std::atan2(across.z(), -across.x());
}

std::vector<LocomotionTarget> LoadLocomotionTargets(
    const std::filesystem::path& path, const SkeletonBinding& binding)
{
    std::ifstream input(path);
    if (!input) throw std::invalid_argument("Could not open locomotion targets: " + path.string());
    nlohmann::json json;
    input >> json;
    if (json.at("format") != "vl.motionbricks.locomotion.v1")
        throw std::invalid_argument("Invalid MotionBricks locomotion target format");
    const auto names = json.at("jointNames").get<std::vector<std::string>>();
    if (names.size() != binding.jointNames.size())
        throw std::invalid_argument("Locomotion target skeleton size mismatch");
    std::vector<size_t> indices;
    for (const std::string& name : binding.jointNames)
    {
        const auto found = std::find(names.begin(), names.end(), name);
        if (found == names.end() || std::count(names.begin(), names.end(), name) != 1)
            throw std::invalid_argument("Locomotion target joint missing or duplicated: " + name);
        indices.push_back(static_cast<size_t>(found - names.begin()));
    }
    std::vector<LocomotionTarget> targets;
    for (const auto& entry : json.at("targets"))
    {
        LocomotionTarget target;
        target.speed = entry.at("speed").get<float>();
        if (!std::isfinite(target.speed) || target.speed < 0 ||
            (!targets.empty() && target.speed <= targets.back().speed))
            throw std::invalid_argument("Locomotion speeds must be finite and increasing");
        const auto& poses = entry.at("poses");
        if (poses.size() != kContextPoses)
            throw std::invalid_argument("Locomotion targets require four poses");
        for (size_t frame = 0; frame < kContextPoses; ++frame)
        {
            if (poses[frame].size() != names.size())
                throw std::invalid_argument("Locomotion target pose size mismatch");
            for (size_t index : indices)
            {
                const auto values = poses[frame][index].get<std::vector<float>>();
                if (values.size() != 7)
                    throw std::invalid_argument("Locomotion joint requires seven values");
                Transform transform;
                transform.position = Eigen::Vector3f(values[0], values[1], values[2]);
                transform.rotation = Eigen::Quaternionf(values[6], values[3], values[4], values[5]);
                target.poses[frame].push_back(Normalized(transform));
            }
        }
        targets.push_back(std::move(target));
    }
    if (targets.size() < 2 || targets.front().speed != 0)
        throw std::invalid_argument("Locomotion targets require idle and moving poses");
    return targets;
}

LocalPose RetargetInPlace(const SomaSkeletonAsset& source,
    const SkeletonMesh& target, const CompiledRetarget& retarget, Pose pose)
{
    const float yaw = PoseHeading(source.binding, pose) -
        PoseHeading(source.binding, source.binding.referenceLocalPose);
    pose[0].rotation = Eigen::Quaternionf(Eigen::AngleAxisf(-yaw,
        Eigen::Vector3f::UnitY())) * pose[0].rotation;
    pose[0].position.x() = source.binding.referenceLocalPose[0].position.x();
    pose[0].position.z() = source.binding.referenceLocalPose[0].position.z();
    LocalPose locals = retarget.Apply(PoseToLocalPoseCm(pose));
    Pose transforms = LocalPoseToPoseMeters(locals);
    const Pose reference = LocalPoseToPoseMeters(retarget.TargetReference());
    const auto pelvis = std::find_if(target.joints.begin(), target.joints.end(),
        [](const auto& joint) { return joint.name == "pelvis"; });
    if (pelvis != target.joints.end() && pelvis->parent == 0)
    {
        const size_t index = static_cast<size_t>(pelvis - target.joints.begin());
        transforms[index] = Compose(Compose(Inverse(reference[0]), transforms[0]),
            transforms[index]);
    }
    transforms[0] = reference[0];
    LocalPose result = PoseToLocalPoseCm(transforms);
    for (size_t index = 0; index < result.size(); ++index)
        result[index].scale = locals[index].scale;
    return result;
}

namespace
{
void WriteLane(ozz::math::SimdFloat4& vector, size_t lane, float value)
{
    float values[4];
    ozz::math::StorePtrU(vector, values);
    values[lane] = value;
    vector = ozz::math::simd_float4::LoadPtrU(values);
}

Eigen::Quaternionf RestRotation(const Eigen::Matrix4f& restLocal, const std::string& name)
{
    const Eigen::Matrix3f linear = restLocal.block<3, 3>(0, 0);
    const Eigen::Vector3f scale(linear.col(0).norm(), linear.col(1).norm(),
        linear.col(2).norm());
    if (!linear.allFinite() || scale.minCoeff() <= 1e-8f)
        throw std::invalid_argument("Skeleton joint '" + name +
            "' has a degenerate rest local transform");
    Eigen::Matrix3f rotation;
    for (int column = 0; column < 3; ++column)
        rotation.col(column) = linear.col(column) / scale[column];
    Eigen::Quaternionf value(rotation);
    value.normalize();
    return value;
}

void AppendRawJoint(const SkeletonAsset& skeleton, const std::vector<std::vector<int>>& children,
    int index, ozz::animation::offline::RawSkeleton::Joint& output)
{
    const SkeletonJoint& source = skeleton.joints[static_cast<size_t>(index)];
    output.name = source.name.c_str();
    const Eigen::Matrix4f& rest = source.restLocal;
    const Eigen::Vector3f translation = rest.block<3, 1>(0, 3);
    const Eigen::Vector3f scale(
        rest.block<3, 3>(0, 0).col(0).norm(),
        rest.block<3, 3>(0, 0).col(1).norm(),
        rest.block<3, 3>(0, 0).col(2).norm());
    const Eigen::Quaternionf rotation = RestRotation(rest, source.name);
    if (!translation.allFinite() || !scale.allFinite())
        throw std::invalid_argument("Skeleton joint '" + source.name +
            "' has a non-finite rest transform");
    output.transform.translation = {translation.x(), translation.y(), translation.z()};
    output.transform.rotation = {rotation.x(), rotation.y(), rotation.z(), rotation.w()};
    output.transform.scale = {scale.x(), scale.y(), scale.z()};
    output.children.resize(children[static_cast<size_t>(index)].size());
    for (size_t child = 0; child < output.children.size(); ++child)
        AppendRawJoint(skeleton, children,
            children[static_cast<size_t>(index)][child], output.children[child]);
}

void ValidateSkeletonRoot(const SkeletonAsset& skeleton)
{
    if (skeleton.joints.empty() || skeleton.joints[0].parent != -1)
        throw std::invalid_argument("Skeleton must have its root at joint zero");
    if (skeleton.joints[0].name != skeleton.root)
        throw std::invalid_argument("Skeleton root name does not match joints[0]");
    for (size_t index = 1; index < skeleton.joints.size(); ++index)
        if (skeleton.joints[index].parent < 0)
            throw std::invalid_argument("Skeleton must have exactly one root joint");
}
}

SkeletonMesh BuildSkeletonMesh(const SkeletonAsset& skeleton)
{
    ValidateSkeletonRoot(skeleton);
    const size_t jointCount = skeleton.joints.size();
    std::vector<std::vector<int>> children(jointCount);
    for (size_t index = 1; index < jointCount; ++index)
    {
        const int parent = skeleton.joints[index].parent;
        if (parent < 0 || parent >= static_cast<int>(index))
            throw std::invalid_argument("Skeleton parents must precede their children");
        children[static_cast<size_t>(parent)].push_back(static_cast<int>(index));
    }

    ozz::animation::offline::RawSkeleton raw;
    raw.roots.resize(1);
    AppendRawJoint(skeleton, children, 0, raw.roots[0]);

    ozz::animation::offline::SkeletonBuilder builder;
    auto runtime = builder(raw);
    if (!runtime || runtime->num_joints() != static_cast<int>(jointCount))
        throw std::runtime_error("Could not build the ozz skeleton runtime");

    std::vector<int> order;
    order.reserve(jointCount);
    std::function<void(int)> visit = [&](int index)
    {
        order.push_back(index);
        for (int child : children[static_cast<size_t>(index)])
            visit(child);
    };
    visit(0);
    if (order.size() != jointCount)
        throw std::invalid_argument("Skeleton hierarchy has unreachable joints");

    std::vector<int> newIndex(jointCount, -1);
    for (size_t slot = 0; slot < order.size(); ++slot)
        newIndex[static_cast<size_t>(order[slot])] = static_cast<int>(slot);

    SkeletonMesh mesh;
    mesh.joints.resize(jointCount);
    for (size_t slot = 0; slot < order.size(); ++slot)
    {
        const SkeletonJoint& source = skeleton.joints[static_cast<size_t>(order[slot])];
        Joint& target = mesh.joints[slot];
        target.name = source.name;
        target.parent = source.parent < 0 ? -1 : newIndex[static_cast<size_t>(source.parent)];
        target.restLocal = source.restLocal;
        target.restModel = target.parent < 0 ? target.restLocal :
            mesh.joints[static_cast<size_t>(target.parent)].restModel * target.restLocal;
        if (source.parent >= 0 &&
            runtime->joint_parents()[slot] != target.parent)
            throw std::runtime_error("ozz skeleton order does not match the SK hierarchy");
    }
    mesh.runtime = std::move(runtime);
    return mesh;
}

std::shared_ptr<const SomaSkeletonAsset> LoadSomaSkeleton(
    const std::filesystem::path& skeletonPath,
    const std::filesystem::path& manifestPath)
{
    std::ifstream input(skeletonPath, std::ios::binary);
    if (!input)
        throw std::invalid_argument("Could not open SOMA skeleton: " + skeletonPath.string());
    nlohmann::json json;
    input >> json;

    auto asset = std::make_shared<SomaSkeletonAsset>();
    asset->contract = ParseModelManifest(manifestPath);
    ValidateSomaContract(asset->contract);
    asset->skeleton = ParseSkeleton(json, skeletonPath.string());
    asset->mesh = BuildSkeletonMesh(asset->skeleton);

    std::vector<std::string> names;
    std::vector<int> parents;
    Pose reference;
    names.reserve(asset->mesh.joints.size());
    parents.reserve(asset->mesh.joints.size());
    reference.reserve(asset->mesh.joints.size());
    for (const Joint& joint : asset->mesh.joints)
    {
        names.push_back(joint.name);
        parents.push_back(joint.parent);
        Transform transform;
        transform.position = joint.restLocal.block<3, 1>(0, 3) / 100.0f;
        transform.rotation = RestRotation(joint.restLocal, joint.name);
        reference.push_back(transform);
    }
    asset->binding = BindSomaSkeleton(asset->contract, std::move(names),
        std::move(parents), std::move(reference));
    return asset;
}

LocalPose PoseToLocalPoseCm(const Pose& pose)
{
    if (pose.empty())
        throw std::invalid_argument("MotionBricks pose cannot be empty");
    LocalPose locals((pose.size() + 3) / 4, ozz::math::SoaTransform::identity());
    for (size_t joint = 0; joint < pose.size(); ++joint)
    {
        const Transform value = Normalized(pose[joint]);
        auto& target = locals[joint / 4];
        const size_t lane = joint % 4;
        WriteLane(target.translation.x, lane, value.position.x() * 100.0f);
        WriteLane(target.translation.y, lane, value.position.y() * 100.0f);
        WriteLane(target.translation.z, lane, value.position.z() * 100.0f);
        WriteLane(target.rotation.x, lane, value.rotation.x());
        WriteLane(target.rotation.y, lane, value.rotation.y());
        WriteLane(target.rotation.z, lane, value.rotation.z());
        WriteLane(target.rotation.w, lane, value.rotation.w());
    }
    return locals;
}

Pose LocalPoseToPoseMeters(const LocalPose& pose)
{
    Pose result(pose.size() * 4);
    for (size_t joint = 0; joint < result.size(); ++joint)
    {
        const auto& source = pose[joint / 4];
        const size_t lane = joint % 4;
        float values[4];
        Transform transform;
        ozz::math::StorePtrU(source.translation.x, values);
        transform.position.x() = values[lane] / 100.0f;
        ozz::math::StorePtrU(source.translation.y, values);
        transform.position.y() = values[lane] / 100.0f;
        ozz::math::StorePtrU(source.translation.z, values);
        transform.position.z() = values[lane] / 100.0f;
        ozz::math::StorePtrU(source.rotation.x, values);
        const float x = values[lane];
        ozz::math::StorePtrU(source.rotation.y, values);
        const float y = values[lane];
        ozz::math::StorePtrU(source.rotation.z, values);
        const float z = values[lane];
        ozz::math::StorePtrU(source.rotation.w, values);
        const float w = values[lane];
        transform.rotation = Eigen::Quaternionf(w, x, y, z).normalized();
        result[joint] = transform;
    }
    return result;
}
}
