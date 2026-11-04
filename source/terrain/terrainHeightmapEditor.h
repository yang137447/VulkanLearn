#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include <Eigen/Core>

#include "terrain/terrainAssetTypes.h"
#include "terrain/terrainBrushShape.h"
#include "terrain/terrainMeshBuilder.h"

namespace VL
{

// 一次 Sculpt 笔刷模式。当前只做高度雕刻；层权重绘制需要独立的 override 层，
// 不能复用这里的单一高度样本（见文件末尾的边界说明）。
enum class TerrainSculptMode
{
    Raise,
    Lower,
    Smooth,
};

// 编辑器像素矩形，坐标是高度图 texel 下标，min/max 都包含在范围内。
// v 对应 +Z，与 TerrainHeightmap::SampleHeightCm 的 u/v 约定一致。
struct TerrainPixelRect
{
    uint32_t minX = 0;
    uint32_t minY = 0;
    uint32_t maxX = 0;
    uint32_t maxY = 0;
    bool valid = false;

    uint32_t Width() const noexcept
    {
        return valid ? maxX - minX + 1 : 0;
    }

    uint32_t Height() const noexcept
    {
        return valid ? maxY - minY + 1 : 0;
    }

    bool Contains(uint32_t x, uint32_t y) const noexcept
    {
        return valid && x >= minX && x <= maxX && y >= minY && y <= maxY;
    }
};

// 单个高度图上受影响的 chunk。x/z 是 LOD0 chunk 网格下标。
struct TerrainDirtyChunk
{
    uint32_t x = 0;
    uint32_t z = 0;

    bool operator==(const TerrainDirtyChunk& other) const noexcept
    {
        return x == other.x && z == other.z;
    }
};

// 一次笔刷的参数。radiusUv 是归一化 UV 半径（不是 cm），因此非正方形地形上
// 的笔刷足迹在世界上是椭圆；UI 若需要世界空间圆形笔刷，应在调用前把 cm
// 半径换算成 UV，并明确接受这一各向异性。
// strength 对 Raise/Lower 是归一化高度增量，对 Smooth 是最大混合系数。
// falloff=0 为硬边，falloff=1 从圆心开始平滑衰减到边缘。
struct TerrainSculptBrush
{
    TerrainSculptMode mode = TerrainSculptMode::Raise;
    TerrainBrushShape shape = TerrainBrushShape::Circle;
    Eigen::Vector2f centerUv = Eigen::Vector2f::Zero();
    float radiusUv = 0.0f;
    float strength = 0.0f;
    float falloff = 1.0f;
};

// 一次笔刷的结果。dirtyRect 只覆盖实际发生数值变化的 texel；没有可写像素
// （例如半径太小、完全在半径外或全部被 clamp）时 applied=false，revision 不变。
// changedMin/Max 是本次变化像素的新值范围。
struct TerrainSculptResult
{
    bool applied = false;
    uint64_t revision = 0;
    TerrainPixelRect dirtyRect;
    float changedMin = 0.0f;
    float changedMax = 0.0f;
};

// Landscape Sculpt 的纯 CPU 数据源。构造时复制 TerrainBuildPlan 的高度图，
// 不持有 GPU、World 或 renderer 状态；所有修改都发生在自己的 float 副本中。
//
// 采样约定与运行时完全一致：u 对应 +X、v 对应 +Z，UV 0..1 线性映射到
// [0, width-1] / [0, height-1]，再做双线性插值；世界高度 =
// sample * desc.heightScale + desc.heightOffset。
//
// 源图可以包含 0..1 之外的有限值（例如 HDR/EXR 高度）；未触及的样本保持原值，
// 笔刷写入时才 clamp 到 [0,1]。这样进入编辑器不会静默改变已有地形形状。
class TerrainHeightmapEditor
{
public:
    explicit TerrainHeightmapEditor(const TerrainBuildPlan& plan);

