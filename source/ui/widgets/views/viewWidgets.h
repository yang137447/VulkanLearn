#pragma once

#include "ui/widgets/input/inputWidgets.h"

namespace VL::UiWidgets
{
// 行消费纯展示数据；树展开集合和选择状态由宿主拥有，不持有场景对象。
struct TreeRowData
{
    std::string label;
    WidgetAction select;
    WidgetAction toggle;
    uint32_t depth = 0;
    bool selected = false;
    bool hasChildren = false;
    bool expanded = false;
    // 行首类型图示（资产相对路径，空表示没有图标）；只是类型图标，不代表缩略图。
    std::string icon;
    // 追加到行容器的 class，让宿主区分分组行/actor 行/子项行；宿主提供的短标识。
    std::string rowClass;
};

std::string ListRow(const WidgetAction& action, const std::string& label, bool selected = false,
    const std::string& icon = {});
std::string TreeRow(const TreeRowData& row);
} // namespace VL::UiWidgets
