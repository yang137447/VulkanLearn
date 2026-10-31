#pragma once

#include <cstdint>
#include <vector>

#include <Eigen/Core>

namespace VL::Physics::Fracture
{

using Point2 = Eigen::Vector2d;
using Polygon2 = std::vector<Point2>;

// 2D 冲击破碎参数。当前只处理凸多边形域（玻璃板、薄片），
// 不做任意三角网格的体素化或凹多边形分解。
struct PlanarFractureSettings
{
    // 同一 seed + 同一输入必须得到同一碎片布局；游戏回放和网络同步都依赖这一点。
    uint64_t seed = 0;
    // 期望的 Voronoi 种子数，实际数量会因去重和域外投影略少；上限 256。
    int siteCount = 24;
    // 0 = 均匀随机；1 = 尽量把种子排成以 impact 为中心的同心环。
    double radialBias = 0.75;
    // 同心环种子的角度和半径抖动，按域包围盒尺度归一化。
    double jitterFraction = 0.12;
    // 小于该面积的退化 cell 不进入结果。仅用于过滤数值碎片，不用于常规预算裁剪。
    double degenerateArea = 1e-10;
};

// 径向裂纹 + 同心环图案。玻璃/FEM 冲击更常见的形态：冲击点是细碎区，
// 外向是放射状长楔形和较少的环向裂纹，碎片尺寸随半径明显增大，
// 而不是各向同性 Voronoi 那种接近等大的圆润多边形。
struct RadialFractureSettings
{
    uint64_t seed = 0;
    // 是否在冲击点周围为每个扇区生成一个细碎三角（粉碎区）。
    double coreRadius = 10.0;
    // 径向主裂纹数量（扇区数），结果约为 radialCracks * (1 + 环数)。
    int radialCracks = 12;
    // 每个扇区的同心环数量上限；最外侧总会补一个覆盖到域边界的环。
    int ringCount = 3;
    // 相邻环半径增长倍率，越接近真实玻璃的“内密外疏”。
    double ringGrowth = 1.55;
    // > 0 时在 stopRadius 处强制一圈裂纹边界：半径内是破碎区，半径外保持完整；
    // 0 表示一直覆盖到域边界（整板碎裂）。环半径会在 coreRadius..stopRadius 内
    // 按几何级数重新分配，保证最后一圈落在伤害边界上。
    double stopRadius = 0.0;
    // 径向裂纹角度和环半径的相对抖动 [0, 1]。
    double angularJitter = 0.35;
    double ringJitter = 0.25;
    // 每个母扇区按概率再分成 2-3 个细支裂纹；细支从冲击点一直延伸到边界，
    // 形成玻璃常见的细长楔形碎片，同时保持相邻 cell 边界一致。
    double dicingProbability = 0.65;
    double degenerateArea = 1e-10;
};

// 一个凸的平面碎片。outline 为 XY 平面内 CCW 顺序，centroid 是面积质心。
struct PlanarShard
{
    Polygon2 outline;
    double area = 0.0;
    Point2 centroid = Point2::Zero();
    uint32_t patternIndex = 0;
};

struct PlanarPattern
{
    std::vector<PlanarShard> shards;
    double coveredArea = 0.0;
};

// 多边形有向面积；CCW 为正。输入必须至少 3 个点。
double SignedArea(const Polygon2& polygon);
Point2 PolygonCentroid(const Polygon2& polygon);
bool IsConvex(const Polygon2& polygon, double tolerance = 1e-9);

// 保留 n·p <= offset 的一侧。使用 Sutherland-Hodgman 裁剪凸多边形；
// 输入不是凸多边形时仍能裁剪，但输出的拓扑不再是保证凸的 Voronoi cell。
Polygon2 ClipPolygonByHalfPlane(
    const Polygon2& polygon,
    const Point2& normal,
    double offset);

// 以 impact 为冲击中心生成平面 Voronoi 碎片。domain 必须是 CCW 凸多边形；
// 返回的碎片覆盖 domain，面积之和与 domain 的差仅来自浮点误差。
PlanarPattern GenerateImpactPattern(
    const Polygon2& domain,
    const Point2& impact,
    const PlanarFractureSettings& settings);

// 以 impact 为中心生成径向裂纹/同心环图案。domain 必须是 CCW 凸多边形；
// 输出覆盖整个 domain，面积和与 domain 只差浮点误差。
PlanarPattern GenerateRadialImpactPattern(
    const Polygon2& domain,
    const Point2& impact,
    const RadialFractureSettings& settings);

// 对单个凸碎片继续切分。用于玻璃碎片落地后的二次破碎；
// 调用方负责限制递归深度和全局碎片预算。
PlanarPattern SplitPlanarShard(
    const PlanarShard& shard,
    const PlanarFractureSettings& settings);

// 用随机直线弦把凸碎片切成若干尖片。玻璃二次碎裂比 Voronoi 更接近真实的
// 尖锐长条；cutCount 表示切割次数，结果最多 cutCount + 1 片。
PlanarPattern SplitPlanarShardByCracks(
    const PlanarShard& shard,
    int cutCount,
    uint64_t seed);

} // namespace VL::Physics::Fracture
