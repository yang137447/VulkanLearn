#include "editor/ui/tressfxGroupEditor.h"

#include "hair/tressfxGroupRanges.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <limits>
#include <string>

namespace
{
using nlohmann::json;

constexpr uint32_t MaxGuideIndex = std::numeric_limits<uint32_t>::max();

std::string Trim(std::string_view text)
{
    size_t begin = 0;
    size_t end = text.size();
    while (begin < end && std::isspace(static_cast<unsigned char>(text[begin]))) ++begin;
    while (end > begin && std::isspace(static_cast<unsigned char>(text[end - 1]))) --end;
    return std::string(text.substr(begin, end - begin));
}

std::string FoldAscii(std::string_view text)
{
    std::string result(text);
    for (char& ch : result)
        if (ch >= 'A' && ch <= 'Z') ch = static_cast<char>(ch - 'A' + 'a');
    return result;
}

// 只接受非负整数或整值浮点（JSON 的 4 与 4.0 等价）；布尔/字符串/负数一律拒绝。
bool ReadGuideIndex(const json& value, uint32_t& output)
{
    if (value.is_number_unsigned())
    {
        const auto number = value.get<uint64_t>();
        if (number > MaxGuideIndex) return false;
        output = static_cast<uint32_t>(number);
        return true;
    }
    if (value.is_number_integer())
    {
        const auto number = value.get<int64_t>();
        if (number < 0 || static_cast<uint64_t>(number) > MaxGuideIndex) return false;
        output = static_cast<uint32_t>(number);
        return true;
    }
    if (value.is_number_float())
    {
        const double number = value.get<double>();
        if (!std::isfinite(number) || number < 0.0 || number > MaxGuideIndex ||
            std::floor(number) != number)
            return false;
        output = static_cast<uint32_t>(number);
        return true;
    }
    return false;
}

bool IsFiniteNumber(const json& value)
{
    if (value.is_number_unsigned() || value.is_number_integer()) return true;
    return value.is_number_float() && std::isfinite(value.get<double>());
}

// 区间去重计数：先排序再合并，重叠输入也只计一次。
uint64_t UniqueGuideCount(const std::vector<std::pair<uint32_t, uint32_t>>& ranges)
{
    std::vector<std::pair<uint32_t, uint32_t>> sorted = ranges;
    std::sort(sorted.begin(), sorted.end());
    uint64_t total = 0;
    bool open = false;
    uint32_t currentStart = 0;
    uint32_t currentEnd = 0;
    for (const auto& range : sorted)
    {
        if (!open || range.first > currentEnd)
        {
            if (open) total += uint64_t(currentEnd) - currentStart;
            currentStart = range.first;
            currentEnd = range.second;
            open = true;
        }
        else if (range.second > currentEnd)
            currentEnd = range.second;
    }
    if (open) total += uint64_t(currentEnd) - currentStart;
    return total;
}

bool RangesOverlap(const std::pair<uint32_t, uint32_t>& left,
    const std::pair<uint32_t, uint32_t>& right)
{
    // 半开区间首尾相接（[0,2) 与 [2,4)）不算重叠。
    return left.first < right.second && right.first < left.second;
}

struct OwnedRange
{
    uint32_t start;
    uint32_t end;
    size_t group;
};

bool OwnedRangeStartsBefore(const OwnedRange& left, const OwnedRange& right)
{
    return left.start != right.start ? left.start < right.start : left.end < right.end;
}

} // namespace

