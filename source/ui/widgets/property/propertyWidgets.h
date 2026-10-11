#pragma once

#include <array>
#include <vector>
#include "ui/widgets/input/inputWidgets.h"

namespace VL::UiWidgets
{
// 通用属性组件只接收标签、控件内容与 Reset 意图，不认识 MI schema 或 override。
std::string PropertyRow(const std::string& label, const std::string& controlRml,
    const std::string& resetId, const WidgetAction& reset);
std::string PropertyGroup(const std::string& label, const std::string& controlsRml,
    const std::string& resetId, const WidgetAction& reset, const std::string& description = {});
// 只读属性行：没有控件也没有 Reset，只展示业务已经算好的文本（同样经过转义）。
std::string PropertyInfo(const std::string& label, const std::string& value);

// 折叠分组的展开状态属于宿主（会话 UI 状态，不是文档状态）；组件只生成当前状态的 RML
// 和切换地址。切换由宿主在既有元素上改 class，不重建分组 DOM，否则会丢焦点与滚动位置。
std::string CollapsibleSection(const std::string& id, const std::string& key,
    const std::string& title, const std::string& childrenRml,
    const WidgetAction& toggle, bool expanded);

// 分量值保持字符串，允许焦点控件处于未完成输入；数值解释由具体业务的输入边界负责。
struct NumericComponent
{
    std::string id;
    std::string label;
    std::string value;
    WidgetAction change;
    // 可选范围：带上 min/max 才生成本分量的滑条。范围含义与来源由业务宿主决定，
    // 组件只负责把它交给范围控件；文本输入始终保留，精确输入不依赖滑条。
    bool hasRange = false;
    float minValue = 0.0f;
    float maxValue = 0.0f;
};
std::string NumericComponents(const std::vector<NumericComponent>& components);

// 线性 RGBA 的显示侧读数：引擎按线性存色，编辑器面向 sRGB 显示。
struct ColorReadout
{
    std::string hex;   // "#RRGGBBAA"，alpha 不做 gamma 变换
    std::string rgb;   // "rgb(r,g,b)"，色块底色
};
ColorReadout MakeColorReadout(const std::array<float, 4>& linearRgba);
// 色块 + 读数由宿主逐帧刷新：色块写 background-color，读数写 markup，两者都走既有缓存。
std::string ColorPreview(const std::string& swatchId, const std::string& hexId,
    const ColorReadout& readout);
} // namespace VL::UiWidgets
