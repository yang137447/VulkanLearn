#include "../common/function.glsl"

float CalculateMeasureGridMask(vec3 pixelPos_WS, vec3 normal_WS, float gridSize)
{
    const float lineWidth = 1.0; // 线宽（像素单位），配合 smoothstep 使用
    const float contrast = 0.5;

    // 计算网格坐标
    vec3 coord = pixelPos_WS / gridSize;
    
    // 使用 fwidth 计算屏幕空间导数，用于抗锯齿
    // fwidth(coord) ≈ |d(coord)/dx| + |d(coord)/dy|
    // 这代表了当前像素覆盖了多少 grid 坐标空间
    vec3 derivatives = fwidth(coord);
    
    // 计算每个维度上，当前像素距离最近网格线的距离（单位：像素）
    // fract(coord - 0.5) - 0.5 产生 [-0.5, 0.5] 的锯齿波，波谷在整数点
    // abs(...) 得到距离整数点的距离 [0, 0.5]
    // 除以 derivatives 将距离转换为像素单位
    vec3 gridDist = abs(fract(coord - 0.5) - 0.5) / derivatives;
    
    // 使用 smoothstep 进行抗锯齿处理
    // 使得线条在 lineWidth 附近产生平滑过渡
    // 距离小于 lineWidth - 1.0 像素时为 1.0 (全白)
    // 距离大于 lineWidth + 1.0 像素时为 0.0 (全黑)
    vec3 gridLine = vec3(
        1.0 - smoothstep(lineWidth - 1.0, lineWidth + 1.0, gridDist.x),
        1.0 - smoothstep(lineWidth - 1.0, lineWidth + 1.0, gridDist.y),
        1.0 - smoothstep(lineWidth - 1.0, lineWidth + 1.0, gridDist.z)
    );

    float gridMaskFB = max(gridLine.x, gridLine.y);
    float gridMaskRL = max(gridLine.z, gridLine.y);
    float gridMaskTB = max(gridLine.x, gridLine.z);

    // 混合FB和RL
    float gridMask = mix(gridMaskFB, gridMaskRL, cheapContrast(normal_WS.x, contrast));
    // 混合TB
    gridMask = mix(gridMask, gridMaskTB, cheapContrast(normal_WS.y, contrast));
    
    return clamp(gridMask, 0.0, 1.0);
}