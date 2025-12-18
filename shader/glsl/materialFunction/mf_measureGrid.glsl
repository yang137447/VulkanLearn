#include "../common/function.glsl"

float CalculateMeasureGridMask(vec3 pixelPos_WS, vec3 normal_WS, float gridSize)
{
    const float lineWidth = 0.01;
    const float contrast = 0.5;

    // 计算网格
    vec3 grid = fract(pixelPos_WS / gridSize) - 0.5;
    grid = abs(grid - 0.5);
    grid = vec3(
        grid.x < lineWidth ? 1.0 : 0.0,
        grid.y < lineWidth ? 1.0 : 0.0,
        grid.z < lineWidth ? 1.0 : 0.0);

    float gridMaskFB = max(grid.x, grid.y);
    float gridMaskRL = max(grid.z, grid.y);
    float gridMaskTB = max(grid.x, grid.z);

    // 混合FB和RL
    float gridMask = mix(gridMaskFB, gridMaskRL, cheapContrast(normal_WS.x, contrast));
    // 混合TB
    gridMask = mix(gridMask, gridMaskTB, cheapContrast(normal_WS.y, contrast));
    
    return clamp(gridMask, 0.0, 1.0);
}