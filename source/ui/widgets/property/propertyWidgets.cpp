#include "ui/widgets/property/propertyWidgets.h"

#include <iomanip>
#include <sstream>

#include "ui/uiColorTransform.h"
#include "ui/widgets/text/textWidgets.h"
#include "ui/widgets/layout/layoutWidgets.h"

namespace VL::UiWidgets
{
namespace
{
std::string HexByte(unsigned int value)
{
    std::ostringstream stream;
    stream << std::uppercase << std::hex << std::setw(2) << std::setfill('0') << value;
    return stream.str();
}
}

std::string PropertyRow(const std::string& label, const std::string& controlRml,
    const std::string& resetId, const WidgetAction& reset)
{
    return "<div class=\"property-row\"><label>" + Text(label) + "</label>" +
        controlRml + Button(resetId, reset, "Reset") + "</div>";
}

std::string PropertyControlRow(const std::string& label, const std::string& controlRml)
{
    return "<div class=\"property-row\"><label>" + Text(label) + "</label>" +
        controlRml + "</div>";
}

std::string PropertyGroup(const std::string& label, const std::string& controlsRml,
    const std::string& resetId, const WidgetAction& reset, const std::string& description)
{
    return "<div class=\"parameter\"><div class=\"property-heading\"><label>" + Text(label) +
        "</label>" + Button(resetId, reset, "Reset") + "</div>" + controlsRml +
        Description(description) + "</div>";
}

std::string PropertyInfo(const std::string& label, const std::string& value,
    const std::string& valueId)
{
    const std::string identity = valueId.empty() ? std::string() : " id=\"" + Text(valueId) + "\"";
    return "<div class=\"property-info\"><label>" + Text(label) + "</label><span" + identity +
        ">" + Text(value) + "</span></div>";
}

std::string CollapsibleSection(const std::string& id, const std::string& key,
    const std::string& title, const std::string& childrenRml,
    const WidgetAction& toggle, bool expanded)
{
    // 展开与折叠标记同时存在，宿主切换时只改根节点 class，不重写 markup：
    // 重写会销毁分组内的焦点与滚动位置，而这个面板每帧都从冻结快照刷新。
    return "<div id=\"" + Text(id) + "\" class=\"details-section" +
        (expanded ? "" : " collapsed") + "\">" +
        "<button id=\"" + Text(id) + "-title\" class=\"details-section-title\"" +
        ActionAttributes(toggle) + " data-section=\"" + Text(key) + "\">" +
        "<span class=\"section-marker-expanded\">-</span>" +
        "<span class=\"section-marker-collapsed\">+</span>" + Text(title) + "</button>" +
        "<div class=\"details-section-body\">" + childrenRml + "</div></div>";
}

std::string NumericComponents(const std::vector<NumericComponent>& components)
{
    std::string markup;
    for (const auto& component : components)
    {
        markup += "<div class=\"component\"><label>" + Text(component.label) + "</label>" +
            Input(component.id, component.change, component.value);
        // 精确输入始终保留：滑条只提供范围语义下的快速调整，不替代文本输入。
        if (component.hasRange)
        {
            markup += Slider(component.id + "-slider", component.change, component.value,
                component.minValue, component.maxValue);
        }
        markup += "</div>";
    }
    return HorizontalBox(markup);
}

ColorReadout MakeColorReadout(const std::array<float, 4>& linearRgba)
{
    // 线性存储 → sRGB 显示；alpha 是透明度而不是颜色，不参与 gamma 变换。
    const unsigned int red = VL::UiSrgbHexByte(VL::UiLinearToSrgb(linearRgba[0]));
    const unsigned int green = VL::UiSrgbHexByte(VL::UiLinearToSrgb(linearRgba[1]));
    const unsigned int blue = VL::UiSrgbHexByte(VL::UiLinearToSrgb(linearRgba[2]));
    const unsigned int alpha = VL::UiSrgbHexByte(linearRgba[3]);
    ColorReadout readout;
    readout.hex = "#" + HexByte(red) + HexByte(green) + HexByte(blue) + HexByte(alpha);
    // 色块用不透明 rgb()：色块本身不需要半透明，而 RmlUi 6.2 的 rgba() 小数 alpha
    // 会被 PropertyParserColour 的整数路径截断为 0（静默变全透明）。alpha 只由十六进制
    // 读数表达，避免色块与读数不一致。详见 documents/contracts/ui-component-system.md
    // 的 RCSS 颜色格式约定，以及 documents/rationale/pitfalls.md 的 [rcss-alpha] 条。
    readout.rgb = "rgb(" + std::to_string(red) + "," + std::to_string(green) + "," +
        std::to_string(blue) + ")";
    return readout;
}

std::string ColorPreview(const std::string& swatchId, const std::string& hexId,
    const ColorReadout& readout)
{
    return "<div class=\"color-preview\"><span class=\"color-swatch\" id=\"" + Text(swatchId) +
        "\"></span><span class=\"color-hex\" id=\"" + Text(hexId) + "\">" +
        Text(readout.hex) + "</span></div>";
}
} // namespace VL::UiWidgets
