#include "editorRetargetPreview.h"

#include "animation/animationRetarget.h"
#include "animation/animationSkeleton.h"
#include <ozz/animation/offline/raw_skeleton.h>
#include <ozz/animation/offline/skeleton_builder.h>
#include <algorithm>
#include <stdexcept>

namespace VL::Editor
{
namespace
{
using namespace Animation;
using namespace EditorUi;

void BuildRawJoint(const SkeletonAsset& skeleton, size_t index,
    const std::vector<std::vector<size_t>>& children,
    ozz::animation::offline::RawSkeleton::Joint& output)
{
    const auto& joint = skeleton.joints[index];
    output.name = joint.name.c_str();
    const Eigen::Vector3f translation = joint.restLocal.block<3, 1>(0, 3);
    Eigen::Matrix3f rotation = joint.restLocal.block<3, 3>(0, 0);
    const Eigen::Vector3f scale(rotation.col(0).norm(), rotation.col(1).norm(), rotation.col(2).norm());
    // ParseSkeleton 已拒绝 shear/镜像/奇异；仅分解已验证 TRS，不采用另一套容差修复输入。
    for (int axis = 0; axis < 3; ++axis) rotation.col(axis) /= scale[axis];
    const Eigen::Quaternionf quaternion(rotation);
    output.transform.translation = {translation.x(), translation.y(), translation.z()};
    output.transform.rotation = {quaternion.x(), quaternion.y(), quaternion.z(), quaternion.w()};
    output.transform.scale = {scale.x(), scale.y(), scale.z()};
    output.children.resize(children[index].size());
    for (size_t child = 0; child < children[index].size(); ++child)
        BuildRawJoint(skeleton, children[index][child], children, output.children[child]);
}

SkeletonMesh PreviewSkeleton(const SkeletonAsset& skeleton)
{
    std::vector<std::vector<size_t>> children(skeleton.joints.size());
    for (size_t index = 1; index < skeleton.joints.size(); ++index)
        children[static_cast<size_t>(skeleton.joints[index].parent)].push_back(index);
    ozz::animation::offline::RawSkeleton raw;
    raw.roots.resize(1);
    BuildRawJoint(skeleton, 0, children, raw.roots[0]);
    SkeletonMesh mesh;
    mesh.runtime = ozz::animation::offline::SkeletonBuilder()(raw);
    if (!mesh.runtime) throw std::runtime_error("Cannot build reference skeleton");
    // ozz 使用深度优先顺序；SK 只要求父先于子，展示索引必须按实际 runtime 顺序重排。
    for (int index = 0; index < mesh.runtime->num_joints(); ++index)
    {
        const auto& authored = skeleton.joints[static_cast<size_t>(
            skeleton.JointIndex(mesh.runtime->joint_names()[index]))];
        const int parent = mesh.runtime->joint_parents()[index];
        const Eigen::Matrix4f model = parent < 0 ? authored.restLocal :
            (mesh.joints[static_cast<size_t>(parent)].restModel * authored.restLocal).eval();
        mesh.joints.push_back({authored.name, parent, authored.restLocal, model});
    }
    return mesh;
}

int Index(const SkeletonMesh& mesh, const std::string& name)
{
    for (size_t index = 0; index < mesh.joints.size(); ++index)
        if (mesh.joints[index].name == name) return static_cast<int>(index);
    throw std::runtime_error("Unknown preview joint: " + name);
}

std::array<float, 3> Vector(const Eigen::Vector3f& value)
{
    return {value.x(), value.y(), value.z()};
}

std::vector<EditorRetargetJoint> Freeze(const SkeletonMesh& mesh, const LocalPose& reference)
{
    const auto rest = mesh.runtime->joint_rest_poses();
    const auto bindModels = LocalToModels(mesh, LocalPose(rest.begin(), rest.end()));
    const auto models = LocalToModels(mesh, reference);
    std::vector<EditorRetargetJoint> result;
    for (size_t index = 0; index < mesh.joints.size(); ++index)
    {
        // 有限局部 TRS 的层级乘积仍可能溢出；在发布 UI 像素坐标前拒绝，不能输出 NaN CSS。
        if (!bindModels[index].allFinite() || !models[index].allFinite())
            throw std::runtime_error("Non-finite reference model transform: " + mesh.joints[index].name);
        EditorRetargetJoint joint;
        joint.name = mesh.joints[index].name;
        joint.parent = mesh.joints[index].parent;
        joint.bindPosition = Vector(bindModels[index].block<3, 1>(0, 3));
        joint.referencePosition = Vector(models[index].block<3, 1>(0, 3));
        for (int axis = 0; axis < 3; ++axis)
        {
            joint.bindAxes[axis] = Vector(bindModels[index].block<3, 1>(0, axis).normalized());
            joint.referenceAxes[axis] = Vector(models[index].block<3, 1>(0, axis).normalized());
        }
        result.push_back(std::move(joint));
    }
    return result;
}

bool Ancestor(const std::vector<EditorRetargetJoint>& joints, int ancestor, int joint)
{
    for (int parent = joints[static_cast<size_t>(joint)].parent; parent >= 0;
        parent = joints[static_cast<size_t>(parent)].parent)
        if (parent == ancestor) return true;
    return false;
}
}

EditorUi::EditorRetargetPreview BuildEditorRetargetPreview(
    const nlohmann::json& json, const nlohmann::json& sourceJson, const nlohmann::json& targetJson)
{
    const auto profile = ParseRetargetProfile(json, "Retarget preview");
    const auto source = ParseSkeleton(sourceJson, "preview.sourceSkeletonPath");
    const auto target = ParseSkeleton(targetJson, "preview.targetSkeletonPath");
    if (source.id != profile.sourceSkeleton || target.id != profile.targetSkeleton)
        throw std::runtime_error("Preview skeleton identity differs from retarget profile");
    const auto sourceMesh = PreviewSkeleton(source);
    const auto targetMesh = PreviewSkeleton(target);
    const CompiledRetarget compiled(sourceMesh, targetMesh, profile);
    EditorRetargetPreview result;
    result.sourceSkeleton = source.id;
    result.targetSkeleton = target.id;
    result.sourcePose = profile.sourcePose.name;
    result.targetPose = profile.targetPose.name;
    result.sourceOffsets = profile.sourcePose.rotationOffsets.size();
    result.targetOffsets = profile.targetPose.rotationOffsets.size();
    result.source = Freeze(sourceMesh, compiled.SourceReference());
    result.target = Freeze(targetMesh, compiled.TargetReference());
    for (const auto& mapping : profile.joints)
        result.mappings.push_back({Index(sourceMesh, mapping.source), Index(targetMesh, mapping.target), -1});
    for (size_t index = 0; index < result.mappings.size(); ++index)
    {
        auto& mapping = result.mappings[index];
        // 沿源父链找最近的共同映射祖先；twist 等未映射中间骨仍可参与几何链路。
        for (int parent = result.source[mapping.source].parent; parent >= 0 && mapping.ancestor < 0;
            parent = result.source[parent].parent)
            for (size_t candidate = 0; candidate < result.mappings.size(); ++candidate)
            {
                const auto& other = result.mappings[candidate];
                if (other.source == parent && Ancestor(result.target, other.target, mapping.target))
                {
                    mapping.ancestor = static_cast<int>(candidate);
                    break;
                }
            }
        // root 以下的首个映射通常为 pelvis；显示默认在这里平移对齐，不缩放骨长。
        if (result.defaultAnchor == 0 && mapping.source > 0 && mapping.target > 0)
            result.defaultAnchor = index;
    }
    return result;
}
}
