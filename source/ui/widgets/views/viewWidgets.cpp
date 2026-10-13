#include "ui/widgets/views/viewWidgets.h"

#include "ui/widgets/text/textWidgets.h"

namespace VL::UiWidgets
{
std::string ListRow(const WidgetAction& action, const std::string& label, bool selected,
    const std::string& icon)
{
    return Button("", action, label, ButtonStyle::ListItem, selected, icon);
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
    // rowClass 是宿主给出的短标识（group-row / actor-row / …），只进入 class 属性。
    const std::string extraClass = row.rowClass.empty() ? std::string() : " " + Text(row.rowClass);
    // hiddenRow 是固定的第二个 class，不是宿主可控文本。
    const std::string hiddenClass = row.hiddenRow ? " hidden-row" : "";
    // 行尾控件与行标题并列而不是嵌在标题按钮里：既能保持一个点击靶面对应一个动作，
    // 也让"点眼睛"与"点行选中"在事件层就是两个不同的目标元素。
    std::string trailing;
    if (row.hasTrailing)
    {
        trailing = "<button class=\"tree-trailing" +
            std::string(row.trailingAlternate ? " alternate" : "") + "\" title=\"" +
            Text(row.trailingLabel) + "\" aria-label=\"" + Text(row.trailingLabel) + "\"" +
            ActionAttributes(row.trailing) + "><img class=\"row-icon\" src=\"" +
            Text(row.trailingIcon) + "\"/></button>";
    }
    return "<div class=\"tree-row" + extraClass + hiddenClass + "\" style=\"padding-left: " +
        std::to_string(row.depth * 16u) + "px;\">" + disclosure +
        ListRow(row.select, row.label, row.selected, row.icon) + trailing + "</div>";
}
} // namespace VL::UiWidgets
