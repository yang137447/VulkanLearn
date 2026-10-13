#include "ui/widgets/navigation/tabWidgets.h"

#include "ui/widgets/text/textWidgets.h"
#include "ui/widgets/views/viewWidgets.h"

namespace VL::UiWidgets
{
std::string DocumentTab(const std::string& label, const WidgetAction& activate,
    const WidgetAction& close, bool selected)
{
    return "<div class=\"document-tab\" title=\"" + Text(label) + "\">" + ListRow(activate, label, selected) +
        "<button class=\"tab-close\"" + ActionAttributes(close) + ">&#215;</button></div>";
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
