#include "ui/widgets/views/viewWidgets.h"

#include "ui/widgets/text/textWidgets.h"

namespace VL::UiWidgets
{
std::string ListRow(const WidgetAction& action, const std::string& label, bool selected)
{
    return Button("", action, label, ButtonStyle::ListItem, selected);
}

std::string TreeRow(const TreeRowData& row)
{
    std::string disclosure = "<span class=\"tree-spacer\"></span>";
    if (row.hasChildren)
    {
        // 使用基础字形避免平台字体缺少三角符号；不接受业务提供的图标 RML。
        disclosure = "<button class=\"tree-toggle\"" + ActionAttributes(row.toggle) +
            " aria-expanded=\"" + (row.expanded ? "true" : "false") + "\">" +
            (row.expanded ? "-" : "+") + "</button>";
    }
    return "<div class=\"tree-row\" style=\"padding-left: " + std::to_string(row.depth * 16u) +
        "px;\">" + disclosure + ListRow(row.select, row.label, row.selected) + "</div>";
}
} // namespace VL::UiWidgets
