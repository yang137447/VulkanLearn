#include "hair/tressfxGroupBinding.h"

#include <algorithm>
#include <stdexcept>

namespace VL::Hair
{
namespace
{

struct OwnedRange
{
    uint32_t group = 0;
    uint32_t start = 0;
    uint32_t end = 0;
};

std::vector<OwnedRange> CollectRanges(const std::vector<TressfxGroupDeclaration>& groups)
{
    std::vector<OwnedRange> ranges;
    for (size_t group = 0; group < groups.size(); ++group)
        for (const TressfxGuideRange& range : groups[group].ranges)
        {
            if (range.first >= range.second)
                continue;
            ranges.push_back({static_cast<uint32_t>(group), range.first, range.second});
        }
    std::sort(ranges.begin(), ranges.end(), [](const OwnedRange& left, const OwnedRange& right)
    {
        return left.start < right.start;
    });
    return ranges;
}

} // namespace

void ValidateTressfxGuideGroups(
    const std::vector<TressfxGroupDeclaration>& groups,
    uint32_t guideCount,
    const std::string& where)
{
    for (const TressfxGroupDeclaration& group : groups)
        for (const TressfxGuideRange& range : group.ranges)
            if (range.second > guideCount)
                throw std::runtime_error(
                    "TressFX group \"" + group.name + "\" guideRanges [" +
                    std::to_string(range.first) + ", " + std::to_string(range.second) +
                    ") exceed the .tfx guide count " + std::to_string(guideCount) + where);

    // 按起点扫描并保留已出现的最大终点；半开区间首尾相接不算重叠。
    const std::vector<OwnedRange> ranges = CollectRanges(groups);
    if (ranges.empty())
        return;
    size_t owner = ranges.front().group;
    uint32_t maxEnd = ranges.front().end;
    for (size_t index = 1; index < ranges.size(); ++index)
    {
        if (ranges[index].start < maxEnd)
        {
            const std::string first = groups[owner].name;
            const std::string second = groups[ranges[index].group].name;
            throw std::runtime_error(
                (owner == ranges[index].group
                    ? "TressFX group guideRanges overlap within group \"" + first + "\""
                    : "TressFX guide belongs to two groups: \"" + first +
                        "\" and \"" + second + "\"") + where);
        }
        if (ranges[index].end > maxEnd)
        {
            maxEnd = ranges[index].end;
            owner = ranges[index].group;
        }
    }
}

std::shared_ptr<const TressfxGroupBinding> BuildTressfxGroupBinding(
    const std::vector<TressfxGroupDeclaration>& groups,
    uint32_t guideCount,
    std::string key,
    const std::string& where)
{
    ValidateTressfxGuideGroups(groups, guideCount, where);

    auto binding = std::make_shared<TressfxGroupBinding>();
    binding->guideGroupIds.assign(guideCount, 0u);
    binding->groupNames.reserve(groups.size());
    for (size_t group = 0; group < groups.size(); ++group)
    {
        binding->groupNames.push_back(groups[group].name);
        for (const TressfxGuideRange& range : groups[group].ranges)
            for (uint32_t guide = range.first; guide < range.second; ++guide)
                binding->guideGroupIds[guide] = static_cast<uint32_t>(group + 1u);
    }
    binding->key = std::move(key);
    return binding;
}

bool ValidateTressfxGroupBindingPreviewRequest(
    const TressfxGroupBindingPreviewRequest& request, std::string& error)
{
    error.clear();
    if (request.objectName.empty())
    {
        error = "objectName is empty";
        return false;
    }
    if (request.worldGeneration == 0)
    {
        error = "worldGeneration is zero";
        return false;
    }
    if (request.guideGroupIds.empty())
    {
        error = "guideGroupIds is empty";
        return false;
    }
    if (request.groupNames.size() > TressfxGroupPreviewNamedGroupCapacity)
    {
        error = "named group count " + std::to_string(request.groupNames.size()) +
            " exceeds the preview capacity " +
            std::to_string(TressfxGroupPreviewNamedGroupCapacity);
        return false;
    }
    const uint32_t activeColorCount =
        static_cast<uint32_t>(request.groupNames.size()) + 1u;
    for (uint32_t groupId : request.guideGroupIds)
        if (groupId >= activeColorCount)
        {
            error = "guide group id " + std::to_string(groupId) +
                " exceeds the declared group count " +
                std::to_string(activeColorCount);
            return false;
        }
    return true;
}

TressfxGroupBinding MakeTressfxGroupBinding(
    const TressfxGroupBindingPreviewRequest& request)
{
    TressfxGroupBinding binding;
    binding.guideGroupIds = request.guideGroupIds;
    binding.groupNames = request.groupNames;
    binding.key = request.bindingKey;
    return binding;
}

bool ValidateTressfxGroupPreviewBinding(
    const TressfxGroupBinding& binding, uint32_t guideCount, std::string& error)
{
    error.clear();
    if (guideCount == 0)
    {
        error = "the renderable has no guides";
        return false;
    }
    if (binding.guideGroupIds.size() != guideCount)
    {
        error = "guide count " + std::to_string(binding.guideGroupIds.size()) +
            " does not match the renderable guide count " +
            std::to_string(guideCount);
        return false;
    }
    if (binding.groupNames.size() > TressfxGroupPreviewNamedGroupCapacity)
    {
        error = "named group count " + std::to_string(binding.groupNames.size()) +
            " exceeds the preview capacity " +
            std::to_string(TressfxGroupPreviewNamedGroupCapacity);
        return false;
    }
    const uint32_t activeColorCount =
        static_cast<uint32_t>(binding.groupNames.size()) + 1u;
    for (uint32_t groupId : binding.guideGroupIds)
        if (groupId >= activeColorCount)
        {
            error = "guide group id " + std::to_string(groupId) +
                " exceeds the declared group count " +
                std::to_string(activeColorCount);
            return false;
        }
    return true;
}

} // namespace VL::Hair
