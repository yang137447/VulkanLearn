#pragma once

#include "animationAssets.h"
#include "poseDriver.h"

#include <Eigen/Dense>
#include <nlohmann/json.hpp>

#include <filesystem>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace VL::Animation
{
// UE PostProcess AnimBP 的正式原生子集（Echo_PostProcess_AnimBP）：
//   Echo_Twist_CtrlRig   → twist 段（骨骼相对旋转 → swing/twist → 写 twist 骨）
//   Echo_Helpers_CtrlRig → helpers 段（Project To New Parent → GlobalSpace 设置旋转）
// 两者都只改局部姿态，不触碰 Actor/Jolt/root motion。

struct TwistTarget
{
    std::string bone;
    float weight = 1.0f;
};

struct TwistEntry
{
    std::string name;
    std::string driverBone;
    PoseDriverAxis twistAxis = PoseDriverAxis::X;
    // upperarm/thigh 组写 inverse(twist)，hand/foot 组直接写 twist。
    bool invert = false;
    std::vector<TwistTarget> targets;
};

// FRigUnit_ProjectTransformToNewParent 的输入。
struct HelperProjection
{
    std::string child;
    std::string oldParent;
    std::string newParent;
    bool childInitial = true;
    bool oldParentInitial = true;
    bool newParentInitial = false;
};

struct HelperEntry
{
    std::string name;
    // 1 或 2 个投影；2 个时按 blendT 做四元数插值（Echo 的裙摆前中骨为 0.5）。
    std::vector<HelperProjection> projections;
    float blendT = 0.0f;
    std::vector<TwistTarget> targets;
};

// UE AnimNode_Constraint（TransformType=Parent + OffsetOption=RefPose）。
// 结果目标是组件空间变换，因此这一段的输入输出都在组件空间完成。
struct ConstraintTarget
{
    std::string bone;
    float weight = 1.0f;
};

struct ConstraintEntry
{
    std::string name;
    std::string bone;
    std::vector<ConstraintTarget> targets;
};

struct PostProcessAsset
{
    std::string format;
    std::string id;
    std::string sourcePath;
    std::string sourceDigest;
    std::vector<TwistEntry> twist;
    std::vector<HelperEntry> helpers;
    std::vector<ConstraintEntry> constraints;
};

struct PostProcessDiagnostic
{
    std::string entry;
    std::string code;
    std::string detail;
};

// 每帧求解统计：写入的骨骼数与最大扭转角，供反馈/验收使用。
struct PostProcessApplyStats
{
    int rotatedBones = 0;
    float maxTwistDegrees = 0.0f;
};

// 纯解析；未知字段/非法权重/空 targets 一律拒绝。twist 与 helpers 至少一段非空。
PostProcessAsset ParsePostProcessAsset(const nlohmann::json& json, std::string_view sourcePath);

std::shared_ptr<const PostProcessAsset> LoadPostProcessAsset(
    const std::filesystem::path& resourceRoot, const std::string& relativePath);

class PostProcessSolver
{
public:
    PostProcessSolver(const SkeletonMesh& mesh, std::shared_ptr<const PostProcessAsset> asset);
    PostProcessSolver(const PostProcessSolver&) = delete;
    PostProcessSolver& operator=(const PostProcessSolver&) = delete;
    PostProcessSolver(PostProcessSolver&&) noexcept;
    PostProcessSolver& operator=(PostProcessSolver&&) noexcept;
    ~PostProcessSolver();

    const std::shared_ptr<const PostProcessAsset>& Asset() const { return asset; }
    const std::vector<PostProcessDiagnostic>& BindDiagnostics() const { return bindDiagnostics; }
    bool Valid() const { return validConstraints + validTwist + validHelpers > 0; }

    // 就地写局部旋转；返回是否真的改动了姿态。缺骨条目在绑定期已降级为跳过。
    // 顺序固定为 twist → helpers（与 UE PostProcess 图一致）。
    bool Apply(LocalPose& locals, PostProcessApplyStats* stats = nullptr) const;

private:
    struct CompiledTwist;
    struct CompiledHelper;
    struct CompiledConstraint;
    std::shared_ptr<const PostProcessAsset> asset;
    const SkeletonMesh* mesh = nullptr;
    std::vector<std::unique_ptr<CompiledTwist>> twist;
    std::vector<std::unique_ptr<CompiledHelper>> helpers;
    std::vector<std::unique_ptr<CompiledConstraint>> constraints;
    std::vector<PostProcessDiagnostic> bindDiagnostics;
    size_t validTwist = 0;
    size_t validHelpers = 0;
    size_t validConstraints = 0;
};
}
