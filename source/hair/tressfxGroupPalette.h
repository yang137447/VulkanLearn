#pragma once

#include <cstddef>
#include <cstdint>
#include <string_view>

namespace VL::Hair
{

// TF 分组颜色不写资产：同一名称在所有会话、编辑器和 GPU 侧得到同一颜色。
struct TressfxGroupColor
{
    uint8_t r = 0;
    uint8_t g = 0;
    uint8_t b = 0;
};

inline bool operator==(const TressfxGroupColor& left, const TressfxGroupColor& right)
{
    return left.r == right.r && left.g == right.g && left.b == right.b;
}

inline bool operator!=(const TressfxGroupColor& left, const TressfxGroupColor& right)
{
    return !(left == right);
}

// 固定 12 色：编辑器列表、视口图例和 GPU groupColors 共用这一份表，
// 避免两边各自哈希出不同颜色。
constexpr TressfxGroupColor TressfxGroupPalette[] = {
    {230, 90, 90}, {235, 150, 70}, {225, 205, 80}, {130, 205, 90},
    {80, 200, 170}, {85, 175, 230}, {110, 130, 235}, {165, 120, 235},
    {225, 110, 200}, {200, 140, 110}, {150, 160, 175}, {90, 200, 225}};

constexpr size_t TressfxGroupPaletteSize =
    sizeof(TressfxGroupPalette) / sizeof(TressfxGroupPalette[0]);

inline TressfxGroupColor TressfxGroupColorForName(std::string_view name)
{
    // FNV-1a 32-bit；混合高位后取模，低比特相关性不会让相近名称挤在一起。
    uint32_t hash = 2166136261u;
    for (char ch : name)
    {
        hash ^= static_cast<unsigned char>(ch);
        hash *= 16777619u;
    }
    const uint32_t mixed = hash ^ (hash >> 16);
    return TressfxGroupPalette[mixed % TressfxGroupPaletteSize];
}

// 隐式组 0 的固定中性灰；UI 的默认色块与 GPU groupColors[0] 都使用它。
inline TressfxGroupColor TressfxDefaultGroupColor()
{
    return {120, 128, 136};
}

} // namespace VL::Hair
