#include "editor/terrain/terrainPaintSession.h"

#include <algorithm>
#include <cmath>
#include <stdexcept>

namespace VL::Editor
{
namespace
{

// 覆盖层是 layer-major 的完整快照；限制历史条数并让高 layer 数退到更低
// 分辨率，避免 256 层地形一次 undo 就占几十 MB。
constexpr size_t PaintUndoLimit = 4;

bool Finite(const Eigen::Vector2f& value)
{
    return std::isfinite(value.x()) && std::isfinite(value.y());
}

float SmoothStep(float value)
{
    const float x = std::clamp(value, 0.0f, 1.0f);
    return x * x * (3.0f - 2.0f * x);
}

float Falloff(float distance, float falloff)
{
    const float d = std::clamp(distance, 0.0f, 1.0f);
    const float edge = std::clamp(falloff, 0.0f, 1.0f);
    if (d >= 1.0f) return 0.0f;
    if (edge <= 0.0f) return 1.0f;
    const float inner = 1.0f - edge;
    if (d <= inner) return 1.0f;
    return 1.0f - SmoothStep((d - inner) / edge);
}

uint32_t PaintResolution(uint32_t layerCount)
{
    if (layerCount <= 32) return 256u;
    if (layerCount <= 64) return 128u;
    return 64u;
}

// 与 editor/brush/brushSampler.cpp 的 StableNoise01 保持同一 32-bit 混合公式。
// terrain_tests 不链接 BrushSampler，这里保留局部实现，避免 terrain 会话反向
// 依赖编辑器笔刷模块；两处分布若调整必须同步。
uint32_t Mix32(uint32_t value)
{
    value ^= value >> 16;
    value *= 0x7feb352du;
    value ^= value >> 15;
    value *= 0x846ca68bu;
    value ^= value >> 16;
    return value;
}

float StableNoise01(uint32_t x, uint32_t y, uint32_t seed)
{
    const uint32_t hash = Mix32(
        x * 0x8da6b343u ^ y * 0xd8163841u ^ seed * 0xcb1ab31fu);
    return static_cast<float>(hash) * (1.0f / 4294967295.0f);
}

} // namespace

bool TerrainPaintSession::Create(
    const TerrainCollisionInstance& instance,
    TerrainPaintSession& out,
    std::string& error)
{
    error.clear();
    if (instance.placementId.rfind("terrain|", 0) != 0 ||
        instance.placementId.size() <= 8)
    {
        error = "Terrain paint requires a terrain|<name> placement id";
        return false;
    }
    if (instance.plan.desc.layers.empty())
    {
        error = "Terrain paint requires a layered terrain";
        return false;
    }
    if (instance.plan.desc.layers.size() > 256)
    {
        error = "Terrain paint supports at most 256 layers";
        return false;
    }

    TerrainPaintSession candidate;
    candidate.placementId_ = instance.placementId;
    candidate.paintPath_ = instance.plan.desc.paintPath;
    candidate.layerCount_ = static_cast<uint32_t>(instance.plan.desc.layers.size());
    const std::shared_ptr<const TerrainLayerPaintData> loaded =
        instance.plan.layerPaint;
    if (loaded && !loaded->Empty())
    {
        // 已持久化快照是会话的起点，不能再用空白层覆盖；尺寸与层数必须与
        // TR 完全一致，否则笔画索引会写到错误的 layer/texel。
        if (loaded->width != loaded->height ||
            loaded->layerCount != candidate.layerCount_)
        {
            error =
                "Persisted terrain paint dimensions do not match the terrain layer table: " +
                instance.plan.desc.paintPath;
            return false;
        }
        candidate.paint_ =
            std::make_shared<TerrainLayerPaintData>(*loaded);
    }
    else
    {
        const uint32_t resolution = PaintResolution(candidate.layerCount_);
        candidate.paint_ = std::make_shared<TerrainLayerPaintData>();
        candidate.paint_->width = resolution;
        candidate.paint_->height = resolution;
        candidate.paint_->layerCount = candidate.layerCount_;
        candidate.paint_->weights.assign(
            static_cast<size_t>(resolution) * resolution * candidate.layerCount_, 0.0f);
        candidate.paint_->opacity.assign(
            static_cast<size_t>(resolution) * resolution * candidate.layerCount_, 0u);
    }
    out = std::move(candidate);
    return true;
}

uint32_t TerrainPaintSession::LayerCount() const noexcept
{
    return layerCount_;
}

void TerrainPaintSession::MarkChanged()
{
    ++revision_;
    strokeChanged_ = true;
}

void TerrainPaintSession::Restore(TerrainLayerPaintData data)
{
    if (data.width != paint_->width || data.height != paint_->height ||
        data.layerCount != paint_->layerCount)
        throw std::invalid_argument("Terrain paint restore dimensions changed");
    *paint_ = std::move(data);
    ++revision_;
}

bool TerrainPaintSession::BeginStroke()
{
    if (!paint_ || strokeActive_)
        return false;
    strokeStart_ = *paint_;
    strokeActive_ = true;
    strokeChanged_ = false;
    return true;
}

bool TerrainPaintSession::ApplyBrush(const TerrainPaintBrush& brush)
{
    if (!paint_ || !strokeActive_)
        return false;
    if (!Finite(brush.centerUv) ||
        brush.centerUv.x() < 0.0f || brush.centerUv.x() > 1.0f ||
        brush.centerUv.y() < 0.0f || brush.centerUv.y() > 1.0f ||
        !std::isfinite(brush.radiusUv) || brush.radiusUv <= 0.0f ||
        !std::isfinite(brush.strength) || brush.strength <= 0.0f ||
        !std::isfinite(brush.falloff) ||
        brush.falloff < 0.0f || brush.falloff > 1.0f ||
        !std::isfinite(brush.opacity) ||
        brush.opacity < 0.0f || brush.opacity > 1.0f ||
        !std::isfinite(brush.flow) ||
        brush.flow < 0.0f || brush.flow > 1.0f ||
        !std::isfinite(brush.noise) ||
        brush.noise < 0.0f || brush.noise > 1.0f ||
        brush.layerIndex >= layerCount_)
    {
        return false;
    }

    const uint32_t width = paint_->width;
    const uint32_t height = paint_->height;
    const float centerX = brush.centerUv.x() * static_cast<float>(width - 1);
    const float centerY = brush.centerUv.y() * static_cast<float>(height - 1);
    const float radiusX = brush.radiusUv * static_cast<float>(width - 1);
    const float radiusY = brush.radiusUv * static_cast<float>(height - 1);
    const int minX = std::max(0, static_cast<int>(std::floor(centerX - radiusX)));
    const int maxX = std::min(static_cast<int>(width) - 1,
        static_cast<int>(std::ceil(centerX + radiusX)));
    const int minY = std::max(0, static_cast<int>(std::floor(centerY - radiusY)));
    const int maxY = std::min(static_cast<int>(height) - 1,
        static_cast<int>(std::ceil(centerY + radiusY)));
    if (minX > maxX || minY > maxY)
        return false;

    const size_t layerBase =
        static_cast<size_t>(brush.layerIndex) * width * height;
    bool changed = false;
    const float opacity = std::clamp(brush.opacity, 0.0f, 1.0f);
    const float flow = std::clamp(brush.flow, 0.0f, 1.0f);
    const float noise = std::clamp(brush.noise, 0.0f, 1.0f);
    const float strength = std::clamp(brush.strength, 0.0f, 1.0f);
    for (int y = minY; y <= maxY; ++y)
    {
        for (int x = minX; x <= maxX; ++x)
        {
            const float dx = radiusX > 0.0f
                ? (static_cast<float>(x) - centerX) / radiusX : 0.0f;
            const float dy = radiusY > 0.0f
                ? (static_cast<float>(y) - centerY) / radiusY : 0.0f;
            const float normalizedDistance =
                brush.shape == TerrainBrushShape::Square
                    ? std::max(std::abs(dx), std::abs(dy))
                    : std::sqrt(dx * dx + dy * dy);
            const float influence =
                Falloff(normalizedDistance, brush.falloff) *
                strength * flow;
            if (influence <= 0.0f) continue;
            // Spray 只调制本次推进量，不改变 texel 的最终 opacity 上限；同 seed
            // 下同一 texel 每帧得到相同 mask，使笔刷结果可重复。
            const float noiseMask = noise > 0.0f
                ? 1.0f - noise +
                    noise * StableNoise01(
                        static_cast<uint32_t>(x),
                        static_cast<uint32_t>(y),
                        brush.noiseSeed)
                : 1.0f;
            const float step = influence * noiseMask;
            if (step <= 0.0f) continue;
            const size_t index = layerBase + static_cast<size_t>(y) * width + x;
            if (brush.erase)
            {
                const float next = paint_->opacity[index] / 255.0f *
                    (1.0f - step);
                const uint8_t value = static_cast<uint8_t>(std::clamp(
                    static_cast<int>(std::round(next * 255.0f)), 0, 255));
                if (value == paint_->opacity[index]) continue;
                paint_->opacity[index] = value;
                changed = true;
            }
            else
            {
                // opacity 是整笔 stroke 的覆盖上限；已有更高覆盖（例如旧文件或
                // 其他层操作）只能保持，不能被较低的画笔上限反向擦除。
                const float currentOpacity =
                    paint_->opacity[index] / 255.0f;
                const float targetOpacity =
                    std::max(currentOpacity, opacity);
                const float nextOpacity = currentOpacity +
                    (targetOpacity - currentOpacity) * step;
                const float nextWeight = paint_->weights[index] +
                    (1.0f - paint_->weights[index]) * step;
                const uint8_t value = static_cast<uint8_t>(std::clamp(
                    static_cast<int>(std::round(nextOpacity * 255.0f)), 0, 255));
                if (value == paint_->opacity[index] &&
                    std::abs(nextWeight - paint_->weights[index]) < 1.0e-6f)
                    continue;
                paint_->opacity[index] = value;
                paint_->weights[index] = std::clamp(nextWeight, 0.0f, 1.0f);
                changed = true;
            }
        }
    }
    if (!changed)
        return false;
    MarkChanged();
    return true;
}

bool TerrainPaintSession::EndStroke()
{
    if (!paint_ || !strokeActive_)
        return false;
    strokeActive_ = false;
    if (!strokeChanged_)
    {
        strokeStart_ = {};
        return false;
    }
    undo_.push_back(std::move(strokeStart_));
    strokeStart_ = {};
    while (undo_.size() > PaintUndoLimit)
        undo_.pop_front();
    redo_.clear();
    strokeChanged_ = false;
    return true;
}

void TerrainPaintSession::CancelStroke()
{
    if (!paint_ || !strokeActive_)
        return;
    strokeActive_ = false;
    if (strokeChanged_)
        Restore(std::move(strokeStart_));
    strokeStart_ = {};
    strokeChanged_ = false;
}

bool TerrainPaintSession::CanUndo() const noexcept
{
    return paint_ && !strokeActive_ && !undo_.empty();
}

bool TerrainPaintSession::CanRedo() const noexcept
{
    return paint_ && !strokeActive_ && !redo_.empty();
}

bool TerrainPaintSession::Undo()
{
    if (!CanUndo()) return false;
    TerrainLayerPaintData previous = std::move(undo_.back());
    undo_.pop_back();
    redo_.push_back(*paint_);
    while (redo_.size() > PaintUndoLimit)
        redo_.pop_front();
    Restore(std::move(previous));
    return true;
}

bool TerrainPaintSession::Redo()
{
    if (!CanRedo()) return false;
    TerrainLayerPaintData next = std::move(redo_.back());
    redo_.pop_back();
    undo_.push_back(*paint_);
    while (undo_.size() > PaintUndoLimit)
        undo_.pop_front();
    Restore(std::move(next));
    return true;
}

std::shared_ptr<const TerrainLayerPaintData> TerrainPaintSession::PaintData() const
{
    return paint_;
}

} // namespace VL::Editor
