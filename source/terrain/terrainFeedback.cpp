#include "terrain/terrainFeedback.h"

#include <algorithm>
#include <array>
#include <stdexcept>

namespace VL
{
namespace
{

constexpr uint32_t TerrainFeedbackPageCount =
    1u << (TerrainFeedbackPageBitCount * 2u);

uint32_t PageIndex(uint32_t pageX, uint32_t pageY)
{
    return (pageY << TerrainFeedbackPageBitCount) |
        (pageX & TerrainFeedbackPageMask);
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

    TerrainFeedbackFrameSummary summary;
    summary.frameTag = expectedFrameTag;

    // 页坐标是 5+5 bit，总共固定 1024 个槽位。逐 texel 做 hash/map
    // 会在这个 128×128 回读里产生大量桶查找和重哈希；定长表是 O(1)
    // 直接索引，且和格式版本 1 的 32×32 页表上界完全一致。
    std::array<TerrainFeedbackPageRecord, TerrainFeedbackPageCount> pages{};
    std::array<uint32_t, TerrainFeedbackPageCount> sampleCounts{};

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

        const uint32_t pageIndex =
            PageIndex(texel.virtualPageX, texel.virtualPageY);
        TerrainFeedbackPageRecord& page = pages[pageIndex];
        page.virtualPageX = texel.virtualPageX;
        page.virtualPageY = texel.virtualPageY;
        page.miss = page.miss || texel.miss;
        page.priority = std::max(page.priority, texel.priority);
        ++sampleCounts[pageIndex];
    }

    summary.pages.reserve(TerrainFeedbackPageCount);
    for (uint32_t pageIndex = 0;
         pageIndex < TerrainFeedbackPageCount;
         ++pageIndex)
    {
        if (sampleCounts[pageIndex] == 0)
            continue;
        TerrainFeedbackPageRecord record = pages[pageIndex];
        record.sampleCount = sampleCounts[pageIndex];
        summary.pages.push_back(record);
        ++summary.distinctPageCount;
        if (record.miss)
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
