#pragma once

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <vector>

namespace VL
{

inline float UiSrgbToLinear(float value)
{
    return value <= 0.04045f ? value / 12.92f
        : std::pow((value + 0.055f) / 1.055f, 2.4f);
}

// 引擎按线性存色，编辑器面向 sRGB 显示（调色板语义）。这里使用扩展公式且不截断：
// HDR 线性值（emissive 之类）必须原样越过 1.0，截断只发生在无法表达 HDR 的 8bit 读数处。
inline float UiLinearToSrgb(float value)
{
    return value <= 0.0031308f ? value * 12.92f
        : 1.055f * std::pow(value, 1.0f / 2.4f) - 0.055f;
}

// 色块与十六进制文本只能表达 [0,1]，这是唯一的截断点；alpha 不做 gamma 变换，
// 调用者自行决定是否把 alpha 传进来。
inline unsigned int UiSrgbHexByte(float value)
{
    return static_cast<unsigned int>(std::lround(std::clamp(value, 0.0f, 1.0f) * 255.0f));
}

// RmlUi 的颜色在 sRGB 中预乘；先还原非关联颜色，再在线性空间预乘。
// 浮点顶点避免暗色背景在 8bit 线性存储中严重量化；alpha 不做 gamma 变换。
inline std::array<float, 4> UiPremultipliedSrgbToLinear(
    uint8_t red, uint8_t green, uint8_t blue, uint8_t alpha)
{
    if (alpha == 0) return {};
    const float opacity = static_cast<float>(alpha) / 255.0f;
    return {UiSrgbToLinear(static_cast<float>(red) / alpha) * opacity,
        UiSrgbToLinear(static_cast<float>(green) / alpha) * opacity,
        UiSrgbToLinear(static_cast<float>(blue) / alpha) * opacity, opacity};
}

// 图片解码返回 straight sRGB；RmlUi 生成的 atlas 已预乘，不能再次乘 alpha。
// 两条入口均只在纹理注册边界转换，RT 继续采样现有 UNORM 纹理。
inline void ConvertUiPixelsToLinear(std::vector<uint8_t>& pixels, bool premultiplied)
{
    for (size_t offset = 0; offset + 3 < pixels.size(); offset += 4)
    {
        const uint8_t alpha = pixels[offset + 3];
        for (size_t channel = 0; channel < 3; ++channel)
        {
            const float color = alpha == 0 ? 0.0f : UiSrgbToLinear(
                static_cast<float>(pixels[offset + channel]) /
                (premultiplied ? alpha : 255.0f)) * alpha;
            pixels[offset + channel] = static_cast<uint8_t>(std::lround(color));
        }
    }
}

} // namespace VL
