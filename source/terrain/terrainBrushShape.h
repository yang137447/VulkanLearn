#pragma once

namespace VL
{

// 地形画布使用归一化 UV；Circle 用欧氏距离，Square 用 Chebyshev 距离
// max(|du|,|dv|)。该枚举刻意不依赖 editor/brush，避免地形核心反向依赖编辑器。
enum class TerrainBrushShape
{
    Circle,
    Square,
};

} // namespace VL
