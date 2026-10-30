#pragma once

#include <memory>
#include <optional>
#include <string>
#include <vector>

#include <Eigen/Core>
#include <Eigen/Geometry>

namespace VL::Physics
{

enum class FractureImpactType
{
    BalloonHit,
    PaneHit,
    // 命中“裂而不掉”的玻璃板片：该板片随后转成动态碎片。
    AttachedHit,
    ShardLanding,
    ShardHit
};

// 目标材质 profile。冲击世界把 proxy 注册时的 profile 写进每个事件，
// 气球/玻璃运行时据此选择破坏策略和音频，而不是靠对象名或事件顺序猜。
// Generic 留给尚未接入专用运行时的目标。
enum class FractureMaterialProfile
{
    Generic = 0,
    Glass,
    Balloon
};

const char* FractureMaterialProfileName(FractureMaterialProfile profile);

// 一次原生 Jolt 接触事件。objectName 是 balloon 或 glassPane 的场景对象名；
// shardName 只在碎片事件里有值。位置/法线是项目厘米单位；
// relativeSpeed 是接触时的速度大小（米/秒，与 Jolt 一致）。
struct FractureImpact
{
    FractureImpactType type = FractureImpactType::BalloonHit;
    FractureMaterialProfile materialProfile =
        FractureMaterialProfile::Generic;
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
    bool HasProjectile(uint32_t projectileId) const;
    std::optional<FractureShardPose> GetProjectilePose(uint32_t projectileId) const;
    std::optional<Eigen::Vector3f> GetProjectileVelocity(uint32_t projectileId) const;

    // 输入输出均为厘米；内部只在 Jolt 边界换算成米。
    std::vector<FractureImpact> Step(float deltaSeconds);

private:
    struct Impl;
    std::unique_ptr<Impl> impl;
};

} // namespace VL::Physics
