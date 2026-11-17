#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include <Eigen/Dense>

#include "terrain/terrainAssetTypes.h"
#include "terrain/terrainCollisionTypes.h"

namespace VL
{

// Jolt HeightFieldShape 的样本数限制与块对齐。代理和 body 共用这组常量，
// 避免“代理按一个分辨率采样、body 又向上取整”的第二套规则。
inline constexpr uint32_t TerrainCollisionMinSampleCount = 4;
inline constexpr uint32_t TerrainCollisionMaxSampleCount = 4096;
inline constexpr uint32_t TerrainCollisionHeightFieldBlockSize = 2;

// 0 = 跟随 gridResolution + 1；否则使用请求值，并向上对齐到 Jolt block size。
// 越界抛 std::invalid_argument。
uint32_t ResolveTerrainCollisionSampleCount(
    uint32_t requestedSampleCount,
    uint32_t gridResolution);

// 碰撞专用高度场代理。它不持有 TR 层表、材质实例、渲染 LOD 或纹理：
// 只有采样后的世界 Y（cm）、世界 X/Z 跨度、摆放和保守 AABB。
// Step 1 仍从 TR 宏高度图生成 gridResolution+1 样本，行为与旧 Jolt
// heightfield 路径等价；后续 patch streamer 可以直接消费这份类型。
struct TerrainCollisionProxy
{
    std::string placementId;
    TerrainCollisionPlacement placement;
    uint32_t sampleCount = 0;
    // 世界 X/Z 全宽（已乘 placement.scale.x/z）。
    float sizeXCm = 0.0f;
    float sizeZCm = 0.0f;
    // sampleCount²，行优先；世界 Y（cm，已乘 placement.scale.y）。
    std::vector<float> heightCm;
    Eigen::Vector3f boundsMinCm = Eigen::Vector3f::Zero();
    Eigen::Vector3f boundsMaxCm = Eigen::Vector3f::Zero();

    bool IsValid() const;
};

// 从已验证的 TerrainBuildPlan 采样碰撞代理。输入非法抛 std::invalid_argument；
// heightmap margin 不匹配等资产错误仍由 TerrainHeightmap 抛出。
std::shared_ptr<const TerrainCollisionProxy> BuildTerrainCollisionProxy(
    const TerrainBuildPlan& plan,
    const TerrainCollisionPlacement& placement,
    uint32_t requestedSampleCount = 0);

} // namespace VL
