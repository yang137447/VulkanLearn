#include "terrain/terrainFeedback.h"

#include <algorithm>
#include <stdexcept>
#include <unordered_map>

namespace VL
{
namespace
{

uint32_t PageKey(uint32_t pageX, uint32_t pageY)
{
    return (pageY << 8u) | (pageX & 0xFFu);
}

} // namespace

TerrainFeedbackTexel DecodeTerrainFeedbackTexel(const uint8_t* texel)
{
    if (texel == nullptr)
    {
        throw std::invalid_argument(
            "DecodeTerrainFeedbackTexel requires a non-null texel");
    }

    TerrainFeedbackTexel decoded;
    decoded.virtualPageX = texel[0] & TerrainFeedbackPageMask;
    decoded.miss = (texel[0] & TerrainFeedbackMissBit) != 0;
    decoded.valid = (texel[0] & TerrainFeedbackValidBit) != 0;
    decoded.virtualPageY = texel[1] & TerrainFeedbackPageMask;
    decoded.priority = texel[2];
    decoded.frameTag = texel[3];
    return decoded;
}

void EncodeTerrainFeedbackTexel(
    const TerrainFeedbackTexel& texel,
    uint8_t* outTexel)
{
    if (outTexel == nullptr)
    {
        throw std::invalid_argument(
            "EncodeTerrainFeedbackTexel requires a non-null destination");
    }

    uint8_t packedX = static_cast<uint8_t>(
        texel.virtualPageX & TerrainFeedbackPageMask);
    if (texel.miss)
        packedX |= TerrainFeedbackMissBit;
    if (texel.valid)
        packedX |= TerrainFeedbackValidBit;

    outTexel[0] = packedX;
    outTexel[1] = static_cast<uint8_t>(
        texel.virtualPageY & TerrainFeedbackPageMask);
    outTexel[2] = texel.priority;
    outTexel[3] = texel.frameTag;
}

uint8_t TerrainFeedbackFrameTagForEpoch(uint64_t epoch) noexcept
{
    return static_cast<uint8_t>((epoch % 255u) + 1u);
}

TerrainFeedbackFrameSummary SummarizeTerrainFeedback(
    const uint8_t* texels,
    size_t byteCount,
    uint8_t expectedFrameTag)
{
    if (byteCount % TerrainFeedbackTexelBytes != 0)
    {
        throw std::invalid_argument(
            "Terrain feedback byte count must be a multiple of four");
    }
    if (byteCount != 0 && texels == nullptr)
    {
        throw std::invalid_argument(
            "Terrain feedback summary requires non-null texels");
    }

    struct Aggregate
    {
        TerrainFeedbackPageRecord record;
    };

    TerrainFeedbackFrameSummary summary;
    summary.frameTag = expectedFrameTag;

    std::unordered_map<uint32_t, Aggregate> pageAggregates;
    pageAggregates.reserve(256);

    const size_t texelCount = byteCount / TerrainFeedbackTexelBytes;
    for (size_t index = 0; index < texelCount; ++index)
    {
        const TerrainFeedbackTexel texel =
            DecodeTerrainFeedbackTexel(
                texels + index * TerrainFeedbackTexelBytes);
        if (!texel.valid)
        {
            ++summary.invalidTexelCount;
            continue;
        }
        if (expectedFrameTag != 0 && texel.frameTag != expectedFrameTag)
        {
            // 有效但 tag 不匹配：旧帧/半写数据，不能计入本帧统计。
            ++summary.staleTexelCount;
            continue;
        }

        ++summary.validTexelCount;
        summary.maxPriority = std::max(summary.maxPriority, texel.priority);

        const uint32_t key = PageKey(texel.virtualPageX, texel.virtualPageY);
        auto inserted = pageAggregates.emplace(key, Aggregate{});
        TerrainFeedbackPageRecord& page = inserted.first->second.record;
        if (inserted.second)
        {
            page.virtualPageX = texel.virtualPageX;
            page.virtualPageY = texel.virtualPageY;
        }
        page.miss = page.miss || texel.miss;
        page.priority = std::max(page.priority, texel.priority);
        ++page.sampleCount;
    }

    summary.distinctPageCount =
        static_cast<uint32_t>(pageAggregates.size());
    summary.pages.reserve(pageAggregates.size());
    for (const auto& [key, aggregate] : pageAggregates)
    {
        (void)key;
        summary.pages.push_back(aggregate.record);
        if (aggregate.record.miss)
            ++summary.missPageCount;
    }

    std::sort(
        summary.pages.begin(),
        summary.pages.end(),
        [](const TerrainFeedbackPageRecord& left,
           const TerrainFeedbackPageRecord& right)
        {
            if (left.priority != right.priority)
                return left.priority > right.priority;
            if (left.virtualPageY != right.virtualPageY)
                return left.virtualPageY < right.virtualPageY;
            return left.virtualPageX < right.virtualPageX;
        });
    return summary;
}

TerrainFeedbackRingState::TerrainFeedbackRingState(uint32_t slotCount)
{
    Reset(slotCount);
}

void TerrainFeedbackRingState::Reset(uint32_t slotCount)
{
    slots.assign(slotCount, Slot{});
}

uint32_t TerrainFeedbackRingState::SlotCount() const noexcept
{
    return static_cast<uint32_t>(slots.size());
}

void TerrainFeedbackRingState::Record(
    uint32_t slot,
    uint64_t copiedEpoch,
    uint8_t frameTag)
{
    if (slot >= slots.size())
    {
        throw std::out_of_range(
            "Terrain feedback ring slot is out of range");
    }
    if (copiedEpoch == 0)
    {
        throw std::invalid_argument(
            "Terrain feedback ring requires a non-zero epoch");
    }
    if (frameTag == 0)
    {
        throw std::invalid_argument(
            "Terrain feedback ring requires a non-zero frame tag");
    }

    Slot& target = slots[slot];
    target.pending = true;
    target.copiedEpoch = copiedEpoch;
    target.frameTag = frameTag;
}

std::optional<TerrainFeedbackRingState::CompletedFrame>
TerrainFeedbackRingState::TryConsume(
    uint32_t slot,
    uint64_t completedEpoch)
{
    if (slot >= slots.size())
        return std::nullopt;

    Slot& target = slots[slot];
    if (!target.pending ||
        target.copiedEpoch == 0 ||
        target.copiedEpoch > completedEpoch)
    {
        return std::nullopt;
    }

    CompletedFrame completed;
    completed.copiedEpoch = target.copiedEpoch;
    completed.frameTag = target.frameTag;
    target.pending = false;
    target.copiedEpoch = 0;
    target.frameTag = 0;
    return completed;
}

bool TerrainFeedbackRingState::IsPending(uint32_t slot) const noexcept
{
    return slot < slots.size() && slots[slot].pending;
}

uint64_t TerrainFeedbackRingState::CopiedEpoch(uint32_t slot) const noexcept
{
    return slot < slots.size() ? slots[slot].copiedEpoch : 0;
}

uint8_t TerrainFeedbackRingState::FrameTag(uint32_t slot) const noexcept
{
    return slot < slots.size() ? slots[slot].frameTag : 0;
}

bool TerrainFeedbackRingState::PendingTagRange(
    uint8_t& oldest,
    uint8_t& newest) const noexcept
{
    bool hasTag = false;
    for (const Slot& slot : slots)
    {
        if (!slot.pending)
            continue;
        if (!hasTag)
        {
            oldest = slot.frameTag;
            newest = slot.frameTag;
            hasTag = true;
            continue;
        }
        oldest = std::min(oldest, slot.frameTag);
        newest = std::max(newest, slot.frameTag);
    }
    return hasTag;
}

} // namespace VL