    uint32_t Width() const noexcept { return width_; }
    uint32_t Height() const noexcept { return height_; }
    const TerrainAssetDesc& Desc() const noexcept { return desc_; }
    uint64_t Revision() const noexcept { return revision_; }

    // 与 TerrainHeightmap::SampleNormalized/SampleHeightCm 使用同一套 u/v 与
    // 双线性规则。非有限 UV 会被拒绝。
    float SampleNormalized(float u, float v) const;
    float SampleHeightCm(float u, float v) const;

    // 只读访问可编辑样本；行优先，长度为 width*height。持久化层可以直接消费
    // 它，但本轮不提供 PNG/EXR 写回接口。
    const std::vector<float>& NormalizedSamples() const noexcept
    {
        return samples_;
    }

    // Undo/redo 用的完整高度快照。Restore 只替换样本并推进 revision，
    // 不清理 dirty；调用方需要同步渲染时自行 MarkAllDirty。
    std::vector<float> CaptureHeightSamples() const { return samples_; }
    bool RestoreHeightSamples(std::vector<float> samples);

    TerrainSculptResult ApplyBrush(const TerrainSculptBrush& brush);

    // 自上次 ResetDirty 以来所有变化 texel 的并集。
    const TerrainPixelRect& DirtyRect() const noexcept { return dirtyRect_; }
    bool HasDirty() const noexcept { return allDirty_ || dirtyRect_.valid; }

    // dirtyRect 映射到 TR 的 gridResolution/chunkQuads。映射会把双线性
    // 影响范围各向外扩一个 texel，并包含共享边界两侧的 chunk，宁可多重建
    // 也不漏掉受边界顶点影响的数据。
    std::vector<TerrainDirtyChunk> DirtyChunks() const;
    uint32_t DirtyChunkCount() const;
    bool IsChunkDirty(uint32_t chunkX, uint32_t chunkZ) const;

    // 全量重建标记；不改变 revision 或样本，只影响后续 chunk 查询/重建。
    void MarkAllDirty();
    void ResetDirty();

    // 导出 CPU 可消费的 R32_FLOAT 快照（mip0、单层、行优先）。这是后续
    // GPU 上传或持久化 writer 的最小入口；本轮明确不写回原始 PNG/EXR，
    // 也不伪造已保存文件。
    HostImage ExportNormalizedHeightmap() const;

    // 用当前高度快照调用现有 TerrainMeshBuilder::BuildChunks，只生成 dirty
    // chunk 子集。网格公式、法线、UV、边界缝合全部复用运行时实现，不在此处复制。
    // 没有 dirty chunk 时返回空；lodLevel 语义同 TerrainMeshBuilder::BuildChunks。
    std::vector<TerrainMeshChunk> BuildDirtyChunkMeshes(
        uint32_t lodLevel = 0) const;

    // UE 风格的平滑衰减：normalizedDistance 是 d/radius。falloff 控制
    // 过渡带宽度，结果在 [0,1] 内单调不增。UI 预览也应使用这个函数，
    // 避免笔刷显示与实际写入不一致。
    static float BrushFalloff(float normalizedDistance, float falloff);

private:
    void UnionDirtyRect(const TerrainPixelRect& rect);

    TerrainAssetDesc desc_;
    std::string placementId_;
    uint32_t width_ = 0;
    uint32_t height_ = 0;
    std::vector<float> samples_;
    uint64_t revision_ = 0;
    TerrainPixelRect dirtyRect_;
    bool allDirty_ = false;
};

// 边界说明：本轮只实现 Sculpt。Layer weight paint 需要每层独立的权重
// override 数据、独立的 dirty tile 和 layer page 重建，不应塞进这里的
// 单一高度样本。后续应新增 TerrainLayerPaintEditor（或等价类型），让
// 高度与每层权重各自拥有 revision/dirty 生命周期，再由 UI 组合提交。

} // namespace VL
