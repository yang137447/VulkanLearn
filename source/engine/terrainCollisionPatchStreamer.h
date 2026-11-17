#pragma once

#include <cstdint>
#include <future>
#include <memory>
#include <string>
#include <vector>

#include <Eigen/Dense>
#include <Jolt/Jolt.h>
#include <Jolt/Physics/Body/BodyID.h>

#include "terrain/terrainCollision.h"
#include "terrain/terrainCollisionPatch.h"
#include "terrain/terrainCollisionProxy.h"

namespace VL
{

// 角色周围的高精度碰撞 patch 流式器。Patch 高度只来自宏碰撞代理（A 口径），
// 由 worker 构建；GT 在 Update() 里领取 ready 结果并创建/替换 Jolt body。
// 同一时刻最多一个 patch，pin 在角色所在 64 m 单元；超出宏代理边缘则回退宏。
class TerrainCollisionPatchStreamer
{
public:
    struct Settings
    {
        TerrainCollisionPatchBuildSettings patch;
        // sampleCount 被忽略；collisionGroup 由调用方设为 patch 专用 group。
        TerrainCollisionSettings body;
    };

    TerrainCollisionPatchStreamer(
        JPH::PhysicsSystem& world,
        Settings settings);
    ~TerrainCollisionPatchStreamer();
    TerrainCollisionPatchStreamer(const TerrainCollisionPatchStreamer&) = delete;
    TerrainCollisionPatchStreamer& operator=(
        const TerrainCollisionPatchStreamer&) = delete;

    void SetProxies(
        std::vector<std::shared_ptr<const TerrainCollisionProxy>> proxies);
    // characterPositionCm 是世界位置（cm）。返回 true 表示 active patch
    // body/key 发生了变化（调用方需要同步碰撞过滤和 terrain identity）。
    bool Update(const Eigen::Vector3f& characterPositionCm);
    void Reset();

    bool HasActivePatch() const { return activeBody.IsValid(); }
    JPH::BodyID ActivePatchBodyId() const;
    const std::string& ActivePlacementId() const;
    uint32_t ActivePatchSampleCount() const;

private:
    std::shared_ptr<const TerrainCollisionProxy> FindProxy(
        const Eigen::Vector3f& position,
        Eigen::Vector2f& outLocalXz) const;
    bool PatchFits(
        const TerrainCollisionProxy& proxy,
        const Eigen::Vector2f& centerLocalCm) const;
    bool PollPending(const std::string& desiredKey);
    void CreateActive(
        const std::shared_ptr<const TerrainCollisionPatch>& patch,
        const std::string& key);
    void ClearActive();

    JPH::PhysicsSystem& world;
    Settings settings;
    std::vector<std::shared_ptr<const TerrainCollisionProxy>> proxies;
    std::future<std::shared_ptr<const TerrainCollisionPatch>> pending;
    std::string pendingKey;
    std::shared_ptr<const TerrainCollisionPatch> activePatch;
    TerrainCollisionBody activeBody;
    std::string activeKey;
};

} // namespace VL
