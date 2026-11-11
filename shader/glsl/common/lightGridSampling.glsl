#ifndef VL_LIGHT_GRID_SAMPLING_GLSL
#define VL_LIGHT_GRID_SAMPLING_GLSL

// 屏幕空间 tile 光照列表的只读视图。
//
// 调用方必须在 include 之前定义三个 binding 宏，并以固定实例名声明：
//   VL_LIGHT_GRID_BINDING        -> lightGrid
//   VL_LIGHT_INDEX_BINDING       -> lightIndices
//   VL_LIGHT_GRID_PARAMS_BINDING -> lightGridParams
// 材质路径用 5/6/7，粒子路径用 10/11/12；遍历逻辑只保留这一份，
// 避免同一套 tile 语义出现两份实现。
//
// 关闭时（enabled=0，例如缩略图/自定义图没有聚类 pass）调用方必须回退到
// UBOLight 的三段连续循环，行为与引入聚类前完全一致。

layout(std430, set = 0, binding = VL_LIGHT_GRID_BINDING) readonly buffer LightGridBuffer
{
    // x = 发生溢出的 tile 数，y = 被丢弃的灯索引总数。
    uvec4 header;
    // x = count，y = overflow，z/w reserved。固定 stride = maxLightsPerTile。
    uvec4 tiles[];
} lightGrid;

layout(std430, set = 0, binding = VL_LIGHT_INDEX_BINDING) readonly buffer LightIndexBuffer
{
    uint indices[];
} lightIndices;

layout(std140, set = 0, binding = VL_LIGHT_GRID_PARAMS_BINDING) uniform LightGridParamsBlock
{
    // x = tileSize，y = tilesX，z = tilesY，w = maxLightsPerTile
    uvec4 tileParams;
    // x = enabled，y = renderWidth，z = renderHeight，w = reserved
    uvec4 extentParams;
    // x = pointLightOffset，y = pointLightCount，z = spotLightOffset，w = spotLightCount
    uvec4 lightRanges;
    uvec4 reservedParams;
} lightGridParams;

bool LightGridEnabled()
{
    return
        lightGridParams.extentParams.x != 0u &&
        lightGridParams.tileParams.x != 0u &&
        lightGridParams.tileParams.y != 0u &&
        lightGridParams.tileParams.z != 0u &&
        lightGridParams.tileParams.w != 0u;
}

uint LightGridTileCapacity()
{
    return max(lightGridParams.tileParams.w, 1u);
}

// gl_FragCoord.xy 与 culling compute 使用同一套像素坐标（左上原点）。
uint LightGridTileIndexFromFragCoord(vec2 fragCoord)
{
    uint tileSize = max(lightGridParams.tileParams.x, 1u);
    uint tilesX = max(lightGridParams.tileParams.y, 1u);
    uint tilesY = max(lightGridParams.tileParams.z, 1u);
    uint tileX = min(
        uint(max(fragCoord.x, 0.0)) / tileSize,
        tilesX - 1u);
    uint tileY = min(
        uint(max(fragCoord.y, 0.0)) / tileSize,
        tilesY - 1u);
    return tileY * tilesX + tileX;
}

uint LightGridVisibleLightCount(uint tileIndex)
{
    uint tileCount =
        max(lightGridParams.tileParams.y, 1u) *
        max(lightGridParams.tileParams.z, 1u);
    if (tileIndex >= tileCount)
    {
        return 0u;
    }
    return min(
        lightGrid.tiles[tileIndex].x,
        LightGridTileCapacity());
}

uint LightGridLightIndex(uint tileIndex, uint slot)
{
    return lightIndices.indices[
        tileIndex * LightGridTileCapacity() + slot];
}

#endif
