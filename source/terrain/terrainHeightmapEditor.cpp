#include "terrain/terrainHeightmapEditor.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>
#include <memory>
#include <stdexcept>

namespace VL
{
namespace
{

float SmoothStep(float t)
{
    const float clamped = std::clamp(t, 0.0f, 1.0f);
    return clamped * clamped * (3.0f - 2.0f * clamped);
}

bool IsFinite(const Eigen::Vector2f& value)
{
    return std::isfinite(value.x()) && std::isfinite(value.y());
}

// 编辑器需要逐 texel 取得源样本，而 TerrainHeightmap 只暴露任意 UV 的
// 双线性采样。这里必须复制它的格式解码规则，才能保证可编辑副本的每个
// texel 与原图像素一一对应；新增 HostImage 高度格式时，两处必须同步。
float ReadSourceTexel(const HostImage& image, uint32_t x, uint32_t y)
{
    const size_t rowStride = image.rowStrideBytes != 0
        ? image.rowStrideBytes
        : static_cast<size_t>(image.width) * image.GetPixelSize();
    const uint8_t* row =
        image.data.data() + static_cast<size_t>(y) * rowStride;
    switch (image.format)
    {
    case HostImage::PixelFormat::R8_UNORM:
    case HostImage::PixelFormat::R8_SRGB:
        return static_cast<float>(row[x]) / 255.0f;
    case HostImage::PixelFormat::RG8_UNORM:
    case HostImage::PixelFormat::RG8_SRGB:
        return static_cast<float>(row[static_cast<size_t>(x) * 2]) / 255.0f;
    case HostImage::PixelFormat::RGBA8_UNORM:
    case HostImage::PixelFormat::RGBA8_SRGB:
        return static_cast<float>(row[static_cast<size_t>(x) * 4]) / 255.0f;
    case HostImage::PixelFormat::R32_FLOAT:
        return reinterpret_cast<const float*>(row)[x];
    case HostImage::PixelFormat::RGBA32_FLOAT:
        return reinterpret_cast<const float*>(row)[static_cast<size_t>(x) * 4];
    default:
        throw std::runtime_error(
            "Terrain heightmap pixel format is not supported; use 16-bit "
            "grayscale PNG, RGBA EXR/HDR float, or 8-bit grayscale/RGBA");
    }
}

uint32_t CandidateMin(float center, float radius, uint32_t count)
{
    const double denominator = static_cast<double>(count - 1);
    const double minUv = std::max(
        0.0,
        static_cast<double>(center) - static_cast<double>(radius));
    return static_cast<uint32_t>(std::floor(minUv * denominator));
}

uint32_t CandidateMax(float center, float radius, uint32_t count)
{
    const double denominator = static_cast<double>(count - 1);
    const double maxUv = std::min(
        1.0,
        static_cast<double>(center) + static_cast<double>(radius));
    const double candidate =
        std::ceil(maxUv * denominator);
    return static_cast<uint32_t>(std::min(
        candidate,
        static_cast<double>(count - 1)));
}

} // namespace

TerrainHeightmapEditor::TerrainHeightmapEditor(const TerrainBuildPlan& plan)
{
    if (!plan.heightmap)
        throw std::runtime_error(
            "Terrain heightmap editor requires a decoded heightmap");
    const HostImage& image = *plan.heightmap;
    if (image.width < 2 || image.height < 2 || image.data.empty())
        throw std::runtime_error(
            "Terrain heightmap editor requires at least 2x2 pixels");
    if (image.mipLevels == 0 || image.arrayLayers == 0)
        throw std::runtime_error(
            "Terrain heightmap editor requires a valid HostImage layout");
    if (HostImage::IsBlockCompressed(image.format))
        throw std::runtime_error(
            "Terrain heightmap editor does not support block-compressed heightmaps");
    if (plan.desc.gridResolution == 0 || plan.desc.chunkQuads == 0 ||
        plan.desc.gridResolution % plan.desc.chunkQuads != 0)
    {
        throw std::runtime_error(
            "Terrain heightmap editor requires gridResolution divisible by chunkQuads");
    }

    desc_ = plan.desc;
    placementId_ = plan.placementId;
    width_ = image.width;
    height_ = image.height;

    const size_t pixelSize = image.GetPixelSize();
    const size_t compactStride =
        static_cast<size_t>(width_) * pixelSize;
    const size_t rowStride = image.rowStrideBytes != 0
        ? image.rowStrideBytes
        : compactStride;
    if (rowStride < compactStride)
        throw std::runtime_error(
            "Terrain heightmap editor received an invalid row stride");
    if (rowStride != 0 && static_cast<size_t>(height_) >
        image.data.size() / rowStride)
    {
        throw std::runtime_error(
            "Terrain heightmap editor received truncated pixel data");
    }

    samples_.resize(static_cast<size_t>(width_) * height_);
    for (uint32_t y = 0; y < height_; ++y)
    {
        for (uint32_t x = 0; x < width_; ++x)
        {
            const float sample = ReadSourceTexel(image, x, y);
            if (!std::isfinite(sample))
                throw std::runtime_error(
                    "Terrain heightmap contains non-finite samples");
            samples_[static_cast<size_t>(y) * width_ + x] = sample;
        }
    }
}

float TerrainHeightmapEditor::SampleNormalized(float u, float v) const
{
    if (!std::isfinite(u) || !std::isfinite(v))
        throw std::invalid_argument("Terrain heightmap sample UV must be finite");

    // 与 TerrainHeightmap::SampleNormalized 完全相同的端点映射和双线性规则。
    const float clampedU = std::clamp(u, 0.0f, 1.0f);
    const float clampedV = std::clamp(v, 0.0f, 1.0f);
    const float x = clampedU * static_cast<float>(width_ - 1);
    const float y = clampedV * static_cast<float>(height_ - 1);
    const uint32_t x0 = static_cast<uint32_t>(std::floor(x));
    const uint32_t y0 = static_cast<uint32_t>(std::floor(y));
    const uint32_t x1 = std::min(x0 + 1, width_ - 1);
    const uint32_t y1 = std::min(y0 + 1, height_ - 1);
    const float fx = x - static_cast<float>(x0);
    const float fy = y - static_cast<float>(y0);

    const float h00 = samples_[static_cast<size_t>(y0) * width_ + x0];
    const float h10 = samples_[static_cast<size_t>(y0) * width_ + x1];
    const float h01 = samples_[static_cast<size_t>(y1) * width_ + x0];
    const float h11 = samples_[static_cast<size_t>(y1) * width_ + x1];
    const float top = h00 + (h10 - h00) * fx;
    const float bottom = h01 + (h11 - h01) * fx;
    return top + (bottom - top) * fy;
}

float TerrainHeightmapEditor::SampleHeightCm(float u, float v) const
{
    return SampleNormalized(u, v) * desc_.heightScale + desc_.heightOffset;
}

bool TerrainHeightmapEditor::RestoreHeightSamples(std::vector<float> samples)
{
    if (samples.size() != samples_.size())
        throw std::invalid_argument(
            "Terrain heightmap restore requires matching dimensions");
    for (float sample : samples)
    {
        if (!std::isfinite(sample))
            throw std::invalid_argument(
                "Terrain heightmap restore requires finite samples");
    }
    if (samples == samples_)
        return false;
    samples_ = std::move(samples);
    ++revision_;
    return true;
}

float TerrainHeightmapEditor::BrushFalloff(
    float normalizedDistance,
    float falloff)
{
    if (!std::isfinite(normalizedDistance) || !std::isfinite(falloff))
        throw std::invalid_argument("Terrain brush falloff inputs must be finite");
    const float distance = std::clamp(normalizedDistance, 0.0f, 1.0f);
    const float edgeWidth = std::clamp(falloff, 0.0f, 1.0f);
    if (distance >= 1.0f)
        return 0.0f;
    if (edgeWidth <= 0.0f)
        return 1.0f;
    const float inner = 1.0f - edgeWidth;
    if (distance <= inner)
        return 1.0f;
    return 1.0f - SmoothStep((distance - inner) / edgeWidth);
}

TerrainSculptResult TerrainHeightmapEditor::ApplyBrush(
    const TerrainSculptBrush& brush)
{
    if (!IsFinite(brush.centerUv))
        throw std::invalid_argument("Terrain brush center UV must be finite");
    if (brush.centerUv.x() < 0.0f || brush.centerUv.x() > 1.0f ||
        brush.centerUv.y() < 0.0f || brush.centerUv.y() > 1.0f)
    {
        throw std::invalid_argument("Terrain brush center UV must be inside [0,1]");
    }
    if (!std::isfinite(brush.radiusUv) || brush.radiusUv <= 0.0f)
        throw std::invalid_argument("Terrain brush radius must be positive and finite");
    if (!std::isfinite(brush.strength) || brush.strength <= 0.0f)
        throw std::invalid_argument("Terrain brush strength must be positive and finite");
    if (!std::isfinite(brush.falloff) ||
        brush.falloff < 0.0f || brush.falloff > 1.0f)
    {
        throw std::invalid_argument("Terrain brush falloff must be inside [0,1]");
    }
    if (brush.mode != TerrainSculptMode::Raise &&
        brush.mode != TerrainSculptMode::Lower &&
        brush.mode != TerrainSculptMode::Smooth)
    {
        throw std::invalid_argument("Terrain brush mode is invalid");
    }

    const uint32_t minX = CandidateMin(brush.centerUv.x(), brush.radiusUv, width_);
    const uint32_t maxX = CandidateMax(brush.centerUv.x(), brush.radiusUv, width_);
    const uint32_t minY = CandidateMin(brush.centerUv.y(), brush.radiusUv, height_);
    const uint32_t maxY = CandidateMax(brush.centerUv.y(), brush.radiusUv, height_);

    // Smooth 必须从笔刷开始前的快照取邻域均值，避免同一个笔刷内的遍历顺序
    // 影响结果；Raise/Lower 则直接读当前值。
    std::vector<float> smoothSource;
    if (brush.mode == TerrainSculptMode::Smooth)
        smoothSource = samples_;

    TerrainSculptResult result;
    result.revision = revision_;
    float changedMin = std::numeric_limits<float>::infinity();
    float changedMax = -std::numeric_limits<float>::infinity();
    bool changed = false;

    for (uint32_t y = minY; y <= maxY; ++y)
    {
        const float v = static_cast<float>(y) /
            static_cast<float>(height_ - 1);
        const float dv = (v - brush.centerUv.y()) / brush.radiusUv;
        for (uint32_t x = minX; x <= maxX; ++x)
        {
            const float u = static_cast<float>(x) /
                static_cast<float>(width_ - 1);
            const float du = (u - brush.centerUv.x()) / brush.radiusUv;
            const float normalizedDistance =
                brush.shape == TerrainBrushShape::Square
                    ? std::max(std::abs(du), std::abs(dv))
                    : std::sqrt(du * du + dv * dv);
            if (normalizedDistance > 1.0f)
                continue;
            const float influence =
                BrushFalloff(normalizedDistance, brush.falloff);
            if (influence <= 0.0f)
                continue;

            const size_t index =
                static_cast<size_t>(y) * width_ + x;
            const float oldValue = samples_[index];
            float newValue = oldValue;
            if (brush.mode == TerrainSculptMode::Raise)
            {
                newValue = oldValue + brush.strength * influence;
            }
            else if (brush.mode == TerrainSculptMode::Lower)
            {
                newValue = oldValue - brush.strength * influence;
            }
            else
            {
                // 3x3 盒式邻域平均；边界截断。alpha 限制在 [0,1]，
                // strength>1 只表示一步完全收敛到邻域均值。
                float sum = 0.0f;
                uint32_t count = 0;
                const uint32_t startX = x > 0 ? x - 1 : 0;
                const uint32_t endX = std::min(x + 1, width_ - 1);
                const uint32_t startY = y > 0 ? y - 1 : 0;
                const uint32_t endY = std::min(y + 1, height_ - 1);
                for (uint32_t sampleY = startY; sampleY <= endY; ++sampleY)
                {
                    for (uint32_t sampleX = startX; sampleX <= endX; ++sampleX)
                    {
                        sum += smoothSource[
                            static_cast<size_t>(sampleY) * width_ + sampleX];
                        ++count;
                    }
                }
                const float target = sum / static_cast<float>(count);
                const float alpha = std::clamp(
                    brush.strength * influence,
                    0.0f,
                    1.0f);
                newValue = oldValue + (target - oldValue) * alpha;
            }
            newValue = std::clamp(newValue, 0.0f, 1.0f);
            if (newValue == oldValue)
                continue;

            samples_[index] = newValue;
            changed = true;
            changedMin = std::min(changedMin, newValue);
            changedMax = std::max(changedMax, newValue);
            if (!result.dirtyRect.valid)
            {
                result.dirtyRect.valid = true;
                result.dirtyRect.minX = x;
                result.dirtyRect.maxX = x;
                result.dirtyRect.minY = y;
                result.dirtyRect.maxY = y;
            }
            else
            {
                result.dirtyRect.minX = std::min(result.dirtyRect.minX, x);
                result.dirtyRect.maxX = std::max(result.dirtyRect.maxX, x);
                result.dirtyRect.minY = std::min(result.dirtyRect.minY, y);
                result.dirtyRect.maxY = std::max(result.dirtyRect.maxY, y);
            }
        }
    }

    if (!changed)
        return result;

    ++revision_;
    result.applied = true;
    result.revision = revision_;
    result.changedMin = changedMin;
    result.changedMax = changedMax;
    UnionDirtyRect(result.dirtyRect);
    return result;
}

void TerrainHeightmapEditor::UnionDirtyRect(const TerrainPixelRect& rect)
{
    if (!rect.valid)
        return;
    if (!dirtyRect_.valid)
    {
        dirtyRect_ = rect;
        return;
    }
    dirtyRect_.minX = std::min(dirtyRect_.minX, rect.minX);
    dirtyRect_.minY = std::min(dirtyRect_.minY, rect.minY);
    dirtyRect_.maxX = std::max(dirtyRect_.maxX, rect.maxX);
    dirtyRect_.maxY = std::max(dirtyRect_.maxY, rect.maxY);
}

std::vector<TerrainDirtyChunk> TerrainHeightmapEditor::DirtyChunks() const
{
    const uint32_t chunksPerSide =
        desc_.gridResolution / desc_.chunkQuads;
    std::vector<TerrainDirtyChunk> chunks;
    if (allDirty_)
    {
        chunks.reserve(static_cast<size_t>(chunksPerSide) * chunksPerSide);
        for (uint32_t z = 0; z < chunksPerSide; ++z)
        {
            for (uint32_t x = 0; x < chunksPerSide; ++x)
                chunks.push_back({x, z});
        }
        return chunks;
    }
    if (!dirtyRect_.valid)
        return chunks;

    // texel 下标 k 对应 UV=k/(N-1)。双线性采样在 (k-1)/(N-1) 到
    // (k+1)/(N-1) 之间都会读取该 texel，因此脏区各向外扩一个 texel 步长。
    const double uStep = 1.0 / static_cast<double>(width_ - 1);
    const double vStep = 1.0 / static_cast<double>(height_ - 1);
    const double uMin = std::max(
        0.0,
        static_cast<double>(dirtyRect_.minX) * uStep - uStep);
    const double uMax = std::min(
        1.0,
        static_cast<double>(dirtyRect_.maxX) * uStep + uStep);
    const double vMin = std::max(
        0.0,
        static_cast<double>(dirtyRect_.minY) * vStep - vStep);
    const double vMax = std::min(
        1.0,
        static_cast<double>(dirtyRect_.maxY) * vStep + vStep);

    // 恰好落在 chunk 边界上的顶点同时属于左右两个 chunk。ceil-1 让
    // 下边界为整数时把前一个 chunk 也包含进来。
    const double scaledMinX = uMin * static_cast<double>(chunksPerSide);
    const double scaledMaxX = uMax * static_cast<double>(chunksPerSide);
    const double scaledMinZ = vMin * static_cast<double>(chunksPerSide);
    const double scaledMaxZ = vMax * static_cast<double>(chunksPerSide);
    const uint32_t firstX = static_cast<uint32_t>(std::max(
        0.0,
        std::ceil(scaledMinX) - 1.0));
    const uint32_t lastX = static_cast<uint32_t>(std::min(
        std::floor(scaledMaxX),
        static_cast<double>(chunksPerSide - 1)));
    const uint32_t firstZ = static_cast<uint32_t>(std::max(
        0.0,
        std::ceil(scaledMinZ) - 1.0));
    const uint32_t lastZ = static_cast<uint32_t>(std::min(
        std::floor(scaledMaxZ),
        static_cast<double>(chunksPerSide - 1)));

    chunks.reserve(
        static_cast<size_t>(lastX - firstX + 1) *
        static_cast<size_t>(lastZ - firstZ + 1));
    for (uint32_t z = firstZ; z <= lastZ; ++z)
    {
        for (uint32_t x = firstX; x <= lastX; ++x)
            chunks.push_back({x, z});
    }
    return chunks;
}

uint32_t TerrainHeightmapEditor::DirtyChunkCount() const
{
    return static_cast<uint32_t>(DirtyChunks().size());
}

bool TerrainHeightmapEditor::IsChunkDirty(
    uint32_t chunkX,
    uint32_t chunkZ) const
{
    for (const TerrainDirtyChunk& chunk : DirtyChunks())
    {
        if (chunk.x == chunkX && chunk.z == chunkZ)
            return true;
    }
    return false;
}

void TerrainHeightmapEditor::MarkAllDirty()
{
    allDirty_ = true;
    dirtyRect_.valid = true;
    dirtyRect_.minX = 0;
    dirtyRect_.minY = 0;
    dirtyRect_.maxX = width_ - 1;
    dirtyRect_.maxY = height_ - 1;
}

void TerrainHeightmapEditor::ResetDirty()
{
    allDirty_ = false;
    dirtyRect_ = TerrainPixelRect();
}

HostImage TerrainHeightmapEditor::ExportNormalizedHeightmap() const
{
    HostImage image;
    image.width = width_;
    image.height = height_;
    image.channels = 1;
    image.mipLevels = 1;
    image.arrayLayers = 1;
    image.format = HostImage::PixelFormat::R32_FLOAT;
    image.semantic = HostImage::TextureSemantic::Data;
    image.rowStrideBytes =
        static_cast<uint32_t>(width_ * sizeof(float));
    image.data.resize(
        static_cast<size_t>(image.rowStrideBytes) * height_);
    std::memcpy(
        image.data.data(),
        samples_.data(),
        samples_.size() * sizeof(float));
    return image;
}

std::vector<TerrainMeshChunk> TerrainHeightmapEditor::BuildDirtyChunkMeshes(
    uint32_t lodLevel) const
{
    const std::vector<TerrainDirtyChunk> dirtyChunks = DirtyChunks();
    if (dirtyChunks.empty())
        return {};

    TerrainBuildPlan editedPlan;
    editedPlan.desc = desc_;
    editedPlan.placementId = placementId_;
    editedPlan.heightmap = std::make_shared<const HostImage>(
        ExportNormalizedHeightmap());

    const uint32_t chunksPerSide =
        desc_.gridResolution / desc_.chunkQuads;
    TerrainChunkBuildFilter filter;
    filter.chunkMask.assign(
        static_cast<size_t>(chunksPerSide) * chunksPerSide,
        0);
    for (const TerrainDirtyChunk& chunk : dirtyChunks)
    {
        filter.chunkMask[
            static_cast<size_t>(chunk.z) * chunksPerSide + chunk.x] = 1;
    }
    return TerrainMeshBuilder::BuildChunks(
        editedPlan,
        lodLevel,
        &filter);
}

} // namespace VL
