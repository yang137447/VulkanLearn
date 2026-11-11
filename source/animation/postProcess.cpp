#include "postProcess.h"

#include "shader/build/contentHash.h"

#include <algorithm>
#include <cmath>
#include <fstream>
#include <set>
#include <stdexcept>

namespace VL::Animation
{
namespace
{
constexpr float KindaSmallNumber = 1.0e-4f;
constexpr float Pi = 3.14159265358979323846f;

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

Eigen::Quaternionf ReadRotation(const LocalPose& locals, size_t joint)
{
    const auto& value = locals[joint / 4].rotation;
    const size_t lane = joint % 4;
    return Eigen::Quaternionf(ReadLane(value.w, lane), ReadLane(value.x, lane),
        ReadLane(value.y, lane), ReadLane(value.z, lane)).normalized();
}

Eigen::Vector3f ReadTranslation(const LocalPose& locals, size_t joint)
{
    const auto& value = locals[joint / 4].translation;
    const size_t lane = joint % 4;
    return {ReadLane(value.x, lane), ReadLane(value.y, lane), ReadLane(value.z, lane)};
}

void WriteTranslation(LocalPose& locals, size_t joint, const Eigen::Vector3f& translation)
{
    auto& value = locals[joint / 4].translation;
    const size_t lane = joint % 4;
    WriteLane(value.x, lane, translation.x());
    WriteLane(value.y, lane, translation.y());
    WriteLane(value.z, lane, translation.z());
}

void WriteRotation(LocalPose& locals, size_t joint, const Eigen::Quaternionf& rotation)
{
    auto& value = locals[joint / 4].rotation;
    const size_t lane = joint % 4;
    WriteLane(value.x, lane, rotation.x());
    WriteLane(value.y, lane, rotation.y());
    WriteLane(value.z, lane, rotation.z());
    WriteLane(value.w, lane, rotation.w());
}

Eigen::Matrix4f LocalMatrix(const LocalPose& locals, size_t joint)
{
    Eigen::Matrix4f matrix = Eigen::Matrix4f::Identity();
    matrix.block<3, 3>(0, 0) = ReadRotation(locals, joint).toRotationMatrix();
    matrix.block<3, 1>(0, 3) = ReadTranslation(locals, joint);
    return matrix;
}

void GlobalMatrices(const SkeletonMesh& mesh, const LocalPose& locals,
    std::vector<Eigen::Matrix4f>& out)
{
    out.assign(mesh.joints.size(), Eigen::Matrix4f::Identity());
    for (size_t joint = 0; joint < out.size(); ++joint)
    {
        const Eigen::Matrix4f local = LocalMatrix(locals, joint);
        const int parent = mesh.joints[joint].parent;
        out[joint] = parent >= 0 && static_cast<size_t>(parent) < joint ?
            out[static_cast<size_t>(parent)] * local : local;
    }
}

Eigen::Quaternionf NormalizedRotation(const Eigen::Matrix4f& matrix)
{
    Eigen::Matrix3f rotation = matrix.block<3, 3>(0, 0);
    for (int column = 0; column < 3; ++column)
    {
        const Eigen::Vector3f axis = rotation.col(column);
        const float length = axis.norm();
        rotation.col(column) = length > KindaSmallNumber ?
            Eigen::Vector3f(axis / length) : Eigen::Vector3f::Unit(column);
    }
    return Eigen::Quaternionf(rotation).normalized();
}

// 只用旋转的 FK：组件空间旋转 = 父组件旋转 * 本骨局部旋转。
void GlobalRotations(const SkeletonMesh& mesh, const LocalPose& locals,
    std::vector<Eigen::Quaternionf>& out)
{
    out.assign(mesh.joints.size(), Eigen::Quaternionf::Identity());
    for (size_t joint = 0; joint < out.size(); ++joint)
    {
        const Eigen::Quaternionf local = ReadRotation(locals, joint);
        const int parent = mesh.joints[joint].parent;
        out[joint] = parent >= 0 && static_cast<size_t>(parent) < joint ?
            (out[static_cast<size_t>(parent)] * local).normalized() : local;
    }
}

const nlohmann::json& Required(const nlohmann::json& object, const char* key, const char* owner)
{
    if (!object.is_object() || !object.contains(key))
        throw std::invalid_argument(std::string("PostProcess missing field ") + owner + "." + key);
    return object.at(key);
}

void RejectUnknown(const nlohmann::json& object, std::initializer_list<const char*> allowed,
    const char* owner)
{
    if (!object.is_object())
        throw std::invalid_argument(std::string("PostProcess object expected at ") + owner);
    for (const auto& item : object.items())
    {
        bool known = false;
        for (const char* key : allowed)
            if (item.key() == key) { known = true; break; }
        if (!known)
            throw std::invalid_argument(std::string("PostProcess unknown field ") + owner + "." +
                item.key());
    }
}

std::string ReadString(const nlohmann::json& value, const char* field)
{
    if (!value.is_string())
        throw std::invalid_argument(std::string("PostProcess expects string: ") + field);
    return value.get<std::string>();
}

float ReadNumber(const nlohmann::json& value, const char* field)
{
    if (!value.is_number())
        throw std::invalid_argument(std::string("PostProcess expects number: ") + field);
    const float result = value.get<float>();
    if (!std::isfinite(result))
        throw std::invalid_argument(std::string("PostProcess number is not finite: ") + field);
    return result;
}

bool ReadBool(const nlohmann::json& value, const char* field)
{
    if (!value.is_boolean())
        throw std::invalid_argument(std::string("PostProcess expects bool: ") + field);
    return value.get<bool>();
}

std::vector<TwistTarget> ParseTargets(const nlohmann::json& targets, const char* owner)
{
    if (!targets.is_array() || targets.empty() || targets.size() > 8)
        throw std::invalid_argument(std::string("PostProcess ") + owner +
            " targets must contain 1..8 entries");
    std::vector<TwistTarget> result;
    for (const auto& target : targets)
    {
        RejectUnknown(target, {"bone", "weight"}, owner);
        TwistTarget value;
        value.bone = ReadString(Required(target, "bone", owner), "targets.bone");
        if (value.bone.empty())
            throw std::invalid_argument("PostProcess target bone is empty");
        value.weight = ReadNumber(Required(target, "weight", owner), "targets.weight");
        if (value.weight < 0.0f || value.weight > 1.0f)
            throw std::invalid_argument("PostProcess target weight must be within [0,1]");
        result.push_back(std::move(value));
    }
    return result;
}

HelperProjection ParseProjection(const nlohmann::json& node)
{
    RejectUnknown(node, {"child", "oldParent", "newParent", "childInitial", "oldParentInitial",
        "newParentInitial"}, "projection");
    HelperProjection projection;
    projection.child = ReadString(Required(node, "child", "projection"), "projection.child");
    projection.oldParent = ReadString(
        Required(node, "oldParent", "projection"), "projection.oldParent");
    projection.newParent = ReadString(
        Required(node, "newParent", "projection"), "projection.newParent");
    if (projection.child.empty() || projection.oldParent.empty() || projection.newParent.empty())
        throw std::invalid_argument("PostProcess projection bones must be non-empty");
    projection.childInitial = ReadBool(
        Required(node, "childInitial", "projection"), "projection.childInitial");
    projection.oldParentInitial = ReadBool(
        Required(node, "oldParentInitial", "projection"), "projection.oldParentInitial");
    projection.newParentInitial = ReadBool(
        Required(node, "newParentInitial", "projection"), "projection.newParentInitial");
    return projection;
}
}

struct PostProcessSolver::CompiledTwist
{
    const TwistEntry* definition = nullptr;
    int driver = -1;
    Eigen::Quaternionf bindRotation = Eigen::Quaternionf::Identity();
    std::vector<std::pair<int, float>> targets;
};

struct PostProcessSolver::CompiledHelper
{
    const HelperEntry* definition = nullptr;
    struct Projection
    {
        int child = -1;
        int oldParent = -1;
        int newParent = -1;
        bool childInitial = true;
        bool oldParentInitial = true;
        bool newParentInitial = false;
        Eigen::Quaternionf childBind = Eigen::Quaternionf::Identity();
        Eigen::Quaternionf oldParentBind = Eigen::Quaternionf::Identity();
        Eigen::Quaternionf newParentBind = Eigen::Quaternionf::Identity();
    };
    std::vector<Projection> projections;
    float blendT = 0.0f;
    std::vector<std::pair<int, float>> targets;
};

struct PostProcessSolver::CompiledConstraint
{
    const ConstraintEntry* definition = nullptr;
    int bone = -1;
    struct Target
    {
        int joint = -1;
        float weight = 1.0f;
        // UE OffsetOption=RefPose：Offset = inverse(Target_ref) * Source_ref。
        Eigen::Matrix4f offset = Eigen::Matrix4f::Identity();
    };
    std::vector<Target> targets;
};

PostProcessAsset ParsePostProcessAsset(const nlohmann::json& json, std::string_view sourcePath)
{
    RejectUnknown(json, {"format", "id", "source", "twist", "helpers", "constraints"}, "root");
    PostProcessAsset asset;
    asset.format = ReadString(Required(json, "format", "root"), "format");
    if (asset.format != "vl.anim-post-process.v1")
        throw std::invalid_argument("PostProcess unsupported format: " + asset.format);
    asset.id = ReadString(Required(json, "id", "root"), "id");
    if (asset.id.empty())
        throw std::invalid_argument("PostProcess id is empty");
    asset.sourcePath = std::string(sourcePath);

    std::set<std::string> targetBones;
    if (json.contains("twist"))
    {
        const auto& twist = json.at("twist");
        if (!twist.is_array() || twist.size() > 64)
            throw std::invalid_argument("PostProcess twist must be an array of at most 64");
        std::set<std::string> names;
        for (const auto& node : twist)
        {
            RejectUnknown(node, {"name", "driverBone", "twistAxis", "invert", "targets"}, "twist");
            TwistEntry entry;
            entry.name = ReadString(Required(node, "name", "twist"), "twist.name");
            if (entry.name.empty() || !names.insert(entry.name).second)
                throw std::invalid_argument(
                    "PostProcess twist names must be non-empty and unique");
            entry.driverBone = ReadString(
                Required(node, "driverBone", "twist"), "twist.driverBone");
            if (entry.driverBone.empty())
                throw std::invalid_argument("PostProcess driverBone is empty");
            const std::string axis = ReadString(
                Required(node, "twistAxis", "twist"), "twist.twistAxis");
            if (!TryParsePoseDriverAxis(axis, entry.twistAxis))
                throw std::invalid_argument("PostProcess unknown twistAxis: " + axis);
            entry.invert = ReadBool(Required(node, "invert", "twist"), "twist.invert");
            entry.targets = ParseTargets(Required(node, "targets", "twist"), "twist");
            for (const auto& target : entry.targets)
                if (!targetBones.insert(target.bone).second)
                    throw std::invalid_argument(
                        "PostProcess target bones must be unique across the asset: " +
                        target.bone);
            asset.twist.push_back(std::move(entry));
        }
    }

    if (json.contains("helpers"))
    {
        const auto& helpers = json.at("helpers");
        if (!helpers.is_array() || helpers.size() > 64)
            throw std::invalid_argument("PostProcess helpers must be an array of at most 64");
        std::set<std::string> names;
        for (const auto& node : helpers)
        {
            RejectUnknown(node, {"name", "projections", "blendT", "targets"}, "helpers");
            HelperEntry entry;
            entry.name = ReadString(Required(node, "name", "helpers"), "helpers.name");
            if (entry.name.empty() || !names.insert(entry.name).second)
                throw std::invalid_argument(
                    "PostProcess helper names must be non-empty and unique");
            const auto& projections = Required(node, "projections", "helpers");
            if (!projections.is_array() || projections.empty() || projections.size() > 2)
                throw std::invalid_argument(
                    "PostProcess helper projections must contain 1..2 entries");
            for (const auto& projection : projections)
                entry.projections.push_back(ParseProjection(projection));
            entry.blendT = projections.size() == 2 ?
                ReadNumber(Required(node, "blendT", "helpers"), "helpers.blendT") : 0.0f;
            if (entry.blendT < 0.0f || entry.blendT > 1.0f)
                throw std::invalid_argument("PostProcess helpers.blendT must be within [0,1]");
            entry.targets = ParseTargets(Required(node, "targets", "helpers"), "helpers");
            for (const auto& target : entry.targets)
                if (!targetBones.insert(target.bone).second)
                    throw std::invalid_argument(
                        "PostProcess target bones must be unique across the asset: " +
                        target.bone);
            asset.helpers.push_back(std::move(entry));
        }
    }
    if (json.contains("constraints"))
    {
        const auto& constraints = json.at("constraints");
        if (!constraints.is_array() || constraints.size() > 64)
            throw std::invalid_argument(
                "PostProcess constraints must be an array of at most 64");
        std::set<std::string> names, bones;
        for (const auto& node : constraints)
        {
            RejectUnknown(node, {"name", "bone", "transformType", "offsetOption", "targets"},
                "constraints");
            ConstraintEntry entry;
            entry.name = ReadString(Required(node, "name", "constraints"), "constraints.name");
            if (entry.name.empty() || !names.insert(entry.name).second)
                throw std::invalid_argument(
                    "PostProcess constraint names must be non-empty and unique");
            entry.bone = ReadString(Required(node, "bone", "constraints"), "constraints.bone");
            if (entry.bone.empty() || !bones.insert(entry.bone).second)
                throw std::invalid_argument(
                    "PostProcess constraint bones must be non-empty and unique");
            // 当前只实现 UE 的 Parent + RefPose 组合；其余组合明确拒绝，避免静默近似。
            const std::string transformType = ReadString(
                Required(node, "transformType", "constraints"), "constraints.transformType");
            if (transformType != "Parent")
                throw std::invalid_argument(
                    "PostProcess only supports constraint transformType=Parent: " +
                    transformType);
            const std::string offsetOption = ReadString(
                Required(node, "offsetOption", "constraints"), "constraints.offsetOption");
            if (offsetOption != "RefPose")
                throw std::invalid_argument(
                    "PostProcess only supports constraint offsetOption=RefPose: " +
                    offsetOption);
            const auto& targets = Required(node, "targets", "constraints");
            if (!targets.is_array() || targets.empty() || targets.size() > 8)
                throw std::invalid_argument(
                    "PostProcess constraint targets must contain 1..8 entries");
            float total = 0.0f;
            for (const auto& target : targets)
            {
                RejectUnknown(target, {"bone", "weight"}, "constraints.targets");
                ConstraintTarget value;
                value.bone = ReadString(
                    Required(target, "bone", "constraints.targets"), "targets.bone");
                if (value.bone.empty())
                    throw std::invalid_argument("PostProcess constraint target bone is empty");
                value.weight = ReadNumber(
                    Required(target, "weight", "constraints.targets"), "targets.weight");
                if (value.weight < 0.0f)
                    throw std::invalid_argument(
                        "PostProcess constraint weight must be nonnegative");
                total += value.weight;
                entry.targets.push_back(std::move(value));
            }
            if (total <= KindaSmallNumber)
                throw std::invalid_argument("PostProcess constraint weights sum to zero");
            asset.constraints.push_back(std::move(entry));
        }
    }
    if (asset.twist.empty() && asset.helpers.empty() && asset.constraints.empty())
        throw std::invalid_argument(
            "PostProcess requires at least one constraint, twist or helper entry");
    return asset;
}

std::shared_ptr<const PostProcessAsset> LoadPostProcessAsset(
    const std::filesystem::path& resourceRoot, const std::string& relativePath)
{
    const auto relative = std::filesystem::u8path(relativePath);
    if (relativePath.empty() || relativePath.size() > 1024 || relative.is_absolute() ||
        relative.has_root_name() || relativePath.find('\\') != std::string::npos ||
        relativePath.find(':') != std::string::npos ||
        relative.lexically_normal().generic_string() != relativePath ||
        relative.extension() != ".json" ||
        (relativePath.rfind("Maps/", 0) != 0 && relativePath.rfind("Common/", 0) != 0))
        throw std::invalid_argument("PostProcess requires a formal resource path");
    const auto canonicalRoot = std::filesystem::canonical(resourceRoot);
    const auto resolved = std::filesystem::weakly_canonical(canonicalRoot / relative);
    const auto inside = resolved.lexically_relative(canonicalRoot);
    if (inside.empty() || *inside.begin() == "..")
        throw std::invalid_argument("PostProcess escapes the resource root");
    if (!std::filesystem::exists(resolved))
        throw std::runtime_error("PostProcess asset does not exist: " + relativePath);
    if (std::filesystem::file_size(resolved) > 4 * 1024 * 1024)
        throw std::invalid_argument("PostProcess asset exceeds 4 MiB");
    std::ifstream input(resolved, std::ios::binary);
    const std::string bytes((std::istreambuf_iterator<char>(input)),
        std::istreambuf_iterator<char>());
    if (!input || bytes.size() > 4 * 1024 * 1024)
        throw std::runtime_error("Cannot read PostProcess asset: " + relativePath);
    auto asset = std::make_shared<PostProcessAsset>(
        ParsePostProcessAsset(nlohmann::json::parse(bytes), relativePath));
    asset->sourceDigest = ContentHasher::HashString(bytes).ToHex();
    return asset;
}

PostProcessSolver::PostProcessSolver(const SkeletonMesh& mesh,
    std::shared_ptr<const PostProcessAsset> asset)
    : asset(std::move(asset)), mesh(&mesh)
{
    if (!this->asset)
        throw std::invalid_argument("PostProcess asset is null");

    auto findJoint = [&](const std::string& name)
    {
        for (size_t joint = 0; joint < mesh.joints.size(); ++joint)
            if (mesh.joints[joint].name == name) return static_cast<int>(joint);
        return -1;
    };

    std::vector<Eigen::Quaternionf> bindGlobals;
    bool bindGlobalsReady = false;
    auto ensureBindGlobals = [&]
    {
        if (bindGlobalsReady) return;
        bindGlobals.assign(mesh.joints.size(), Eigen::Quaternionf::Identity());
        for (size_t joint = 0; joint < bindGlobals.size(); ++joint)
        {
            const Eigen::Quaternionf local = NormalizedRotation(mesh.joints[joint].restLocal);
            const int parent = mesh.joints[joint].parent;
            bindGlobals[joint] = parent >= 0 && static_cast<size_t>(parent) < joint ?
                (bindGlobals[static_cast<size_t>(parent)] * local).normalized() : local;
        }
        bindGlobalsReady = true;
    };

    for (const auto& definition : this->asset->twist)
    {
        auto compiled = std::make_unique<CompiledTwist>();
        compiled->definition = &definition;
        compiled->driver = findJoint(definition.driverBone);
        if (compiled->driver < 0)
        {
            bindDiagnostics.push_back({definition.name, "missing-driver-bone",
                definition.driverBone});
            continue;
        }
        compiled->bindRotation =
            NormalizedRotation(mesh.joints[static_cast<size_t>(compiled->driver)].restLocal);
        bool complete = true;
        for (const auto& target : definition.targets)
        {
            const int joint = findJoint(target.bone);
            if (joint < 0)
            {
                bindDiagnostics.push_back({definition.name, "missing-target-bone", target.bone});
                complete = false;
                continue;
            }
            compiled->targets.emplace_back(joint, target.weight);
        }
        // 单条目目标骨缺失时整条跳过：伪造成只写一半会让左右不对称。
        if (!complete || compiled->targets.empty())
        {
            compiled->targets.clear();
            continue;
        }
        ++validTwist;
        twist.push_back(std::move(compiled));
    }

    for (const auto& definition : this->asset->helpers)
    {
        auto compiled = std::make_unique<CompiledHelper>();
        compiled->definition = &definition;
        compiled->blendT = definition.blendT;
        bool complete = true;
        for (const auto& projection : definition.projections)
        {
            CompiledHelper::Projection value;
            value.child = findJoint(projection.child);
            value.oldParent = findJoint(projection.oldParent);
            value.newParent = findJoint(projection.newParent);
            value.childInitial = projection.childInitial;
            value.oldParentInitial = projection.oldParentInitial;
            value.newParentInitial = projection.newParentInitial;
            if (value.child < 0 || value.oldParent < 0 || value.newParent < 0)
            {
                bindDiagnostics.push_back({definition.name, "missing-projection-bone",
                    value.child < 0 ? projection.child : value.oldParent < 0 ?
                        projection.oldParent : projection.newParent});
                complete = false;
                continue;
            }
            ensureBindGlobals();
            value.childBind = bindGlobals[static_cast<size_t>(value.child)];
            value.oldParentBind = bindGlobals[static_cast<size_t>(value.oldParent)];
            value.newParentBind = bindGlobals[static_cast<size_t>(value.newParent)];
            compiled->projections.push_back(value);
        }
        for (const auto& target : definition.targets)
        {
            const int joint = findJoint(target.bone);
            if (joint < 0)
            {
                bindDiagnostics.push_back({definition.name, "missing-target-bone", target.bone});
                complete = false;
                continue;
            }
            compiled->targets.emplace_back(joint, target.weight);
        }
        if (!complete || compiled->projections.empty() || compiled->targets.empty())
        {
            compiled->projections.clear();
            compiled->targets.clear();
            continue;
        }
        ++validHelpers;
        helpers.push_back(std::move(compiled));
    }

    if (!this->asset->constraints.empty())
    {
        LocalPose restPose((mesh.joints.size() + 3) / 4);
        for (auto& value : restPose)
        {
            value.translation.x = ozz::math::simd_float4::zero();
            value.translation.y = ozz::math::simd_float4::zero();
            value.translation.z = ozz::math::simd_float4::zero();
            value.rotation.x = ozz::math::simd_float4::zero();
            value.rotation.y = ozz::math::simd_float4::zero();
            value.rotation.z = ozz::math::simd_float4::zero();
            value.rotation.w = ozz::math::simd_float4::one();
            value.scale.x = ozz::math::simd_float4::one();
            value.scale.y = ozz::math::simd_float4::one();
            value.scale.z = ozz::math::simd_float4::one();
        }
        for (size_t joint = 0; joint < mesh.joints.size(); ++joint)
        {
            const Eigen::Matrix4f& rest = mesh.joints[joint].restLocal;
            WriteRotation(restPose, joint, NormalizedRotation(rest));
            WriteTranslation(restPose, joint, rest.block<3, 1>(0, 3));
        }
        std::vector<Eigen::Matrix4f> refGlobals;
        GlobalMatrices(mesh, restPose, refGlobals);

        for (const auto& definition : this->asset->constraints)
        {
            auto compiled = std::make_unique<CompiledConstraint>();
            compiled->definition = &definition;
            compiled->bone = findJoint(definition.bone);
            if (compiled->bone < 0)
            {
                bindDiagnostics.push_back({definition.name, "missing-constraint-bone",
                    definition.bone});
                continue;
            }
            bool complete = true;
            for (const auto& target : definition.targets)
            {
                CompiledConstraint::Target value;
                value.joint = findJoint(target.bone);
                value.weight = target.weight;
                if (value.joint < 0)
                {
                    bindDiagnostics.push_back({definition.name, "missing-constraint-target",
                        target.bone});
                    complete = false;
                    continue;
                }
                value.offset = refGlobals[static_cast<size_t>(value.joint)].inverse() *
                    refGlobals[static_cast<size_t>(compiled->bone)];
                compiled->targets.push_back(value);
            }
            if (!complete || compiled->targets.empty())
            {
                compiled->targets.clear();
                continue;
            }
            ++validConstraints;
            constraints.push_back(std::move(compiled));
        }
    }
}

PostProcessSolver::PostProcessSolver(PostProcessSolver&&) noexcept = default;
PostProcessSolver& PostProcessSolver::operator=(PostProcessSolver&&) noexcept = default;
PostProcessSolver::~PostProcessSolver() = default;

bool PostProcessSolver::Apply(LocalPose& locals, PostProcessApplyStats* stats) const
{
    if (!mesh || locals.empty() || (twist.empty() && helpers.empty() && constraints.empty()))
        return false;
    const size_t jointCount = static_cast<size_t>(locals.size()) * 4;
    bool changed = false;
    int rotated = 0;
    float maxAngle = 0.0f;

    // UE 顺序：Constraint → Twist → Helpers。Constraint 是组件空间加权混合，
    // 每写一根骨都重算 FK，保证后续约束看到最新姿态。
    if (!constraints.empty())
    {
        std::vector<Eigen::Matrix4f> currentGlobals;
        for (const auto& entry : constraints)
        {
            GlobalMatrices(*mesh, locals, currentGlobals);
            if (currentGlobals.size() != mesh->joints.size()) break;
            float total = 0.0f;
            for (const auto& target : entry->targets) total += target.weight;
            if (total <= KindaSmallNumber) continue;
            const float multiply = total > 1.0f ? 1.0f / total : 1.0f;

            Eigen::Vector3f translation = Eigen::Vector3f::Zero();
            Eigen::Quaternionf rotation = Eigen::Quaternionf::Identity();
            bool first = true;
            for (const auto& target : entry->targets)
            {
                if (target.joint < 0 || static_cast<size_t>(target.joint) >= currentGlobals.size())
                    continue;
                const Eigen::Matrix4f value = target.offset *
                    currentGlobals[static_cast<size_t>(target.joint)];
                const float weight = target.weight * multiply;
                translation += value.block<3, 1>(0, 3) * weight;
                Eigen::Quaternionf q(value.block<3, 3>(0, 0));
                q.normalize();
                if (first)
                {
                    rotation.coeffs() = q.coeffs() * weight;
                    first = false;
                }
                else
                {
                    // UE AccumulateWithShortestRotation：先对齐符号再加权求和。
                    if (q.dot(rotation) < 0.0f) q.coeffs() = -q.coeffs();
                    rotation.coeffs() += q.coeffs() * weight;
                }
            }
            if (first) continue;
            rotation.normalize();
            const size_t joint = static_cast<size_t>(entry->bone);
            if (joint >= jointCount) continue;
            const int parent = mesh->joints[joint].parent;
            const Eigen::Matrix4f parentGlobal = parent >= 0 ?
                currentGlobals[static_cast<size_t>(parent)] : Eigen::Matrix4f::Identity();
            Eigen::Matrix4f blended = Eigen::Matrix4f::Identity();
            blended.block<3, 3>(0, 0) = rotation.toRotationMatrix();
            blended.block<3, 1>(0, 3) = translation;
            const Eigen::Matrix4f local = parentGlobal.inverse() * blended;
            WriteTranslation(locals, joint, local.block<3, 1>(0, 3));
            WriteRotation(locals, joint, Eigen::Quaternionf(local.block<3, 3>(0, 0)).normalized());
            changed = true;
            ++rotated;
        }
    }

    for (const auto& entry : twist)
    {
        const auto& definition = *entry->definition;
        if (entry->driver < 0 || static_cast<size_t>(entry->driver) >= jointCount)
            continue;
        const size_t driver = static_cast<size_t>(entry->driver);
        const Eigen::Quaternionf current = ReadRotation(locals, driver);
        // delta = bind^-1 * current；两个旋转都在同一局部空间，等价于 UE 的
        // FTransform::GetRelativeTransform(initial, current)。
        const Eigen::Quaternionf delta =
            (entry->bindRotation.conjugate() * current).normalized();
        Eigen::Quaternionf swing, value;
        ToSwingTwist(delta, PoseDriverAxisVector(definition.twistAxis), swing, value);
        if (definition.invert) value = value.conjugate();
        maxAngle = std::max(maxAngle, 2.0f * std::acos(std::clamp(std::abs(value.w()),
            0.0f, 1.0f)) * 180.0f / Pi);
        for (const auto& target : entry->targets)
        {
            if (target.first < 0 || static_cast<size_t>(target.first) >= jointCount)
                continue;
            const size_t joint = static_cast<size_t>(target.first);
            const Eigen::Quaternionf from = ReadRotation(locals, joint);
            // ControlRig SetRotation(Weight, LocalSpace)：对当前局部旋转做最短路径 slerp。
            const Eigen::Quaternionf blended =
                target.second >= 1.0f ? value : from.slerp(target.second, value).normalized();
            WriteRotation(locals, joint, blended);
            changed = true;
            ++rotated;
        }
    }

    if (!helpers.empty())
    {
        std::vector<Eigen::Quaternionf> currentGlobals;
        for (const auto& entry : helpers)
        {
            // Project To New Parent 的 bNewParentInitial=false 读取“当前”全局旋转，
            // 因此每次写骨后都要重算一次 FK；骨架规模很小，代价可忽略。
            GlobalRotations(*mesh, locals, currentGlobals);
            if (currentGlobals.size() != mesh->joints.size()) break;
            auto projectionValue = [&](const CompiledHelper::Projection& projection)
            {
                const Eigen::Quaternionf child = projection.childInitial ?
                    projection.childBind : currentGlobals[static_cast<size_t>(projection.child)];
                const Eigen::Quaternionf oldParent = projection.oldParentInitial ?
                    projection.oldParentBind :
                    currentGlobals[static_cast<size_t>(projection.oldParent)];
                const Eigen::Quaternionf newParent = projection.newParentInitial ?
                    projection.newParentBind :
                    currentGlobals[static_cast<size_t>(projection.newParent)];
                // 相对旧父骨，再挂到新父骨：NewParent * OldParent^-1 * Child。
                return (newParent * (oldParent.conjugate() * child)).normalized();
            };
            Eigen::Quaternionf value;
            if (entry->projections.size() >= 2)
            {
                const Eigen::Quaternionf a = projectionValue(entry->projections[0]);
                const Eigen::Quaternionf b = projectionValue(entry->projections[1]);
                value = entry->blendT <= 0.0f ? a :
                    entry->blendT >= 1.0f ? b : a.slerp(entry->blendT, b).normalized();
            }
            else
                value = projectionValue(entry->projections[0]);

            for (const auto& target : entry->targets)
            {
                if (target.first < 0 || static_cast<size_t>(target.first) >= jointCount)
                    continue;
                const size_t joint = static_cast<size_t>(target.first);
                const int parent = mesh->joints[joint].parent;
                const Eigen::Quaternionf parentGlobal = parent >= 0 ?
                    currentGlobals[static_cast<size_t>(parent)] : Eigen::Quaternionf::Identity();
                const Eigen::Quaternionf currentGlobal = currentGlobals[joint];
                // SetRotation(GlobalSpace, Weight)：先对全局旋转做 slerp，再还原为局部。
                const Eigen::Quaternionf blended = target.second >= 1.0f ? value :
                    currentGlobal.slerp(target.second, value).normalized();
                WriteRotation(locals, joint, (parentGlobal.conjugate() * blended).normalized());
                changed = true;
                ++rotated;
            }
        }
    }

    if (stats)
    {
        stats->rotatedBones = rotated;
        stats->maxTwistDegrees = maxAngle;
    }
    return changed;
}
}
