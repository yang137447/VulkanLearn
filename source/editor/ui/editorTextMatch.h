#pragma once

#include <string>

namespace VL::EditorUi
{

// 编辑器搜索框的统一折叠规则：只折叠 ASCII 大小写，保留 UTF-8 多字节名称。
// Content Browser 与 Scene Outliner 共用这一份实现，避免出现第二个折叠规则。
inline std::string FoldAsciiCase(std::string text)
{
    for (char& character : text)
        if (character >= 'A' && character <= 'Z') character += 'a' - 'A';
    return text;
}

} // namespace VL::EditorUi
