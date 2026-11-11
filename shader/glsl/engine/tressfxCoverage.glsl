#ifndef VL_TRESSFX_COVERAGE_GLSL
#define VL_TRESSFX_COVERAGE_GLSL

// VL 静态预览的随机裁剪，不是源引擎的 MSAA/A2C 或 OIT 实现。
// provoking vertex 的点索引为每个三角形提供固定种子，交叠层不会共用整屏阈值。
// 不加入帧号：静止画面保持稳定；缺少时域累积时仍会看到空间噪声。
float TressfxCoverageThreshold(uvec2 pixel, uint seed)
{
    uint hash = pixel.x * 0x9e3779b9u ^ pixel.y * 0x85ebca6bu ^ seed * 0xc2b2ae35u;
    hash ^= hash >> 16;
    hash *= 0x7feb352du;
    hash ^= hash >> 15;
    hash *= 0x846ca68bu;
    hash ^= hash >> 16;
    // 阈值严格在 (0,1) 内，确保全透明必裁除、全覆盖必保留。
    return (float(hash & 0x007fffffu) + 0.5) / 8388608.0;
}

#endif
