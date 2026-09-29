#include "core/renderer.h"

#include <cmath>

namespace VL::Offline
{
Vec3 sampleColorMap(const ColorMap& map, Vec2 uv)
{
    const auto texel = [&](int x, int y)
    {
        if (map.repeat)
        {
            x = (x % map.width + map.width) % map.width;
            y = (y % map.height + map.height) % map.height;
        }
        else
        {
            x = std::clamp(x, 0, map.width - 1);
            y = std::clamp(y, 0, map.height - 1);
        }
        return map.pixels[static_cast<size_t>(y) * map.width + x];
    };
    // 与 Vulkan normalized sampler 的 texel-center 约定一致；UV 的 v=0
    // 对应材质加载时已翻转一次的图像首行，采样阶段不再翻转 UV。
    const double x = uv.x * map.width - 0.5;
    const double y = uv.y * map.height - 0.5;
    if (!map.linear)
        return texel(static_cast<int>(std::floor(x + 0.5)),
                     static_cast<int>(std::floor(y + 0.5)));
    const int ix = static_cast<int>(std::floor(x));
    const int iy = static_cast<int>(std::floor(y));
    const double fx = x - ix, fy = y - iy;
    return (texel(ix, iy) * (1.0 - fx) + texel(ix + 1, iy) * fx) * (1.0 - fy) +
           (texel(ix, iy + 1) * (1.0 - fx) + texel(ix + 1, iy + 1) * fx) * fy;
}
} // namespace VL::Offline
