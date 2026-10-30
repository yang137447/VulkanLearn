#include "terrain/terrainLayerFeedback.h"

#include <algorithm>
#include <cmath>
#include <map>
#include <stdexcept>

namespace VL
{
namespace
{

// (layerId, mip, pageIndex) 打包成一个 32 位键，便于 std::map 聚合去重。
// layerId 8 bit、mip 3 bit、pageIndex 8 bit，总 19 bit，远小于 32。
uint32_t PackPageKey(uint32_t layerId, uint32_t mip, uint32_t pageIndex)
{
    return (layerId << 11) | ((mip & TerrainLayerFeedbackMipMask) << 8) |
        (pageIndex & 0xFFu);
}

} // namespace

TerrainLayerFeedbackTexel DecodeTerrainLayerFeedbackTexel(
    const uint8_t* texel)
{
    TerrainLayerFeedbackTexel result;
    if (texel == nullptr)
        return result;
    result.layerId = texel[0];
    result.pageIndex = texel[1];
    result.mip = static_cast<uint32_t>(texel[2]) & TerrainLayerFeedbackMipMask;
    result.valid = (texel[2] & TerrainLayerFeedbackValidBit) != 0;
    result.frameTag = texel[3];
    return result;
}

void EncodeTerrainLayerFeedbackTexel(
    const TerrainLayerFeedbackTexel& texel,
    uint8_t* outTexel)
{
    if (outTexel == nullptr)
        throw std::invalid_argument(
            "Terrain layer feedback encode requires an output texel");
    if (texel.layerId > 255 || texel.pageIndex > TerrainLayerFeedbackMaxPageIndex ||
        texel.mip > TerrainLayerFeedbackMaxMip)
    {
        throw std::out_of_range(
            "Terrain layer feedback texel field exceeds the 8/8/3-bit encoding");
    }
    outTexel[0] = static_cast<uint8_t>(texel.layerId);
    outTexel[1] = static_cast<uint8_t>(texel.pageIndex);
    uint32_t packedMip = texel.mip;
    if (texel.valid)
        packedMip |= TerrainLayerFeedbackValidBit;
    outTexel[2] = static_cast<uint8_t>(packedMip);
    outTexel[3] = texel.frameTag;
}

TerrainLayerFeedbackFrameSummary SummarizeTerrainLayerFeedback(
    const uint8_t* texels,
    size_t byteCount,
    uint8_t expectedFrameTag)
{
    if (texels == nullptr)
        throw std::invalid_argument(
            "Terrain layer feedback summary requires texel data");
    if (byteCount < TerrainLayerFeedbackImageBytes ||
        byteCount % TerrainLayerFeedbackTexelBytes != 0)
    {
        throw std::invalid_argument(
            "Terrain layer feedback summary requires whole RGBA8 texels");
    }

    TerrainLayerFeedbackFrameSummary summary;
    summary.frameTag = expectedFrameTag;

    const size_t texelCount = byteCount / TerrainLayerFeedbackTexelBytes;
    std::map<uint32_t, uint32_t> counts;
    uint32_t maxSampleCount = 0;
    for (size_t texelIndex = 0; texelIndex < texelCount; ++texelIndex)
    {
        const TerrainLayerFeedbackTexel texel =
            DecodeTerrainLayerFeedbackTexel(
                texels + texelIndex * TerrainLayerFeedbackTexelBytes);
        if (!texel.valid)
        {
            ++summary.invalidTexelCount;
            continue;
        }
        // frameTag 必须与本次回读的提交 epoch 匹配，否则说明 ring 槽错位。
        if (texel.frameTag != expectedFrameTag)
        {
            ++summary.staleTexelCount;
            continue;
        }
        ++summary.validTexelCount;
        const size_t slot = texelIndex / TerrainLayerFeedbackTexelsPerImage;
        if (slot < TerrainLayerFeedbackSlotCount)
            ++summary.slotSampleCount[slot];

        uint32_t& count = counts[PackPageKey(
            texel.layerId, texel.mip, texel.pageIndex)];
        ++count;
        maxSampleCount = std::max(maxSampleCount, count);
    }

    summary.distinctPageCount = static_cast<uint32_t>(counts.size());
    summary.pages.reserve(counts.size());
    for (const auto& entry : counts)
    {
        TerrainLayerFeedbackRecord record;
        record.pageIndex = entry.first & 0xFFu;
        record.mip = (entry.first >> 8) & TerrainLayerFeedbackMipMask;
        record.layerId = entry.first >> 11;
        record.sampleCount = entry.second;
        // 相对覆盖率：让近景/大面积层排到请求队列前面，同时压进 1..255，
        // 与页表 A 通道（priority）的既有取值范围一致。
        const float ratio =
            maxSampleCount == 0
                ? 0.0f
                : static_cast<float>(record.sampleCount) /
                    static_cast<float>(maxSampleCount);
        record.priority = static_cast<uint32_t>(std::lround(
            std::clamp(255.0f * std::sqrt(std::clamp(ratio, 0.0f, 1.0f)),
                1.0f,
                255.0f)));
        summary.pages.push_back(record);
    }
    std::sort(
        summary.pages.begin(),
        summary.pages.end(),
        [](const TerrainLayerFeedbackRecord& lhs,
           const TerrainLayerFeedbackRecord& rhs)
        {
            if (lhs.priority != rhs.priority)
                return lhs.priority > rhs.priority;
            return lhs < rhs;
        });
    return summary;
}

} // namespace VL
