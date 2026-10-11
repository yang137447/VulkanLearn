#include "ui/widgets/input/inputWidgets.h"

#include <cmath>
#include <iomanip>
#include <sstream>

#include "ui/widgets/text/textWidgets.h"

namespace VL::UiWidgets
{
namespace
{
// RmlUi 的 WidgetSlider 默认 step=1，会把浮点滑条量化成整数，因此必须显式给出步长。
constexpr float SliderStepDivisions = 1000.0f;

std::string Number(float value)
{
    std::ostringstream stream;
    stream << std::setprecision(9) << value;
    return stream.str();
}

// 步长只是拖动粒度，不参与业务语义；区间不可用时不写 step 属性，让控件保持自身默认行为。
std::string SliderStep(float minValue, float maxValue)
{
    const float span = maxValue - minValue;
    if (!std::isfinite(span) || span <= 0.0f) return std::string();
    return " step=\"" + Number(span / SliderStepDivisions) + "\"";
}
// 行首图标与文本同属按钮内容，点击靶面保持一个；图标尺寸与对齐由 RCSS 决定。
std::string RowContent(const std::string& icon, const std::string& label)
{
    if (icon.empty()) return Text(label);
    return "<img class=\"row-icon\" src=\"" + Text(icon) + "\"/><span>" + Text(label) + "</span>";
}
} // namespace

std::string ActionAttributes(const WidgetAction& action)
{
    return " data-action=\"" + Text(action.name) + "\" data-index=\"" +
        std::to_string(action.index) + "\" data-component=\"" + std::to_string(action.component) + "\"";
}

std::string Button(const std::string& id, const WidgetAction& action,
    const std::string& label, ButtonStyle style, bool selected, const std::string& icon)
{
    const char* styleClass = "";
    switch (style)
    {
    case ButtonStyle::Command: break;
    case ButtonStyle::ListItem: styleClass = "list-item"; break;
    case ButtonStyle::TabClose: styleClass = "tab-close"; break;
    case ButtonStyle::TreeToggle: styleClass = "tree-toggle"; break;
    }
    const std::string identity = id.empty() ? std::string() : " id=\"" + Text(id) + "\"";
    return "<button" + identity + ActionAttributes(action) + " class=\"" +
        styleClass + (selected ? " selected" : "") + "\">" + RowContent(icon, label) + "</button>";
}

std::string Input(const std::string& id, const WidgetAction& action, const std::string& value)
{
    return "<input type=\"text\" id=\"" + Text(id) + "\"" + ActionAttributes(action) +
        " value=\"" + Text(value) + "\"/>";
}

std::string Slider(const std::string& id, const WidgetAction& action,
    const std::string& value, float minValue, float maxValue)
{
    // min/max 必须有限且有序：RmlUi 在 min==max 时只回落到 min，
    // 而 NaN 会让滑块位置与读数都失去意义，宿主负责在解析范围时就挡住这类输入。
    return "<input type=\"range\" id=\"" + Text(id) + "\"" + ActionAttributes(action) +
        " min=\"" + Number(minValue) + "\" max=\"" + Number(maxValue) + "\"" +
        SliderStep(minValue, maxValue) + " value=\"" + Text(value) + "\"/>";
}

std::string Option(const std::string& value, const std::string& label)
{
    return "<option value=\"" + Text(value) + "\">" + Text(label) + "</option>";
}

std::string Select(const std::string& id, const WidgetAction& action, const std::string& optionsRml)
{
    return "<select id=\"" + Text(id) + "\"" + ActionAttributes(action) + ">" + optionsRml + "</select>";
}
} // namespace VL::UiWidgets
