#pragma once

#include <cstddef>
#include <filesystem>
#include <memory>
#include <map>
#include <string>
#include <vector>
#include <Eigen/Core>
#include <nlohmann/json.hpp>
#include "animation/footPlacement.h"
#include "terrain/terrainCollisionTypes.h"

namespace VL
{
class TerrainHeightTextureCache;

// GT 独占的 Jolt CPU 物理世界；脚本批次使用 JSON，动画同步探测只传世界空间纯值。
// 不访问 renderer、文件或托管对象；每次 Play 重建，Stop/World 换代释放。
class ScriptPhysicsWorld : public Animation::GroundProbe
{
public:
    ScriptPhysicsWorld();
    ~ScriptPhysicsWorld();
    // 每帧物理入口：dt 为帧间隔（已乘 timeScale）。角色按帧推进，刚体按内部 1/60 子步追赶；
    // dt=0 只同步状态/指令并收集接触，不移动任何实体。没有固定步入口。
    void Step(nlohmann::json& states, const nlohmann::json& declarations, float dt);
    nlohmann::json Contacts() const;
    nlohmann::json Triggers() const;
    nlohmann::json Raycast(const nlohmann::json& requests) const;
    Animation::FootGroundHit ProbeGround(const std::string& owner, const Eigen::Vector3f& origin,
        const Eigen::Vector3f& displacement) const override;
    void SetMeshGeometry(const std::map<std::string, std::vector<Eigen::Vector3f>>& triangles);
    // Play 时把 World 的地形摆放一次性交进来：先销毁旧 body，再为每个 placement
    // 创建静态 heightfield。重复调用是“替换”语义；空集合只清空。
    // body 由本对象持有，析构/Stop 时一定早于 PhysicsSystem 销毁。
    //
    // resourceRoot 非空时同时建立 CPU 同源位移采样器（脚 IK 的视觉真值）；
    // 纹理缓存由调用方跨 Play 持有，避免每次 Play 重新解码 16-bit 层高度图。
    // 空 resourceRoot 只建宏高度碰撞（测试/无资产夹具）。
    void SetTerrainCollisions(
        const std::vector<TerrainCollisionInstance>& instances,
        const std::filesystem::path& resourceRoot = {},
        std::shared_ptr<TerrainHeightTextureCache> textureCache = {});
    size_t TerrainCollisionBodyCount() const;
    std::vector<std::string> TerrainCollisionPlacementIds() const;
    size_t TerrainDisplacementSamplerCount() const;
private:
    void StepInternal(nlohmann::json& states, const nlohmann::json& declarations, float dt, int substeps);
    struct Impl;
    std::unique_ptr<Impl> impl;
};
} // namespace VL
