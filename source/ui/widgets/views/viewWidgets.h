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
};

std::string ListRow(const WidgetAction& action, const std::string& label, bool selected = false);
std::string TreeRow(const TreeRowData& row);
} // namespace VL::UiWidgets
