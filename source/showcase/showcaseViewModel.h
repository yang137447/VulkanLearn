#pragma once

#include <string>

namespace VL
{

// UiSubsystem 只消费这个纯值快照：展示控制器写，RmlUi 绑定读。
// 数值用格式化字符串，避免 float 打印位数在界面上抖动。
struct ShowcaseViewModel
{
    bool visible = false;
    bool panelOpen = false;
    int material = 0;
    std::string name;
    std::string tag;
    std::string baseHex;
    std::string sheenHex;
    std::string f0 = "0.05";
    std::string roughness = "0.50";
    std::string rotation = "0°";
};

} // namespace VL
