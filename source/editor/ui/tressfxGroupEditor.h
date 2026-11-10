#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <nlohmann/json.hpp>

#include "hair/tressfxGroupPalette.h"

namespace VL::Editor
{

// TF groups[] 的一份编辑草稿；不包含颜色，颜色由 GroupColor(name) 确定性生成。
struct TressfxGuideGroupDraft
{
    std::string name;
    // 从已保存资产加载时的原始名称。GPU 组号/颜色表按它冻结；
    // 重命名后仍用它映射预览组，未保存的新建组为空（不能预览/笔刷）。
    std::string originName;
    // 半开区间 [start, end)，组内/跨组不重叠，首尾相接允许。
    std::vector<std::pair<uint32_t, uint32_t>> guideRanges;
    nlohmann::json simulationOverrides = nlohmann::json::object();
};

using TressfxGroupColor = VL::Hair::TressfxGroupColor;

// Groups 编辑的纯逻辑模型：不依赖 RmlUi、World、Vulkan，也不直接读写资产文件。
// 只负责 groups[] 草稿的增删改、校验、写回和确定性配色；保存仍走现有 asset modify 链路。
class TressfxGroupEditor
{
public:
    // 笔刷一次提交可能产生大量不连续区间；超过上限时拒绝保存，提示拆组或改用数值范围，
    // 避免单个 groups[] 数组把资产 JSON 和面板 markup 撑大。
    static constexpr size_t MaxGuideRanges = 4096;
    // "Default" 是隐式组 0，不允许作者创建同名组；"unassigned" 也不作为命名组。
    static bool IsReservedName(std::string_view name);
    // FNV-1a + 固定调色板；同一名称跨调用/会话稳定，不写资产。
    static TressfxGroupColor GroupColor(std::string_view name);
    // 把任意顺序的 guide 索引压缩成升序、互不相邻的半开区间。
    static std::vector<std::pair<uint32_t, uint32_t>> CompressGuides(
        const std::vector<uint32_t>& guideIndices);
    // groups[].simulationOverrides 的唯一合法字段集。
    static bool IsAllowedOverrideField(std::string_view field);

    // guideCount 为 0 表示未知（.tfx 缺失/损坏），此时跳过 end <= guideCount 检查；
    // 其余结构、名称、重叠规则仍照常执行。
    bool Load(const nlohmann::json& document, uint32_t guideCount, std::string& error);

    const std::vector<TressfxGuideGroupDraft>& Groups() const { return groups_; }
    uint32_t GuideCount() const { return guideCount_; }
    std::vector<std::string> GroupNames() const;
    // 当前草稿展开成每 guide 一个运行时组号：0=Default，k=声明序第 k 组。
    // 笔刷 session 只消费这份副本；值随每次提交重建，不是持久 ID。
    std::vector<uint32_t> GuideGroupIds() const;
    // 命名组覆盖的 guide 数（区间去重计数）。
    uint32_t GuideCountFor(size_t index) const;
    // 隐式组 0 的数量：guideCount - 所有命名组覆盖数；guideCount 未知时为 0。
    uint32_t DefaultGuideCount() const;
    bool Validate(std::string& error) const;

    // 新组必须带首个非空 range，否则返回错误（资产 schema 不允许空组）。
    bool AddGroup(std::string name, std::pair<uint32_t, uint32_t> firstRange,
        std::string& error);
    // 新建空组：guide 之后由笔刷填充；空数组是合法作者状态。
    bool AddEmptyGroup(std::string name, std::string& error);
    void DeleteGroup(size_t index);
    bool RenameGroup(size_t index, std::string name, std::string& error);
    bool AddRange(size_t index, std::pair<uint32_t, uint32_t> range, std::string& error);
    // 删除组内最后一个 range 时一并删除整组，避免留下无法保存的空组。
    bool RemoveRange(size_t index, size_t rangeIndex, std::string& error);
    // 笔刷提交的唯一写入口：先重排 ranges，再统一 Validate。失败不修改草稿。
    // addGuides 会从所有组移除后加入目标组，实现 Replace existing 的移动；removeGuides
    // 只从目标组移除。空组按 schema 规则删除。
    bool ApplyPaintedGuides(size_t targetIndex,
        const std::vector<uint32_t>& addGuides,
        const std::vector<uint32_t>& removeGuides, std::string& error);
    // value 为空表示继承资产默认值；不在白名单内的字段返回 false。
    bool SetOverride(size_t index, std::string_view field, std::optional<double> value);
    void ClearOverrides(size_t index);

    // 只改写 document["groups"]，不触碰 simulationDefaults；空组列表时移除该键。
    void WriteGroupArray(nlohmann::json& document) const;

private:
    bool ValidateName(const std::string& name, size_t ignoreIndex, std::string& error) const;
    bool ValidateRange(const std::pair<uint32_t, uint32_t>& range, std::string& error) const;
    bool RangeConflicts(const std::pair<uint32_t, uint32_t>& range, size_t ignoreGroup,
        size_t ignoreRange) const;

    std::vector<TressfxGuideGroupDraft> groups_;
    uint32_t guideCount_ = 0;
};

} // namespace VL::Editor
