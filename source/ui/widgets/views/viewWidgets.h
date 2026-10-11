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
    // hiddenRow 追加固定 class "hidden-row"：不引入任意 class 文本，避免把样式注入
    // 变成第二个不受控接口。
    std::string rowClass;
    bool hiddenRow = false;
    // 行尾附加控件（目前只用于逐 actor 可见性）。它渲染在标题按钮之外，
    // 因此点击它不会同时触发 select；icon 是图标路径，alternate 表示"当前是隐藏态"。
    // 控件不参与行高：自身绝对定位在行内，行的 30px 由 RCSS 固定。
    bool hasTrailing = false;
    WidgetAction trailing;
    std::string trailingIcon;
    std::string trailingLabel;
    bool trailingAlternate = false;
};

std::string ListRow(const WidgetAction& action, const std::string& label, bool selected = false,
    const std::string& icon = {});
std::string TreeRow(const TreeRowData& row);
} // namespace VL::UiWidgets
