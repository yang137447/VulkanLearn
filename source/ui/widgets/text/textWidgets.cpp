#include "ui/widgets/text/textWidgets.h"

#include <RmlUi/Core/StringUtilities.h>

namespace VL::UiWidgets
{
std::string Text(const std::string& value)
{
    return Rml::StringUtilities::EncodeRml(value);
}

std::string Heading(const std::string& label)
{
    return "<h3>" + Text(label) + "</h3>";
}

std::string Description(const std::string& text)
{
    return text.empty() ? std::string() : "<p class=\"muted\">" + Text(text) + "</p>";
}
} // namespace VL::UiWidgets