namespace VL::Editor
{

bool TressfxGroupEditor::IsReservedName(std::string_view name)
{
    const std::string folded = FoldAscii(Trim(name));
    return folded == "default" || folded == "unassigned";
}

TressfxGroupColor TressfxGroupEditor::GroupColor(std::string_view name)
{
    return VL::Hair::TressfxGroupColorForName(name);
}

std::vector<std::pair<uint32_t, uint32_t>> TressfxGroupEditor::CompressGuides(
    const std::vector<uint32_t>& guideIndices)
{
    return VL::Hair::CompressTressfxGuides(guideIndices);
}

bool TressfxGroupEditor::IsAllowedOverrideField(std::string_view field)
{
    return field == "damping" || field == "bendStiffness" ||
        field == "globalShapeStiffness" || field == "globalShapeEffectiveRange" ||
        field == "vspCoefficient" || field == "vspAccelThresholdMax";
}

bool TressfxGroupEditor::Load(const json& document, uint32_t guideCount, std::string& error)
{
    groups_.clear();
    guideCount_ = guideCount;
    error.clear();
    if (!document.is_object())
    {
        error = "TressFX asset document must be an object";
        return false;
    }
    if (!document.contains("groups")) return true;
    const json& array = document.at("groups");
    if (!array.is_array())
    {
        error = "TressFX groups must be an array";
        return false;
    }
    // 空数组按“无分组”处理；WriteGroupArray 会把空列表写成删除 groups 键。
    for (size_t index = 0; index < array.size(); ++index)
    {
        const json& node = array[index];
        const std::string context = "TressFX group #" + std::to_string(index);
        if (!node.is_object())
        {
            error = context + " must be an object";
            groups_.clear();
            return false;
        }
        for (auto field = node.begin(); field != node.end(); ++field)
        {
            if (field.key() != "name" && field.key() != "guideRanges" &&
                field.key() != "simulationOverrides")
            {
                error = context + " has unknown field \"" + field.key() + "\"";
                groups_.clear();
                return false;
            }
        }
        TressfxGuideGroupDraft draft;
        if (!node.contains("name") || !node.at("name").is_string())
        {
            error = context + " requires a string name";
            groups_.clear();
            return false;
        }
        draft.name = node.at("name").get<std::string>();
        draft.originName = draft.name;
        if (!node.contains("guideRanges") || !node.at("guideRanges").is_array())
        {
            error = context + " requires a guideRanges array";
            groups_.clear();
            return false;
        }
        for (const json& pair : node.at("guideRanges"))
        {
            if (!pair.is_array() || pair.size() != 2)
            {
                error = context + " guideRanges entry must be a [start, end] pair";
                groups_.clear();
                return false;
            }
            uint32_t start = 0;
            uint32_t end = 0;
            if (!ReadGuideIndex(pair[0], start) || !ReadGuideIndex(pair[1], end))
            {
                error = context + " guideRanges entry must contain non-negative integers";
                groups_.clear();
                return false;
            }
            draft.guideRanges.emplace_back(start, end);
        }
        if (node.contains("simulationOverrides"))
        {
            if (!node.at("simulationOverrides").is_object())
            {
                error = context + " simulationOverrides must be an object";
                groups_.clear();
                return false;
            }
            draft.simulationOverrides = node.at("simulationOverrides");
        }
        groups_.push_back(std::move(draft));
    }
    if (!Validate(error))
    {
        groups_.clear();
        return false;
    }
    return true;
}

uint32_t TressfxGroupEditor::GuideCountFor(size_t index) const
{
    if (index >= groups_.size()) return 0;
    const uint64_t count = UniqueGuideCount(groups_[index].guideRanges);
    return count > MaxGuideIndex ? MaxGuideIndex : static_cast<uint32_t>(count);
}

std::vector<std::string> TressfxGroupEditor::GroupNames() const
{
    std::vector<std::string> names;
    names.reserve(groups_.size());
    for (const TressfxGuideGroupDraft& group : groups_)
        names.push_back(group.name);
    return names;
}

std::vector<uint32_t> TressfxGroupEditor::GuideGroupIds() const
{
    if (guideCount_ == 0) return {};
    std::vector<uint32_t> ids(guideCount_, 0u);
    for (size_t group = 0; group < groups_.size(); ++group)
    {
        const uint32_t runtimeId = static_cast<uint32_t>(group + 1);
        for (const auto& range : groups_[group].guideRanges)
        {
            const uint32_t end = std::min(range.second, guideCount_);
            for (uint32_t guide = range.first; guide < end; ++guide)
                ids[guide] = runtimeId;
        }
    }
    return ids;
}

uint32_t TressfxGroupEditor::DefaultGuideCount() const
{
    if (guideCount_ == 0) return 0;
    uint64_t covered = 0;
    for (size_t index = 0; index < groups_.size(); ++index)
        covered += GuideCountFor(index);
    return covered >= guideCount_ ? 0 : static_cast<uint32_t>(guideCount_ - covered);
}

bool TressfxGroupEditor::ValidateName(const std::string& name, size_t ignoreIndex,
    std::string& error) const
{
    const std::string trimmed = Trim(name);
    if (trimmed.empty())
    {
        error = "TressFX group name must not be empty";
        return false;
    }
    // 与 MeshAssetValidator 保持同一规则：资产里的名称不能靠 Trim 后才唯一，
    // 否则编辑器保存的 groups 可能通不过下一次导入校验。
    if (trimmed != name)
    {
        error = "TressFX group name must not have leading or trailing whitespace: \"" +
            name + "\"";
        return false;
    }
    if (IsReservedName(trimmed))
    {
        error = "TressFX group name \"" + trimmed + "\" is reserved";
        return false;
    }
    const std::string folded = FoldAscii(trimmed);
    for (size_t index = 0; index < groups_.size(); ++index)
    {
        if (index == ignoreIndex) continue;
        if (FoldAscii(Trim(groups_[index].name)) == folded)
        {
            error = "TressFX group name is duplicated: \"" + trimmed + "\"";
            return false;
        }
    }
    return true;
}

bool TressfxGroupEditor::ValidateRange(const std::pair<uint32_t, uint32_t>& range,
    std::string& error) const
{
    if (range.first >= range.second)
    {
        error = "guide range [" + std::to_string(range.first) + ", " +
            std::to_string(range.second) + ") requires 0 <= start < end";
        return false;
    }
    // guideCount 未知（0）时无法判断上界；由 .tfx 解析层继续兜底。
    if (guideCount_ != 0 && range.second > guideCount_)
    {
        error = "guide range [" + std::to_string(range.first) + ", " +
            std::to_string(range.second) + ") exceeds the .tfx guide count " +
            std::to_string(guideCount_);
        return false;
    }
    return true;
}

bool TressfxGroupEditor::RangeConflicts(const std::pair<uint32_t, uint32_t>& range,
    size_t ignoreGroup, size_t ignoreRange) const
{
    for (size_t group = 0; group < groups_.size(); ++group)
    {
        for (size_t rangeIndex = 0; rangeIndex < groups_[group].guideRanges.size(); ++rangeIndex)
        {
            if (group == ignoreGroup && rangeIndex == ignoreRange) continue;
            if (RangesOverlap(range, groups_[group].guideRanges[rangeIndex])) return true;
        }
    }
    return false;
}

bool TressfxGroupEditor::Validate(std::string& error) const
{
    error.clear();
    for (size_t group = 0; group < groups_.size(); ++group)
    {
        const TressfxGuideGroupDraft& draft = groups_[group];
        if (!ValidateName(draft.name, group, error)) return false;
        const std::string context = "TressFX group \"" + Trim(draft.name) + "\": ";
        // 空组合法：作者可以新建组后用笔刷填充。
        for (const auto& range : draft.guideRanges)
        {
            std::string detail;
            if (!ValidateRange(range, detail))
            {
                error = context + detail;
                return false;
            }
        }
        if (!draft.simulationOverrides.is_object())
        {
            error = context + "simulationOverrides must be an object";
            return false;
        }
        for (auto field = draft.simulationOverrides.begin();
             field != draft.simulationOverrides.end(); ++field)
        {
            if (!IsAllowedOverrideField(field.key()))
            {
                error = context + "simulationOverrides field \"" + field.key() +
                    "\" is not allowed";
                return false;
            }
            if (!IsFiniteNumber(field.value()))
            {
                error = context + "simulationOverrides field \"" + field.key() +
                    "\" must be a finite number";
                return false;
            }
        }
    }
    // 跨组重叠：按起点扫描并记录已出现的最大终点；首尾相接允许，共享 guide 拒绝。
    // 用 O(n log n) 扫描避免编辑器保存时卡住；区间总数另按上限拒绝。
    std::vector<OwnedRange> ranges;
    size_t totalRanges = 0;
    for (const TressfxGuideGroupDraft& draft : groups_)
        totalRanges += draft.guideRanges.size();
    if (totalRanges > MaxGuideRanges)
    {
        error = "TressFX groups contain " + std::to_string(totalRanges) +
            " guide ranges; the limit is " + std::to_string(MaxGuideRanges) +
            ". Split groups or use wider numeric ranges";
        return false;
    }
    ranges.reserve(totalRanges);
    for (size_t group = 0; group < groups_.size(); ++group)
        for (const auto& range : groups_[group].guideRanges)
            ranges.push_back({range.first, range.second, group});
    std::sort(ranges.begin(), ranges.end(), OwnedRangeStartsBefore);
    if (!ranges.empty())
    {
        size_t owner = ranges.front().group;
        uint32_t maxEnd = ranges.front().end;
        for (size_t index = 1; index < ranges.size(); ++index)
        {
            if (ranges[index].start < maxEnd)
            {
                error = owner == ranges[index].group
                    ? "TressFX group guideRanges overlap within group \"" +
                        Trim(groups_[owner].name) + "\""
                    : "TressFX guide belongs to two groups: \"" +
                        Trim(groups_[owner].name) + "\" and \"" +
                        Trim(groups_[ranges[index].group].name) + "\"";
                return false;
            }
            if (ranges[index].end > maxEnd)
            {
                maxEnd = ranges[index].end;
                owner = ranges[index].group;
            }
        }
    }
    return true;
}

bool TressfxGroupEditor::AddGroup(std::string name, std::pair<uint32_t, uint32_t> firstRange,
    std::string& error)
{
    error.clear();
    // 空白校验必须看用户原始输入：先 Trim 再 Validate 会让 " crown " 静默通过，
    // 与资产层“拒绝前后空白”的规则不一致。
    if (!ValidateName(name, groups_.size(), error)) return false;
    const std::string trimmed = Trim(name);
    TressfxGuideGroupDraft draft;
    draft.name = trimmed;
    std::string detail;
    if (!ValidateRange(firstRange, detail))
    {
        error = "TressFX group \"" + trimmed + "\": " + detail;
        return false;
    }
    if (RangeConflicts(firstRange, groups_.size(), 0))
    {
        error = "TressFX group \"" + trimmed + "\" first range overlaps an existing group";
        return false;
    }
    size_t totalRanges = 0;
    for (const TressfxGuideGroupDraft& draft : groups_)
        totalRanges += draft.guideRanges.size();
    if (totalRanges >= MaxGuideRanges)
    {
        error = "TressFX groups already contain " + std::to_string(totalRanges) +
            " guide ranges; the limit is " + std::to_string(MaxGuideRanges);
        return false;
    }
    draft.guideRanges.push_back(firstRange);
    groups_.push_back(std::move(draft));
    return true;
}

bool TressfxGroupEditor::AddEmptyGroup(std::string name, std::string& error)
{
    error.clear();
    const std::string trimmed = Trim(name);
    if (!ValidateName(name, groups_.size(), error)) return false;
    TressfxGuideGroupDraft draft;
    draft.name = trimmed;
    draft.originName.clear();
    groups_.push_back(std::move(draft));
    return true;
}

void TressfxGroupEditor::DeleteGroup(size_t index)
{
    // vector erase 即完成组号前移：k+1..N 的运行时组号自然变成 k..N-1。
    if (index < groups_.size()) groups_.erase(groups_.begin() + static_cast<std::ptrdiff_t>(index));
}

bool TressfxGroupEditor::RenameGroup(size_t index, std::string name, std::string& error)
{
    error.clear();
    if (index >= groups_.size())
    {
        error = "TressFX group index is out of range";
        return false;
    }
    if (!ValidateName(name, index, error)) return false;
    groups_[index].name = Trim(name);
    return true;
}

bool TressfxGroupEditor::AddRange(size_t index, std::pair<uint32_t, uint32_t> range,
    std::string& error)
{
    error.clear();
    if (index >= groups_.size())
    {
        error = "TressFX group index is out of range";
        return false;
    }
    std::string detail;
    if (!ValidateRange(range, detail))
    {
        error = "TressFX group \"" + Trim(groups_[index].name) + "\": " + detail;
        return false;
    }
    if (RangeConflicts(range, groups_.size(), 0))
    {
        error = "TressFX guide range [" + std::to_string(range.first) + ", " +
            std::to_string(range.second) + ") overlaps an existing group";
        return false;
    }
    size_t totalRanges = 0;
    for (const TressfxGuideGroupDraft& draft : groups_)
        totalRanges += draft.guideRanges.size();
    // 先检查再插入：失败时不能留下已修改的草稿。
    if (totalRanges >= MaxGuideRanges)
    {
        error = "TressFX groups already contain " + std::to_string(totalRanges) +
            " guide ranges; the limit is " + std::to_string(MaxGuideRanges);
        return false;
    }
    std::vector<std::pair<uint32_t, uint32_t>>& ranges = groups_[index].guideRanges;
    size_t position = 0;
    while (position < ranges.size() && ranges[position].first < range.first) ++position;
    ranges.insert(ranges.begin() + static_cast<std::ptrdiff_t>(position), range);
    // 首尾相接的区间合并成一段，避免笔刷连续 stamp 把同一撮头发拆成大量单元素 range。
    size_t merged = 0;
    while (merged + 1 < ranges.size())
    {
        if (ranges[merged].second == ranges[merged + 1].first)
        {
            ranges[merged].second = ranges[merged + 1].second;
            ranges.erase(ranges.begin() + static_cast<std::ptrdiff_t>(merged + 1));
        }
        else
            ++merged;
    }
    return true;
}

bool TressfxGroupEditor::RemoveRange(size_t index, size_t rangeIndex, std::string& error)
{
    error.clear();
    if (index >= groups_.size() || rangeIndex >= groups_[index].guideRanges.size())
    {
        error = "TressFX group range index is out of range";
        return false;
    }
    std::vector<std::pair<uint32_t, uint32_t>>& ranges = groups_[index].guideRanges;
    ranges.erase(ranges.begin() + static_cast<std::ptrdiff_t>(rangeIndex));
    // 空组是合法作者状态：删掉最后一个 range 后保留组，等待笔刷重新填充。
    return true;
}

bool TressfxGroupEditor::ApplyPaintedGuides(size_t targetIndex,
    const std::vector<uint32_t>& addGuides,
    const std::vector<uint32_t>& removeGuides, std::string& error)
{
    error.clear();
    if (targetIndex >= groups_.size())
    {
        error = "TressFX group index is out of range";
        return false;
    }
    if (addGuides.empty() && removeGuides.empty()) return true;

    std::vector<TressfxGuideGroupDraft> candidate = groups_;
    const std::string targetName = candidate[targetIndex].name;
    if (!addGuides.empty())
    {
        // 先移除再添加：replace existing 的旧组若因此为空，随后统一删除。
        for (TressfxGuideGroupDraft& group : candidate)
            VL::Hair::RemoveTressfxGuidesFromRanges(group.guideRanges, addGuides);
        auto target = std::find_if(candidate.begin(), candidate.end(),
            [&](const TressfxGuideGroupDraft& group) { return group.name == targetName; });
        if (target == candidate.end())
        {
            error = "TressFX paint target disappeared";
            return false;
        }
        VL::Hair::AddTressfxGuidesToRanges(target->guideRanges, addGuides);
    }
    else
    {
        VL::Hair::RemoveTressfxGuidesFromRanges(
            candidate[targetIndex].guideRanges, removeGuides);
    }
    std::swap(groups_, candidate);
    if (!Validate(error))
    {
        std::swap(groups_, candidate);
        return false;
    }
    return true;
}

bool TressfxGroupEditor::SetOverride(size_t index, std::string_view field,
    std::optional<double> value)
{
    if (index >= groups_.size() || !IsAllowedOverrideField(field)) return false;
    json& overrides = groups_[index].simulationOverrides;
    if (!overrides.is_object()) overrides = json::object();
    const std::string key(field);
    if (!value.has_value())
    {
        overrides.erase(key);
        return true;
    }
    if (!std::isfinite(*value)) return false;
    overrides[key] = *value;
    return true;
}

void TressfxGroupEditor::ClearOverrides(size_t index)
{
    if (index < groups_.size()) groups_[index].simulationOverrides = json::object();
}

void TressfxGroupEditor::WriteGroupArray(json& document) const
{
    if (groups_.empty())
    {
        // 空分组与未声明 groups 语义相同；移除键而不是写空数组。
        document.erase("groups");
        return;
    }
    json array = json::array();
    for (const TressfxGuideGroupDraft& draft : groups_)
    {
        json ranges = json::array();
        for (const auto& range : draft.guideRanges)
            ranges.push_back({range.first, range.second});
        json entry = {{"name", draft.name}, {"guideRanges", ranges}};
        // 空 overrides 不写键，保持既有 schema；颜色从不落盘。
        if (!draft.simulationOverrides.empty())
            entry["simulationOverrides"] = draft.simulationOverrides;
        array.push_back(std::move(entry));
    }
    document["groups"] = std::move(array);
}

} // namespace VL::Editor
