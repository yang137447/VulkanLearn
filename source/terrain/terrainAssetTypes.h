#pragma once

#include <algorithm>
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

// 运行时 LOD 切换的滞回比例。相机在阈值附近抖动时，10% 的带宽可以避免
// 相邻级每帧来回切换；带宽以阈值本身为基准，远近两侧各 10%。
inline constexpr float TerrainLodHysteresisFraction = 0.10f;

// 点到世界 AABB 的最近距离（cm）。相机在盒内返回 0；地形 LOD 选择用
// chunk 的完整 AABB 而不是中心点，避免超大 chunk 在近处被错误地当成远景。
inline float TerrainLodDistanceToAabbCm(
    const Eigen::Vector3f& point,
    const Eigen::Vector3f& boundsMin,
    const Eigen::Vector3f& boundsMax)
{
    const Eigen::Vector3f outside(
        std::max(std::max(boundsMin.x() - point.x(), point.x() - boundsMax.x()), 0.0f),
        std::max(std::max(boundsMin.y() - point.y(), point.y() - boundsMax.y()), 0.0f),
        std::max(std::max(boundsMin.z() - point.z(), point.z() - boundsMax.z()), 0.0f));
    return outside.norm();
}

// 带滞回的运行时选择：第一次选择（hasPreviousLevel=false）与纯函数一致；
// 之后只有越过阈值 ±hysteresisFraction 的带宽才换级。返回级号范围
// [0, lodDistancesCm.size()]。
inline uint32_t TerrainSelectLodLevelHysteresis(
    float distanceCm,
    const std::vector<float>& lodDistancesCm,
    bool hasPreviousLevel,
    uint32_t previousLevel,
    float hysteresisFraction = TerrainLodHysteresisFraction)
{
    const uint32_t maxLevel =
        static_cast<uint32_t>(lodDistancesCm.size());
    if (!hasPreviousLevel)
        return std::min(TerrainSelectLodLevel(distanceCm, lodDistancesCm), maxLevel);

    const float distance = distanceCm > 0.0f ? distanceCm : 0.0f;
    const float hysteresis =
        hysteresisFraction > 0.0f ? hysteresisFraction : 0.0f;
    uint32_t level = std::min(previousLevel, maxLevel);

    // 变粗：必须超过“阈值 + 10%”才升级，防止边界抖动。
    while (level < maxLevel &&
        distance >= lodDistancesCm[level] * (1.0f + hysteresis))
    {
        ++level;
    }
    // 变细：必须低于“阈值 - 10%”才降级。
    while (level > 0 &&
        distance < lodDistancesCm[level - 1] * (1.0f - hysteresis))
    {
        --level;
    }
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
    // 默认取与覆盖页缺页中性表面一致的中性灰（0.18）；全黑会让 mode 1 的
    // 缺页块在光照下像空洞，因此旧 TR 不写该字段时也不能是 0。
    Eigen::Vector3f avgColor = Eigen::Vector3f(0.18f, 0.18f, 0.18f);
    // 未来层 atlas 分组与 sRGB/alpha/biome 等保留位；L1 只透传。
    uint32_t sourceSet = 0;
    uint32_t flags = 0;
    // 跨 TR 共享层的显式身份。为空时由 TerrainLayerRegistry 回退成
    // "<assetPath>#<name>"；同一 key 必须参数完全一致，否则 World 加载失败。
    // key 只是身份，不参与 catalog 采样；参数哈希不作为 ID（内容一改 ID 就漂移）。
    // 放在结构体末尾，保持既有聚合初始化（name/height/... 顺序）源码兼容。
    std::string key;
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
    // 几何 chunk 驻留窗口（cm，严格升序，长度 = lodLevels-1）。第 l 级
    // （0..lodLevels-2）只有在相机到 chunk AABB 距离 <= 该值时才创建/驻留；
    // 最粗一级永远驻留，不进入该表。为空时由 TerrainChunkResidencyDistancesCm
    // 从 lodDistances 推导（×1.1 覆盖滞回带上沿）。
    std::vector<float> chunkResidencyDistancesCm;
};

// 距离→阈值推导。renderer 用它在加载期把阈值写进每个 chunk 的 LOD 组，
// WorldSnapshotBuilder 每帧按相机到 chunk AABB 的距离选择级；返回值不参与
// chunk 资源 key（同一级网格与相机位置无关）。
inline std::vector<float> TerrainLodDistancesCm(const TerrainAssetDesc& desc)
{
    if (!desc.lodDistancesCm.empty())
        return desc.lodDistancesCm;
    std::vector<float> distances;
    const uint32_t levels = desc.lodLevels == 0 ? 1u : desc.lodLevels;
    if (levels <= 1)
        return distances;
    // 默认档位：按级数均分最长边（2 级 = 0.5×；4 级 = 0.25/0.5/0.75×）。
    // 缺少测量数据时的保守占位，运行时应按投影误差/三角形像素尺寸重新标定。
    const float span = desc.size.x() > desc.size.y() ? desc.size.x() : desc.size.y();
    distances.reserve(levels - 1);
    for (uint32_t index = 0; index + 1 < levels; ++index)
        distances.push_back(
            span * static_cast<float>(index + 1) / static_cast<float>(levels));
    return distances;
}

// 几何 chunk 驻留窗口。返回长度 = lodLevels-1，与 lodDistances 一一对应：
// 第 l 级在该距离内视为 resident；最粗一级不查表（永远 resident）。
// 缺省值取 lodDistances[l] × (1 + 滞回比例)，与运行时“变粗”阈值同源，
// 保证加载期不会把“刚好处在滞回带内”的目标级漏建。
inline std::vector<float> TerrainChunkResidencyDistancesCm(
    const TerrainAssetDesc& desc)
{
    if (!desc.chunkResidencyDistancesCm.empty())
        return desc.chunkResidencyDistancesCm;
    std::vector<float> distances = TerrainLodDistancesCm(desc);
    for (float& distance : distances)
        distance *= 1.0f + TerrainLodHysteresisFraction;
    return distances;
}

// 几何 chunk residency 的纯函数判定：最粗一级恒为 resident；其余级看距离
// 窗口（闭区间，等于窗口值仍算 resident）。表长度不足时按“不驻留”处理，
// 由调用方保证资产校验已经拒绝了这种输入。
inline bool TerrainChunkLodIsResident(
    uint32_t lodLevel,
    uint32_t lodLevelCount,
    float distanceCm,
    const std::vector<float>& residencyDistancesCm)
{
    if (lodLevelCount == 0)
        return true;
    if (lodLevel >= lodLevelCount - 1)
        return true;
    if (lodLevel >= residencyDistancesCm.size())
        return false;
    const float distance = distanceCm > 0.0f ? distanceCm : 0.0f;
    return distance <= residencyDistancesCm[lodLevel];
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
