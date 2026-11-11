#pragma once

#include "animationAssets.h"
#include "footPlacement.h"

#include <Eigen/Dense>
#include <cstddef>
#include <string>

namespace VL::Animation
{
// Echo_SlopeWarping_CtrlRig 的正式原生替代：只修正局部姿态，不写 Actor/Jolt，
// 也不消费或改写根运动。Echo locomotion 不使用 FootPlacement，但两者共享同一条
// “基础姿态 -> 视觉修正 -> 蒙皮快照”的阶段。
struct SlopeWarpingParameters
{
    // 总体混合权重；0 表示完全关闭姿态修正。
    float weight = 1.0f;
    // 角色局部 up 与地面 normal 的最大对齐角，超过时按该角度 clamp。
    float maxSlopeDegrees = 45.0f;
    // 沿世界 up 的额外骨盆/整身抬高，单位 cm，随 weight 平滑。
    float pelvisHeightOffset = 0.0f;
    // 最低脚相对探针地面的补偿比例；0 关闭脚部高度补偿，1 完整贴合。
    float footCompensation = 1.0f;
    // 权重与高度修正的指数响应速度，1/s；0 表示立即到位。
    float responseSpeed = 12.0f;
    // 单帧骨盆/根补偿的绝对上限，单位 cm。
    float maxPelvisOffset = 30.0f;
    // 小于该角度的表面视为平地，直接 no-op。
    float flatAngleDegrees = 0.5f;
};

struct SlopeWarpingState
{
    bool enabled = false;
    bool active = false;
    // 原始地面坡度（角色 up 与 support normal 的夹角）。
    float slopeAngleDegrees = 0.0f;
    // 经 maxSlopeDegrees clamp 后的目标角。
    float targetSlopeAngleDegrees = 0.0f;
    // 经平滑权重缩放的当前实际应用角。
    float appliedSlopeAngleDegrees = 0.0f;
    // 当前平滑权重 0..1。
    float weight = 0.0f;
    // 本帧沿世界 up 的骨盆/根补偿，cm。
    float pelvisOffset = 0.0f;
    Eigen::Vector3f groundNormal = Eigen::Vector3f::UnitY();
    std::string supportIdentity;
    std::string reason = "disabled";
};

// 绑定时解析 root/pelvis/foot 关节；缺骨时构造成功但永远 inactive，
// 这样资源缺骨只会安全停用，不会把播放器构造变成硬失败。
class SlopeWarpingSolver
{
public:
    explicit SlopeWarpingSolver(const SkeletonMesh& mesh);

    void SetEnabled(bool value);
    bool Enabled() const { return state.enabled; }
    void SetParameters(const SlopeWarpingParameters& value);
    const SlopeWarpingParameters& Parameters() const { return parameters; }
    void Reset(const char* reason);

    // locals/models 来自基础姿态；component 是骨骼网格的刚性世界变换。
    // probe 只读，求解器不持有 World/Jolt。changed=false 时调用方不得替换姿态。
    LocalPose Solve(const SkeletonMesh& mesh, const LocalPose& locals,
        const std::vector<Eigen::Matrix4f>& models, const Eigen::Matrix4f& component,
        bool grounded, const std::string& owner, float deltaSeconds,
        const GroundProbe& probe, bool& changed);

    const SlopeWarpingState& State() const { return state; }
    bool Valid() const { return valid; }
    const std::string& MissingJoint() const { return missingJoint; }

private:
    size_t root = 0;
    size_t pelvis = 0;
    std::vector<size_t> feet;
    bool valid = false;
    std::string missingJoint;
    SlopeWarpingParameters parameters;
    SlopeWarpingState state;
};
}
