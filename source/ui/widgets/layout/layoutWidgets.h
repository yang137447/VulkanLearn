#pragma once

#include <string>

namespace VL::UiWidgets
{
// 组合已生成的子组件 RML；不接收未经转义的业务文本、不实现第二套布局引擎。
std::string HorizontalBox(const std::string& childrenRml);
std::string VerticalBox(const std::string& childrenRml);
std::string ScrollBox(const std::string& id, const std::string& childrenRml);
} // namespace VL::UiWidgets
