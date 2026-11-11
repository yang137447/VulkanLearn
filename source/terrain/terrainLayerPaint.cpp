#include "terrain/terrainAssetTypes.h"

#include <algorithm>
#include <cmath>

namespace VL
{
namespace
{

struct PaintSample
{
    float weight = 0.0f;
    float opacity = 0.0f;
};

PaintSample SamplePaint(
    const TerrainLayerPaintData& paint,
    uint32_t layer,
    float u,
    float v)
{
    if (paint.Empty() || layer >= paint.layerCount)
        return {};
    const float x = std::clamp(u, 0.0f, 1.0f) * static_cast<float>(paint.width - 1);
    const float y = std::clamp(v, 0.0f, 1.0f) * static_cast<float>(paint.height - 1);
    const uint32_t x0 = static_cast<uint32_t>(std::floor(x));
    const uint32_t y0 = static_cast<uint32_t>(std::floor(y));
    const uint32_t x1 = std::min(x0 + 1, paint.width - 1);
    const uint32_t y1 = std::min(y0 + 1, paint.height - 1);
    const float fx = x - static_cast<float>(x0);
    const float fy = y - static_cast<float>(y0);
    const size_t layerBase =
        static_cast<size_t>(layer) * paint.width * paint.height;
    auto at = [&](uint32_t px, uint32_t py, float& weight, float& opacity)
    {
        const size_t index = layerBase + static_cast<size_t>(py) * paint.width + px;
        weight = paint.weights[index];
        opacity = paint.opacity[index] / 255.0f;
    };
    float w00 = 0.0f, w10 = 0.0f, w01 = 0.0f, w11 = 0.0f;
    float a00 = 0.0f, a10 = 0.0f, a01 = 0.0f, a11 = 0.0f;
    at(x0, y0, w00, a00);
    at(x1, y0, w10, a10);
    at(x0, y1, w01, a01);
    at(x1, y1, w11, a11);
    PaintSample result;
    result.weight =
        (w00 * (1.0f - fx) + w10 * fx) * (1.0f - fy) +
        (w01 * (1.0f - fx) + w11 * fx) * fy;
    result.opacity =
        (a00 * (1.0f - fx) + a10 * fx) * (1.0f - fy) +
        (a01 * (1.0f - fx) + a11 * fx) * fy;
    return result;
}

} // namespace

float TerrainLayerPaintData::Blend(
    uint32_t layer,
    float u,
    float v,
    float base) const
{
    if (Empty() || layer >= layerCount || !std::isfinite(base))
        return base;
    const PaintSample sample = SamplePaint(*this, layer, u, v);
    return base * (1.0f - sample.opacity) + sample.weight * sample.opacity;
}

bool TerrainLayerPaintData::HasPaint(
    uint32_t layer,
    float u,
    float v) const
{
    if (Empty() || layer >= layerCount)
        return false;
    return SamplePaint(*this, layer, u, v).opacity > (1.0f / 255.0f);
}

} // namespace VL
