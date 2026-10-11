#include "ui/widgets/layout/layoutWidgets.h"

#include "ui/widgets/text/textWidgets.h"

namespace VL::UiWidgets
{
std::string HorizontalBox(const std::string& childrenRml)
{
    return "<div class=\"horizontal-box\">" + childrenRml + "</div>";
}

std::string VerticalBox(const std::string& childrenRml)
{
    return "<div class=\"vertical-box\">" + childrenRml + "</div>";
}

std::string ScrollBox(const std::string& id, const std::string& childrenRml)
{
    return "<div id=\"" + Text(id) + "\" class=\"scroll-box\">" + childrenRml + "</div>";
}
} // namespace VL::UiWidgets
