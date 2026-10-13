#pragma once

#include <string>

namespace VL::UiWidgets
{
// 文本与属性值统一转义；调用者不得把资产名称当作可执行 RML。
std::string Text(const std::string& value);
std::string Heading(const std::string& label);
std::string Description(const std::string& text);
} // namespace VL::UiWidgets
