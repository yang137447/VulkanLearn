#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include <Eigen/Dense>

#include "resource/image/hostImage.h"

namespace VL
{

// 长远目标的全局页表边长（tile 数）；单个地形通过 pageOrigin 占用其中一块。
inline constexpr uint32_t TerrainPageTableSize = 32;

// 单页分辨率（每边 texel 数）。必须与 TerrainPageManager::PageResolution 以及
// M_terrain / M_terrainFeedback 的 u_pageResolution 保持一致；网格页 UV 的
// 半 texel 内缩和 CPU 页 bake 共用这一粒度。
inline constexpr uint32_t TerrainPageResolution = 128;

// 层源纹理的分页粒度与覆盖页共用 128：页数 = 层分辨率 / 128。
// 1K 层得到 mip0..3（8/4/2/1 页/边，共 85 页）；2K 层多一级 mip4
// （1 页/边，共 341 页）；512 层只有 mip0..2（4/2/1，共 21 页）。
// 2K 的 mip4 是 128² 的“尾页”，与设计 §4.4 的 tail 页对应。
inline constexpr uint32_t TerrainLayerMaxMipLevel = 4;

inline bool IsSupportedTerrainLayerSourceResolution(uint32_t resolution)
{
    return resolution == 512 || resolution == 1024 || resolution == 2048;
}

// 该源分辨率的有效 mip 级数（含 mip0）。
inline uint32_t TerrainLayerMipCount(uint32_t sourceResolution)
{
    const uint32_t base = sourceResolution / TerrainPageResolution;
    uint32_t count = 0;
    for (uint32_t side = base; side > 0; side >>= 1)
        ++count;
    return count;
}

// 指定 mip 的每边页数；越界 mip 返回 0。
inline uint32_t TerrainLayerPagesPerSide(uint32_t sourceResolution, uint32_t mip)
{
    const uint32_t base = sourceResolution / TerrainPageResolution;
    if (mip >= TerrainLayerMipCount(sourceResolution))
        return 0;
    const uint32_t side = base >> mip;
    return side == 0 ? 1 : side;
}

inline uint32_t TerrainLayerPagesPerMip(uint32_t sourceResolution, uint32_t mip)
{
    const uint32_t side = TerrainLayerPagesPerSide(sourceResolution, mip);
    return side * side;
}

// 该 mip 在“每层扁平页号”里的起始项：前面所有 mip 的页数之和。
inline uint32_t TerrainLayerMipPageBase(uint32_t sourceResolution, uint32_t mip)
{
    uint32_t base = 0;
    for (uint32_t level = 0; level < mip; ++level)
        base += TerrainLayerPagesPerMip(sourceResolution, level);
    return base;
}

inline uint32_t TerrainLayerTotalPages(uint32_t sourceResolution)
{
    return TerrainLayerMipPageBase(
        sourceResolution, TerrainLayerMipCount(sourceResolution));
}

// CPU 几何 LOD 的级数上限。lod 0 是完整网格（每 quad 一个格子），
// 第 n 级把每个方向的 quad 数除以 2^n；超过 3 级时 chunkQuads 很快退化到
// 1~2 个格子，再细分不会再有可见收益。
inline constexpr uint32_t TerrainMaxLodLevels = 4;

// 第 lodLevel 级在 X/Z 方向合并的 quad 数（lod 0 = 1）。
inline uint32_t TerrainLodStepQuads(uint32_t lodLevel)
{
    return 1u << lodLevel;
}

// 距离 → LOD 级（CPU 侧，纯函数）：distance 达到第 i 个阈值就切到 i+1，
// 超过最后一个阈值用最粗一级。负距离按 0 处理；空阈值表恒为 LOD0。
// 阈值来源见 TerrainAssetDesc::lodDistancesCm / TerrainLodDistancesCm。
inline uint32_t TerrainSelectLodLevel(
    float distanceCm,
    const std::vector<float>& lodDistancesCm)
{
    const float distance = distanceCm > 0.0f ? distanceCm : 0.0f;
    uint32_t level = 0;
    while (level < lodDistancesCm.size() && distance >= lodDistancesCm[level])
        ++level;
    return level;
}

// 一层地表调色板的作者数据。M2 只校验并携带，M3 的 top-4 材质才开始消费。
struct TerrainLayerDesc
{
    std::string name;
    float heightMin = 0.0f;
    float heightMax = 1.0f;
    float slopeMin = 0.0f;
    float slopeMax = 90.0f;
    float noise = 0.0f;
    // 每层独立 UV 缩放/旋转；Phase B 的 layer table 会把它送进 shader。
    float uvScale = 1.0f;
    float uvRotation = 0.0f;
    // L1 可选字段，旧 TR 不写时保持原行为。源分辨率只允许 512/1024/2048；
    // catalog 的 pagesPerSide 由它与 128 页分辨率推导。
    uint32_t sourceResolution = 1024;
    // 缺页/低 mip 回退色（线性空间）。L1 只写进 catalog，L2 起由 VT 消费。
    Eigen::Vector3f avgColor = Eigen::Vector3f::Zero();
    // 未来层 atlas 分组与 sRGB/alpha/biome 等保留位；L1 只透传。
    uint32_t sourceSet = 0;
    uint32_t flags = 0;
};

// TR_*.json 解析后的几何/材质描述。size、heightScale、heightOffset 的单位都是 cm，
// 与世界单位一致；heightmap 是资源根相对路径，不套材质纹理的 V 翻转。
struct TerrainAssetDesc
{
    std::string assetPath;
    std::string name;
    std::string heightmapPath;
    Eigen::Vector2f size = Eigen::Vector2f(100000.0f, 100000.0f);
    float heightScale = 0.0f;
    float heightOffset = 0.0f;
    uint32_t gridResolution = 256;
    uint32_t chunkQuads = 64;
    // 该地形在全局 page table 中的 tile 原点与本地 tile 边长。
    Eigen::Vector2i pageOrigin = Eigen::Vector2i(0, 0);
    uint32_t pageTilesPerSide = 8;
    std::string materialInstancePath;
    std::vector<TerrainLayerDesc> layers;
    // CPU 几何 LOD：1 表示只有 LOD0（旧资产默认），最多 4 级。
    uint32_t lodLevels = 1;
    // LOD 切换距离（cm，严格升序，长度 = lodLevels-1）。为空时由
    // TerrainLodDistancesCm 按地形尺寸推导。
    std::vector<float> lodDistancesCm;
};

// 运行时选择尚未接入：renderer 目前只生成/绑定 LOD0。本函数提供可测的
// 距离→阈值推导，供后续按相机距离选择时复用；返回值不参与 chunk 资源 key。
inline std::vector<float> TerrainLodDistancesCm(const TerrainAssetDesc& desc)
{
    if (!desc.lodDistancesCm.empty())
        return desc.lodDistancesCm;
    std::vector<float> distances;
    const uint32_t levels = desc.lodLevels == 0 ? 1u : desc.lodLevels;
    if (levels <= 1)
        return distances;
    // 默认档位：1×、2×…地形最长边。缺少测量数据时的保守占位，
    // 运行时应按投影误差/三角形像素尺寸重新标定。
    const float span = desc.size.x() > desc.size.y() ? desc.size.x() : desc.size.y();
    distances.reserve(levels - 1);
    for (uint32_t index = 0; index + 1 < levels; ++index)
        distances.push_back(span * static_cast<float>(index + 1));
    return distances;
}

// 已解码的高度图与描述一起随场景计划传递；shared_ptr 避免在候选/World 计划复制时
// 复制整张 1024² float 图。高度图只读，运行时网格生成消费同一份像素。
struct TerrainBuildPlan
{
    TerrainAssetDesc desc;
    std::shared_ptr<const HostImage> heightmap;
    // 一次场景摆放的稳定身份，由 SceneAssetValidator 写成 `terrain|<objectName>`。
    // 同一 TR 可以在一个 World 里摆放多次，assetPath 因此不再是唯一注册键；
    // 为空时由 TerrainPlacementIdentity 回退到 assetPath，兼容旧测试/旧资产。
    std::string placementId;
};

// 注册/页键/chunk key 共用的摆放身份。返回值引用 plan 内部字符串，调用方不得
// 在 plan 生命周期结束之后继续持有。
inline const std::string& TerrainPlacementIdentity(const TerrainBuildPlan& plan)
{
    return plan.placementId.empty() ? plan.desc.assetPath : plan.placementId;
}

// 只有声明了 layers 的地形才参与共享 VT 页表。没有 layers 的地形走普通
// M_pbr/M_unlit 材质，既没有虚拟页可反馈，也不应在同一 World 里污染页表。
inline bool TerrainUsesVirtualPages(const TerrainBuildPlan& plan)
{
    return !plan.desc.layers.empty();
}

// feedback 的完整 eligibility：既要材质真正绑定 layerTable，也要地形本身
// 声明了可注册的虚拟页。抽成纯函数让 loader 与测试共享同一守卫，避免
// “只有 layers”或“只有材质槽”任一单边条件误驱动驻留。
inline bool TerrainParticipatesInFeedback(
    const TerrainBuildPlan& plan,
    bool materialUsesLayerTable)
{
    return materialUsesLayerTable && TerrainUsesVirtualPages(plan);
}

} // namespace VL
