#pragma once

#include <memory>
#include <optional>
#include <string>
#include <vector>

#include <Eigen/Core>
#include <Eigen/Geometry>

#include "physics/fracture/fractureTypes.h"

namespace VL::Physics
{

// 一次原生 Jolt 接触事件。targetRole 描述被撞对象在结构中的角色，contact
// 描述接触形态；materialProfile 才是材质分派键。objectName 是场景对象名，
// shardName 只在 attached/free shard 事件里有值。位置/法线是项目厘米单位；
// relativeSpeed 是接触时的速度大小（米/秒，与 Jolt 一致）。
struct FractureImpact
{
    FractureBodyRole targetRole = FractureBodyRole::StaticTarget;
    FractureContactKind contact = FractureContactKind::Impact;
    FractureMaterialProfile materialProfile =
        FractureMaterialProfile::Generic;
    uint32_t targetBodyId = 0;
    std::string objectName;
    std::string shardName;
    Eigen::Vector3f point = Eigen::Vector3f::Zero();
    Eigen::Vector3f normal = Eigen::Vector3f::UnitY();
    float relativeSpeed = 0.0f;
    // 接触速度的法向分量（米/秒）。破坏阈值应看真正压入材料的分量，
    // 而不是被重力/切向速度抬高的总速度。
    float normalSpeed = 0.0f;
    // 命中目标的投射物句柄；只在 BalloonHit/PaneHit 上有意义。
    uint32_t projectileId = 0;
};

struct FractureShardPose
{
    Eigen::Vector3f position = Eigen::Vector3f::Zero();
    Eigen::Quaternionf rotation = Eigen::Quaternionf::Identity();
};

// 原生 Jolt 冲击世界：静态球体代理（气球）、静态盒代理（玻璃板）、
// 动态投射物和动态凸包碎片。软体粒子与玻璃渲染网格仍由各自运行时拥有。
// 它不属于 C# Blueprint 的脚本物理世界。
class FractureImpactWorld
{
public:
    FractureImpactWorld();
    ~FractureImpactWorld();

    // 气球：静态球体代理，撞击后整个软壳爆裂。
    void RegisterSphereProxy(const std::string& name,
        const Eigen::Vector3f& center, float radius,
        FractureMaterialProfile profile =
            FractureMaterialProfile::Balloon);
    void UnregisterSphereProxy(const std::string& name);
    // 玻璃板：静态盒代理，halfExtents 是板厚方向过半尺寸（厘米）。
    void RegisterPane(const std::string& name, const Eigen::Vector3f& center,
        const Eigen::Quaternionf& rotation, const Eigen::Vector3f& halfExtents,
        FractureMaterialProfile profile =
            FractureMaterialProfile::Glass);
    void UnregisterPane(const std::string& name);

    // 凸包碎片。center/rotation 是该碎片局部坐标系的世界姿态；localHullPoints
    // 与旋转同属一个坐标系，已按世界尺度缩放。
    void RegisterShard(const std::string& objectName,
        const std::string& shardName, const Eigen::Vector3f& center,
        const Eigen::Quaternionf& rotation, const Eigen::Vector3f& velocity,
        float mass, const std::vector<Eigen::Vector3f>& localHullPoints,
        FractureMaterialProfile profile,
        float friction = 0.35f, float restitution = 0.12f,
        const Eigen::Vector3f& angularVelocity = Eigen::Vector3f::Zero(),
        bool dynamic = true);
    void RemoveShard(const std::string& objectName, const std::string& shardName);

    void SetGroundPlane(float heightCentimeters);
    std::optional<FractureShardPose> GetShardPose(
        const std::string& objectName, const std::string& shardName) const;
    // 返回碎片线速度（厘米/秒）；二次碎裂需要继承父碎片动量。
    std::optional<Eigen::Vector3f> GetShardVelocity(
        const std::string& objectName, const std::string& shardName) const;
    void Clear();
    // 返回投射物句柄；它同时出现在命中事件的 projectileId 里，供可见箭头
    // 在命中点冻结姿态。
    uint32_t LaunchProjectile(const Eigen::Vector3f& position,
        const Eigen::Vector3f& velocity, float radius, float mass);
    // 主动取消未命中的投射物；TTL/越界/数量上限都应由投射物 owner 调用。
    void CancelProjectile(uint32_t projectileId);
    bool HasProjectile(uint32_t projectileId) const;
    std::optional<FractureShardPose> GetProjectilePose(uint32_t projectileId) const;
    std::optional<Eigen::Vector3f> GetProjectileVelocity(uint32_t projectileId) const;
    // 漏气/移动中的静态球代理需要跟随软壳；只移动 body，不改变形状。
    void SetSphereProxyPose(
        const std::string& name, const Eigen::Vector3f& center);

    // 输入输出均为厘米；内部只在 Jolt 边界换算成米。
    std::vector<FractureImpact> Step(float deltaSeconds);

private:
    struct Impl;
    std::unique_ptr<Impl> impl;
};

} // namespace VL::Physics
