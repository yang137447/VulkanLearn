#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

#include <Eigen/Dense>

#include "world/worldSnapshot.h"

namespace VL
{

// 屏幕空间 tile 光照聚类。
//
// 设计约束：
//   - 每帧由 LightCulling compute 重写全部 tile，网格缓冲因此不需要 CPU 清零；
//   - 方向光始终进入每个 tile；点光/聚光按屏幕投影包围球 + 视深度范围裁剪；
//   - 每 tile 固定容量，溢出确定性截断并计数，绝不越界写入索引表；
//   - CPU 侧只保留“与 shader 同公式”的纯数学，供单元测试钉住语义，
//     运行时不参与逐帧聚类。
inline constexpr uint32_t kLightCullingTileSize = 16;
inline constexpr uint32_t kLightCullingMaxLightsPerTile = 64;
// compute shader 的 workgroup 尺寸；dispatch 以 tile 为单位。
inline constexpr uint32_t kLightCullingGroupSize = 8;

// 与 shader/glsl/pass/lightCulling.comp 的 std140 块逐字段一致。
// 4 × uvec4 = 64 字节，每个 uvec4 天然满足 std140 的 16 字节对齐。
struct alignas(16) LightGridParams
{
    // x = tileSize，y = tilesX，z = tilesY，w = maxLightsPerTile
    uint32_t tileSize = kLightCullingTileSize;
    uint32_t tilesX = 0;
    uint32_t tilesY = 0;
    uint32_t maxLightsPerTile = kLightCullingMaxLightsPerTile;
    // x = enabled，y = renderWidth，z = renderHeight，w = reserved
    uint32_t enabled = 0;
    uint32_t renderWidth = 0;
    uint32_t renderHeight = 0;
    uint32_t reserved0 = 0;
    // x = pointLightOffset，y = pointLightCount，z = spotLightOffset，w = spotLightCount。
    // 只用于诊断；shader 以 UBOLight header 为准，避免与上传缓冲出现两份真相。
    uint32_t pointLightOffset = 0;
    uint32_t pointLightCount = 0;
    uint32_t spotLightOffset = 0;
    uint32_t spotLightCount = 0;
    // 预留：后续按相机/视图扩展（例如正交相机开关）。
    uint32_t reserved1[4] = {};
};

static_assert(sizeof(LightGridParams) == 64,
    "LightGridParams must match the std140 layout consumed by lightCulling.comp");

// lightGrid 缓冲布局：header + tiles[]，每项 4 × uint（16 字节）。
// header.x = 发生溢出的 tile 数，header.y = 丢弃的灯索引总数。
inline constexpr uint32_t kLightGridTileStride = 4;

inline size_t LightGridHeaderBytes()
{
    return sizeof(uint32_t) * 4;
}

inline size_t LightGridBytes(uint32_t tileCount)
{
    return LightGridHeaderBytes() +
        static_cast<size_t>(tileCount) * kLightGridTileStride * sizeof(uint32_t);
}

inline size_t LightIndexListBytes(uint32_t tileCount)
{
    return static_cast<size_t>(tileCount) *
        kLightCullingMaxLightsPerTile * sizeof(uint32_t);
}

// 渲染目标尺寸 → tile 网格。所有渲染目标宽度/高度都至少 1。
void ComputeLightTileGrid(
    uint32_t renderWidth,
    uint32_t renderHeight,
    uint32_t& outTilesX,
    uint32_t& outTilesY);

struct LightCullingCamera
{
    // 与 UBOGlobal 一致：view 前向为 -Z，projection 已含 Vulkan NDC 变换。
    Eigen::Matrix4f view = Eigen::Matrix4f::Identity();
    Eigen::Matrix4f projection = Eigen::Matrix4f::Identity();
    uint32_t width = 1;
    uint32_t height = 1;
    float nearCm = 1.0f;
    float farCm = 1000.0f;
};

struct LightScreenBounds
{
    bool visible = false;
    float minX = 0.0f;
    float minY = 0.0f;
    float maxX = 0.0f;
    float maxY = 0.0f;
};

// 与 lightCulling.comp 的 ComputeLightScreenBounds 同一套算术：
//   1. 视空间深度 + 半径完全在 near 之前 / far 之后 → 不可见；
//   2. 包围球跨过相机平面（clip.w <= 0）→ 保守覆盖全屏；
//   3. 其余按包围球投影到像素半径，得到屏幕 AABB。
// sphereRadius <= 0 表示作者未限制半径，按“无限范围”覆盖全屏。
LightScreenBounds ComputeLightScreenBounds(
    const Eigen::Vector3f& lightCenter,
    float sphereRadius,
    const LightCullingCamera& camera);

// 聚光用“顶点 + 底面圆”的最小包围球近似（比直接用 radius 更紧，且不漏灯）。
Eigen::Vector3f ComputeSpotCullingCenter(const LightSnapshot& light);
float ComputeSpotCullingRadius(const LightSnapshot& light);

// tile 像素矩形与屏幕 AABB 是否相交。边界接触按相交处理（保守）。
bool TileOverlapsLightBounds(
    const LightScreenBounds& bounds,
    uint32_t tileX,
    uint32_t tileY,
    uint32_t tileSize,
    uint32_t width,
    uint32_t height);

// 测试用：复刻 compute 的确定性追加顺序与溢出截断。
struct LightTileAccumulator
{
    uint32_t capacity = kLightCullingMaxLightsPerTile;
    uint32_t overflow = 0;
    std::vector<uint32_t> indices;

    // 返回 true 表示已写入；false 表示容量已满、已计入 overflow。
    bool Append(uint32_t lightIndex);
};

// CPU 参考实现：按“方向光 → 点光 → 聚光”的固定顺序填充给定 tile。
// 只用于单元测试，不参与运行时路径。
void BuildLightTileReference(
    const std::vector<LightSnapshot>& lights,
    const LightCullingCamera& camera,
    uint32_t tileX,
    uint32_t tileY,
    uint32_t tileSize,
    LightTileAccumulator& accumulator);

} // namespace VL
