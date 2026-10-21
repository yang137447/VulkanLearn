#pragma once

#include "animationAssets.h"
#include <array>

namespace VL::Animation
{
struct FootGroundHit
{
    bool hit = false;
    Eigen::Vector3f point = Eigen::Vector3f::Zero();
    Eigen::Vector3f normal = Eigen::Vector3f::UnitY();
    std::string supportIdentity;
};

// GT 同步纯值接地查询；调用方过滤 owner/动态支撑，本模块不持有 World 或 Jolt。
class GroundProbe
{
public:
    virtual ~GroundProbe() = default;
    virtual FootGroundHit ProbeGround(const std::string& owner, const Eigen::Vector3f& origin,
        const Eigen::Vector3f& displacement) const = 0;
};

struct FootPlacementFootState
{
    bool grounded = false;
    bool planted = false;
    Eigen::Vector3f anchor = Eigen::Vector3f::Zero();
    Eigen::Vector3f preSole = Eigen::Vector3f::Zero();
    Eigen::Vector3f postSole = Eigen::Vector3f::Zero();
    Eigen::Vector3f groundPoint = Eigen::Vector3f::Zero();
    Eigen::Vector3f groundNormal = Eigen::Vector3f::UnitY();
    Eigen::Vector3f probeOrigin = Eigen::Vector3f::Zero();
    Eigen::Vector3f probeDisplacement = Eigen::Vector3f::Zero();
    float targetError = 0;
    // 求解判据的只读快照，用于诊断“命中但未锁”的原因；不参与求解。
    float soleSpeed = 0, heelSpeed = 0, soleGap = 0, heelGap = 0;
    bool canPlantBall = false, canPlantHeel = false;
    std::string supportIdentity;
    bool heelGrounded = false;
    Eigen::Vector3f preHeel = Eigen::Vector3f::Zero();
    Eigen::Vector3f postHeel = Eigen::Vector3f::Zero();
    Eigen::Vector3f heelGroundPoint = Eigen::Vector3f::Zero();
    Eigen::Vector3f heelGroundNormal = Eigen::Vector3f::UnitY();
    Eigen::Vector3f heelProbeOrigin = Eigen::Vector3f::Zero();
    Eigen::Vector3f heelProbeDisplacement = Eigen::Vector3f::Zero();
    std::string heelSupportIdentity;
    // pre/postSole 与 ground 字段仍表示 ball；anchor/targetError 属于当前明确选定的支点。
    std::string contactMode = "none";
};

// 与 UE FAnimNode_FootPlacement 的 LockType 对齐：脚骨 IK 固定，只选择锚点参考。
// PivotAroundBall 用 ball 投影作支点（UE 默认）；PivotAroundAnkle 用踝下代理。
enum class FootLockType
{
    // VL 扩展：按更低参考点与滚动自动选支点（UE 当前节点尚未实现自动支点）。
    Auto,
    PivotAroundBall,
    PivotAroundAnkle
};

struct FootPlacementState
{
    bool enabled = false;
    bool active = false;
    float pelvisOffset = 0;
    std::array<FootPlacementFootState, 2> feet;
    std::string lockMode = "auto";
    std::string reason = "disabled";
};

// 绑定时派生腿链/膝轴与 foot/ball 平面参考点；heel 是踝下代理，均非鞋底网格真值。
class FootPlacementSolver
{
public:
    explicit FootPlacementSolver(const SkeletonMesh& mesh);
    void SetEnabled(bool value);
    void SetLockType(FootLockType value);
    FootLockType LockType() const { return lockType; }
    void Reset(const char* reason);
    static void ValidateComponent(const Eigen::Matrix4f& component);
    LocalPose Solve(const SkeletonMesh& mesh, const LocalPose& locals,
        const std::vector<Eigen::Matrix4f>& preIkModels, const Eigen::Matrix4f& component,
        bool grounded, const std::string& owner, float deltaSeconds, const GroundProbe& probe);
    const FootPlacementState& State() const { return state; }
    void Measure(const std::vector<Eigen::Matrix4f>& models, const Eigen::Matrix4f& component);

private:
    struct Leg
    {
        size_t thigh = 0, calf = 0, foot = 0, ball = 0;
        Eigen::Vector3f soleLocal = Eigen::Vector3f::Zero();
        Eigen::Vector3f heelLocal = Eigen::Vector3f::Zero();
        Eigen::Vector3f upLocal = Eigen::Vector3f::UnitY();
        Eigen::Vector3f midAxis = Eigen::Vector3f::UnitZ();
        Eigen::Vector3f previousSole = Eigen::Vector3f::Zero();
        Eigen::Vector3f previousHeel = Eigen::Vector3f::Zero();
        bool heelPivot = false;
    };
    size_t pelvis = 0;
    std::array<Leg, 2> legs;
    FootPlacementState state;
    FootLockType lockType = FootLockType::Auto;
    bool historyValid = false;
    Eigen::Matrix4f previousComponent = Eigen::Matrix4f::Identity();
};
}
