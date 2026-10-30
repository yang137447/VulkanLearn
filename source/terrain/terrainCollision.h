#pragma once

#include <cstdint>
#include <memory>

#include <Eigen/Dense>
#include <Jolt/Jolt.h>
#include <Jolt/Physics/Body/BodyID.h>

namespace JPH
{
class PhysicsSystem;
}

namespace VL
{

struct TerrainBuildPlan;

// 一次地形摆放的世界变换，字段与场景对象的 position/rotation/scale 一一对应：
// position 单位 cm、rotation 单位度（Y*X*Z，与 CommonFunction::RotationToQuat 一致）、
// scale 是无量纲乘数。Jolt 世界本身用米，换算只发生在 terrainCollision 内部。
struct TerrainCollisionPlacement
{
    Eigen::Vector3f position = Eigen::Vector3f::Zero();
    Eigen::Vector3f rotation = Eigen::Vector3f::Zero();
    Eigen::Vector3f scale = Eigen::Vector3f::Ones();
};

struct TerrainCollisionSettings
{
    // 每边高度样本数；0 表示跟随 TR.gridResolution + 1（与渲染网格同分辨率），
    // 再向上取整到 Jolt heightfield blockSize 的倍数。允许区间 [4, 4096]。
    uint32_t sampleCount = 0;
    // Jolt 高度压缩位数；16 位在块内 min/max 上量化，块内误差约为高度范围/65536。
    uint32_t bitsPerSample = 16;
    // Jolt object layer；默认 0，与 ScriptPhysicsWorld 的 StaticLayer 对齐。
    // 该层必须已由调用方在 PhysicsSystem 的 ObjectLayerPairFilter 中注册。
    uint32_t objectLayer = 0;
    // 调用方 PhysicsSystem 实际注册的 object layer 数量；用于在创建前拒绝越界层。
    // 默认 2 与 TerrainCollisionWorld / ScriptPhysicsWorld 的静态+移动两层一致。
    uint32_t objectLayerCount = 2;
};

// 地形静态 body 的 RAII 句柄。析构时从创建它的 PhysicsSystem 移除并销毁 body。
// 句柄必须早于对应 PhysicsSystem 销毁；本类不持有 PhysicsSystem 的所有权。
class TerrainCollisionBody
{
public:
    TerrainCollisionBody();
    ~TerrainCollisionBody();
    TerrainCollisionBody(TerrainCollisionBody&& other) noexcept;
    TerrainCollisionBody& operator=(TerrainCollisionBody&& other) noexcept;
    TerrainCollisionBody(const TerrainCollisionBody&) = delete;
    TerrainCollisionBody& operator=(const TerrainCollisionBody&) = delete;

    // 提前销毁；可重复调用。之后 IsValid() 为 false。
    void Reset();
    bool IsValid() const;
    // Jolt BodyID 的索引+序列号打包值；销毁后索引会被复用，但序列号让这个值
    // 在同一 PhysicsSystem 内保持唯一。仅用于日志/测试断言，不是持久化身份。
    uint32_t GetBodyIdValue() const;
    // 实际参与碰撞的高度场每边样本数（已按 blockSize 对齐）。
    uint32_t GetSampleCount() const;
    // heightfield 的四边形数量 = (sampleCount - 1)^2；不是渲染网格的三角形数。
    uint32_t GetQuadCount() const;
    // 世界空间 AABB（cm）。旋转后的真实包围盒，不是 heightfield 的局部范围。
    Eigen::Vector3f GetWorldBoundsMinCm() const;
    Eigen::Vector3f GetWorldBoundsMaxCm() const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl;

    friend TerrainCollisionBody CreateTerrainCollisionBody(
        JPH::PhysicsSystem&,
        const TerrainBuildPlan&,
        const TerrainCollisionPlacement&,
        const TerrainCollisionSettings&);
};

// 用 TR 高度图在既有 Jolt PhysicsSystem 里创建一个静态 heightfield body。
// 不创建第二套物理世界；调用方传入的 world 必须已经注册 0..objectLayer 的层级。
// 输入不合法（空高度图、非正 size/scale、样本数越界、非有限高度）抛 std::exception，
// 且不会留下已加入的 body。
TerrainCollisionBody CreateTerrainCollisionBody(
    JPH::PhysicsSystem& world,
    const TerrainBuildPlan& plan,
    const TerrainCollisionPlacement& placement = {},
    const TerrainCollisionSettings& settings = {});

// 独立测试/离线夹具：拥有唯一一个 Jolt PhysicsSystem（与 ScriptPhysicsWorld 的
// 静态/移动两层配置一致），用于模块测试或尚未接入 World 的工具。生产路径应把
// 已有的 PhysicsSystem 传给 CreateTerrainCollisionBody，而不是再建一套世界。
class TerrainCollisionWorld
{
public:
    TerrainCollisionWorld();
    ~TerrainCollisionWorld();
    TerrainCollisionWorld(const TerrainCollisionWorld&) = delete;
    TerrainCollisionWorld& operator=(const TerrainCollisionWorld&) = delete;

    JPH::PhysicsSystem& GetPhysicsSystem();
    const JPH::PhysicsSystem& GetPhysicsSystem() const;

    TerrainCollisionBody AddTerrain(
        const TerrainBuildPlan& plan,
        const TerrainCollisionPlacement& placement = {},
        const TerrainCollisionSettings& settings = {});

    // 向下射线（originCm 单位 cm）；命中返回 true，outPointCm 为命中点（cm）。
    // 未命中/超出 distanceCm 返回 false。不改变世界状态。
    bool RaycastDown(
        const Eigen::Vector3f& originCm,
        float distanceCm,
        Eigen::Vector3f& outPointCm,
        Eigen::Vector3f* outNormal = nullptr) const;

    // 固定步长推进；仅测试夹具需要，生产路径由 ScriptPhysicsWorld 驱动。
    void Step(float deltaSeconds);

private:
    struct Impl;
    std::unique_ptr<Impl> impl;
};

} // namespace VL
