#include "ui/widgets/navigation/tabWidgets.h"

#include "ui/widgets/text/textWidgets.h"
#include "ui/widgets/views/viewWidgets.h"

namespace VL::UiWidgets
{
std::string DocumentTab(const std::string& label, const WidgetAction& activate,
    const WidgetAction& close, bool selected, const std::string& icon)
{
    return "<div class=\"document-tab\" title=\"" + Text(label) + "\">" +
        DocumentTabContent(label, activate, close, selected, icon) + "</div>";
}

std::string DocumentTabContent(const std::string& label, const WidgetAction& activate,
    const WidgetAction& close, bool selected, const std::string& icon)
{
    // 激活与关闭是同级按钮；关闭不会冒泡成另一次激活或成为停靠拖动的起点。
    const std::string image = icon.empty() ? std::string() :
        "<img class=\"row-icon\" src=\"" + Text(icon) + "\"/>";
    return "<button class=\"list-item tab-activate" + std::string(selected ? " selected" : "") + "\"" +
        ActionAttributes(activate) + ">" + image + "<span class=\"tab-label\">" + Text(label) +
        "</span></button><button class=\"tab-close\"" + ActionAttributes(close) +
        " title=\"Close Tab\"><span>&#215;</span></button>";
}

std::string MenuItem(const std::string& id, const WidgetAction& action, const std::string& label,
    bool checked, bool disabled)
{
    const std::string identity = id.empty() ? std::string() : " id=\"" + Text(id) + "\"";
    const std::string state = disabled ? " disabled=\"disabled\"" : "";
    return "<button" + identity + ActionAttributes(action) + " class=\"menu-item" +
        (disabled ? " disabled" : "") + "\"" + state + ">" +
        "<span class=\"menu-check\">" + (checked ? "&#10003;" : "") + "</span>" +
        "<span class=\"menu-label\">" + Text(label) + "</span></button>";
}
} // namespace VL::UiWidgets
