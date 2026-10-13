#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

namespace VL::UiWidgets
{
// 值语义交互地址，仅供业务适配器路由；组件不解释 action、不保存命令或回调。
struct WidgetAction
{
    std::string name;
    size_t index = 0;
    uint32_t component = 0;
};

enum class ButtonStyle { Command, ListItem, TabClose, TreeToggle };

std::string ActionAttributes(const WidgetAction& action);
// icon 是资产相对路径（例如 "icons/mesh.png"），空表示纯文本按钮；
// icon 与 label 一样只作为文本转义后写入，不接受调用方提供的裸 RML。
std::string Button(const std::string& id, const WidgetAction& action,
    const std::string& label, ButtonStyle style = ButtonStyle::Command, bool selected = false,
    const std::string& icon = {});
std::string Input(const std::string& id, const WidgetAction& action, const std::string& value);
// 范围控件（input type="range"）只承载 min/max 与拖动粒度；范围从哪来、代表什么属于业务宿主。
// 值仍以字符串传入，精确输入由同行的文本输入负责，滑条不需要业务参数。
std::string Slider(const std::string& id, const WidgetAction& action,
    const std::string& value, float minValue, float maxValue);
std::string Option(const std::string& value, const std::string& label);
// optionsRml 只允许由 Option() 生成，不能传入用户或资产提供的裸 RML。
std::string Select(const std::string& id, const WidgetAction& action, const std::string& optionsRml);
} // namespace VL::UiWidgets
