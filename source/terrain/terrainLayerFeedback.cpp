#include "terrain/terrainLayerFeedback.h"

#include <algorithm>
#include <cmath>
#include <stdexcept>
#include <vector>

namespace VL
{
namespace
{

constexpr uint32_t LayerCount = 256;
constexpr uint32_t MipCount = TerrainLayerFeedbackMaxMip + 1;
constexpr uint32_t PageCount = 256;
constexpr uint32_t AggregateSlotCount = LayerCount * MipCount * PageCount;

uint32_t AggregateIndex(
    uint32_t layerId,
    uint32_t mip,
    uint32_t pageIndex)
{
    return ((layerId * MipCount + mip) * PageCount) + pageIndex;
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
    // 65,536 个 texel 每帧都做 std::map 查找会产生数十毫秒 CPU 开销。
    // 这里用一次分配的 flat count 表；只扫描本帧首次出现的 key，并在
    // 析构时把用过的槽位清零，下一次调用无需整表 memset。
    static thread_local std::vector<uint32_t> counts(AggregateSlotCount, 0);
    static thread_local std::vector<uint32_t> usedIndices;
    usedIndices.clear();
    struct ScratchReset
    {
        std::vector<uint32_t>& counts;
        std::vector<uint32_t>& usedIndices;
        ~ScratchReset()
        {
            for (uint32_t index : usedIndices)
                counts[index] = 0;
            usedIndices.clear();
        }
    } scratchReset{counts, usedIndices};

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
        // 编码只有 3 bit mip，但合法范围是 0..4；越界值会让 flat 索引
        // 落到相邻 layer 槽位，必须在聚合前拒绝。
        if (texel.mip > TerrainLayerFeedbackMaxMip)
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

        const uint32_t aggregateIndex =
            AggregateIndex(texel.layerId, texel.mip, texel.pageIndex);
        uint32_t& sampleCount = counts[aggregateIndex];
        if (sampleCount == 0)
            usedIndices.push_back(aggregateIndex);
        ++sampleCount;
        maxSampleCount = std::max(maxSampleCount, sampleCount);
    }

    summary.distinctPageCount = static_cast<uint32_t>(usedIndices.size());
    summary.pages.reserve(usedIndices.size());
    for (uint32_t aggregateIndex : usedIndices)
    {
        const uint32_t pageIndex = aggregateIndex % PageCount;
        const uint32_t layerMip = aggregateIndex / PageCount;
        const uint32_t mip = layerMip % MipCount;
        const uint32_t layerId = layerMip / MipCount;
        TerrainLayerFeedbackRecord record;
        record.pageIndex = pageIndex;
        record.mip = mip;
        record.layerId = layerId;
        record.sampleCount = counts[aggregateIndex];
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
