#pragma once

#include <cstdint>
#include <deque>
#include <memory>
#include <string>

#include <Eigen/Dense>

#include "terrain/terrainAssetTypes.h"
#include "terrain/terrainBrushShape.h"
#include "terrain/terrainCollisionTypes.h"

namespace VL::Editor
{

struct TerrainPaintBrush
{
    uint32_t layerIndex = 0;
    TerrainBrushShape shape = TerrainBrushShape::Circle;
    Eigen::Vector2f centerUv = Eigen::Vector2f::Zero();
    float radiusUv = 0.0f;
    // Paint 的 Photoshop 语义：strength 仅作为兼容倍率，flow 控制每次落笔的
    // 推进速度，opacity 是本笔画不可超过的覆盖率上限。noise 以逐 texel 的
    // 稳定噪声调制每次推进量，noiseSeed 固定同一笔画的 spray 分布。
    float strength = 1.0f;
    float falloff = 1.0f;
    float opacity = 1.0f;
    float flow = 1.0f;
    float noise = 0.0f;
    uint32_t noiseSeed = 0;
    bool erase = false;
};

// 编辑器 layer weight paint 的 CPU 会话。覆盖层随当前 World 的
// TerrainPageManager 生效；只有 Save Paint 才会把它写到 TR 的 paintPath
// 指向的 .vlpaint 文件。未保存的编辑在 World 重载后仍会丢失。
class TerrainPaintSession
{
public:
    static bool Create(
        const TerrainCollisionInstance& instance,
        TerrainPaintSession& out,
        std::string& error);

    const std::string& PlacementId() const noexcept { return placementId_; }
    const std::string& PaintPath() const noexcept { return paintPath_; }
    uint32_t LayerCount() const noexcept;
    uint64_t Revision() const noexcept { return revision_; }
    bool IsStrokeActive() const noexcept { return strokeActive_; }

    bool BeginStroke();
    bool ApplyBrush(const TerrainPaintBrush& brush);
    bool EndStroke();
    void CancelStroke();

    bool CanUndo() const noexcept;
    bool CanRedo() const noexcept;
    bool Undo();
    bool Redo();

    std::shared_ptr<const TerrainLayerPaintData> PaintData() const;

private:
    void MarkChanged();
    void Restore(TerrainLayerPaintData data);

    std::string placementId_;
    std::string paintPath_;
    uint32_t layerCount_ = 0;
    std::shared_ptr<TerrainLayerPaintData> paint_;
    TerrainLayerPaintData strokeStart_;
    bool strokeActive_ = false;
    bool strokeChanged_ = false;
    uint64_t revision_ = 0;
    std::deque<TerrainLayerPaintData> undo_;
    std::deque<TerrainLayerPaintData> redo_;
};

} // namespace VL::Editor
