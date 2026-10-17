#pragma once

#include "script/scriptTypes.h"

namespace VL
{
inline std::string BuildGameplayHudText(const ScriptRuntimeView& scripts)
{
    if (!scripts.available || scripts.state == ScriptPlayState::Editing) return {};
    std::string text;
    for (const auto& line : scripts.hudLines)
    {
        if (!text.empty()) text += '\n';
        text += line.text;
    }
    return text;
}
}
