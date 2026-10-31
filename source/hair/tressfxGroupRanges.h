#pragma once

#include <algorithm>
#include <cstdint>
#include <utility>
#include <vector>

namespace VL::Hair
{

using TressfxGuideRange = std::pair<uint32_t, uint32_t>;

// 合并半开区间 [start,end)：排序后合并重叠与首尾相接项。
inline std::vector<TressfxGuideRange> MergeTressfxGuideRanges(
    std::vector<TressfxGuideRange> ranges)
{
    std::sort(ranges.begin(), ranges.end());
    std::vector<TressfxGuideRange> merged;
    for (const TressfxGuideRange& range : ranges)
    {
        if (range.first >= range.second) continue;
        if (!merged.empty() && range.first <= merged.back().second)
        {
            merged.back().second = std::max(merged.back().second, range.second);
            continue;
        }
        merged.push_back(range);
    }
    return merged;
}

// 把任意 guide 索引集合压缩成升序、互不相邻的半开区间。
inline std::vector<TressfxGuideRange> CompressTressfxGuides(
    const std::vector<uint32_t>& guideIndices)
{
    std::vector<uint32_t> sorted = guideIndices;
    std::sort(sorted.begin(), sorted.end());
    sorted.erase(std::unique(sorted.begin(), sorted.end()), sorted.end());
    std::vector<TressfxGuideRange> ranges;
    for (uint32_t index : sorted)
    {
        // 半开区间端点必须能表示 index+1；UINT32_MAX 不在导入预算内。
        if (index == UINT32_MAX) continue;
        if (!ranges.empty() && ranges.back().second == index)
            ranges.back().second = index + 1;
        else
            ranges.emplace_back(index, index + 1);
    }
    return ranges;
}

// ranges 原地加入 guides，并保持排序/合并；用于笔刷提交的整根 guide 集合。
inline void AddTressfxGuidesToRanges(
    std::vector<TressfxGuideRange>& ranges,
    const std::vector<uint32_t>& guideIndices)
{
    std::vector<TressfxGuideRange> combined = std::move(ranges);
    std::vector<TressfxGuideRange> added = CompressTressfxGuides(guideIndices);
    combined.insert(combined.end(), added.begin(), added.end());
    ranges = MergeTressfxGuideRanges(std::move(combined));
}

// ranges 原地移除 guides。输入 guide 已排序时走双指针，避免为每根 guide 重建整表。
inline void RemoveTressfxGuidesFromRanges(
    std::vector<TressfxGuideRange>& ranges,
    const std::vector<uint32_t>& guideIndices)
{
    std::vector<uint32_t> sorted = guideIndices;
    std::sort(sorted.begin(), sorted.end());
    sorted.erase(std::unique(sorted.begin(), sorted.end()), sorted.end());
    std::vector<TressfxGuideRange> result;
    size_t guide = 0;
    for (const TressfxGuideRange& range : MergeTressfxGuideRanges(ranges))
    {
        uint32_t cursor = range.first;
        while (guide < sorted.size() && sorted[guide] < cursor) ++guide;
        while (guide < sorted.size() && sorted[guide] < range.second)
        {
            const uint32_t removed = sorted[guide++];
            if (removed > cursor) result.emplace_back(cursor, removed);
            cursor = removed + 1;
        }
        if (cursor < range.second) result.emplace_back(cursor, range.second);
    }
    ranges = MergeTressfxGuideRanges(std::move(result));
}

} // namespace VL::Hair
