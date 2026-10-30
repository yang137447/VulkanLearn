#ifndef VL_MATERIAL_FUNCTION_TERRAIN_LAYER_VT_GLSL
#define VL_MATERIAL_FUNCTION_TERRAIN_LAYER_VT_GLSL

#include "../engine/materialContext.glsl"
#include "mf_normal.glsl"

// L3 层纹理 VT：top-4 层号仍来自覆盖页的 ID/权重图，但像素改为
//   layerId -> catalog page 参数 -> 每 mip 页表 -> 物理页 -> atlas 采样。
// catalog 是 M_terrain 的 layerTable（256x4 RGBA32F）：
//   row1 = noise/blendEdge/uvScale/uvRotation（L2 之前已有）
//   row2 = pagesPerSide(mip0)/pageTableBase/sourceSet/flags
//   row3 = avgColor.rgb/reserved
// 所有页表/atlas 都是 L2 在 World 级创建的共享资源。L4a 起页驻留由覆盖页
// feedback 逐帧驱动，本文件在选中 mip 缺页时回退到更粗的 mip。
const int TERRAIN_VT_MAX_LAYERS = 256;
const int TERRAIN_VT_MAX_MIP = 4;

struct TerrainLayerVTResult
{
    vec4 albedo;
    vec4 pbr;
    vec3 normalWorld;
    // 仅用于 debug view 4/5；取 top-4 第一个槽位。
    float debugPhysicalPage;
    float debugMip;
};

vec4 TerrainLayerCatalogRow(int layer, int row)
{
    // nearest + 无 mip：texel 中心严格落在指定 catalog 行内。
    return texture(
        layerTable,
        vec2(
            (float(layer) + 0.5) / float(TERRAIN_VT_MAX_LAYERS),
            (float(row) + 0.5) / 4.0));
}

// mip 0..4 各自一张页表。这里显式 if/else，避免动态索引 sampler 数组；
// Vulkan/GLSL 不保证 sampler 数组支持非 uniform 动态索引。
vec4 TerrainLayerPageTableFetch(int mip, ivec2 coord)
{
    if (mip <= 0)
        return texelFetch(layerPageTable0, coord, 0);
    if (mip == 1)
        return texelFetch(layerPageTable1, coord, 0);
    if (mip == 2)
        return texelFetch(layerPageTable2, coord, 0);
    if (mip == 3)
        return texelFetch(layerPageTable3, coord, 0);
    return texelFetch(layerPageTable4, coord, 0);
}

ivec2 TerrainLayerPageTableSize(int mip)
{
    if (mip <= 0)
        return textureSize(layerPageTable0, 0);
    if (mip == 1)
        return textureSize(layerPageTable1, 0);
    if (mip == 2)
        return textureSize(layerPageTable2, 0);
    if (mip == 3)
        return textureSize(layerPageTable3, 0);
    return textureSize(layerPageTable4, 0);
}

// catalog 的 pagesPerSide 是 mip0 的每边页数（512 -> 4、1K -> 8、2K -> 16）。
// 尾页是 128²，因此最大 mip = log2(pagesPerSide)，例如 1K 为 mip0..3。
int TerrainLayerMaxMipForPagesPerSide(int pagesPerSide)
{
    int maxMip = 0;
    int side = pagesPerSide;
    while (side > 1 && maxMip < TERRAIN_VT_MAX_MIP)
    {
        side >>= 1;
        ++maxMip;
    }
    return maxMip;
}

int TerrainLayerPagesPerSideAtMip(int pagesPerSide, int mip)
{
    return max(1, pagesPerSide >> mip);
}

TerrainLayerVTResult AccumulateTerrainLayerVT(
    in MaterialFunctionContext context,
    ivec4 topIds,
    vec4 topWeights,
    vec3 worldNormal)
{
    TerrainLayerVTResult result;
    result.albedo = vec4(0.0);
    result.pbr = vec4(0.0);
    result.normalWorld = vec3(0.0);
    result.debugPhysicalPage = 0.0;
    result.debugMip = 0.0;

    for (int slot = 0; slot < 4; ++slot)
    {
        const int layer = topIds[slot];
        const vec4 uvRow = TerrainLayerCatalogRow(layer, 1);
        const vec4 pageRow = TerrainLayerCatalogRow(layer, 2);
        const vec4 colorRow = TerrainLayerCatalogRow(layer, 3);
        const int pagesPerSide = max(1, int(pageRow.x + 0.5));
        const int maxMip = min(
            TerrainLayerMaxMipForPagesPerSide(pagesPerSide),
            int(u_layerMaxMip + 0.5));

        // 层 UV 与 mode 0 完全一致：uvScale/uvRotation 来自 catalog row1。
        // 导数必须在 fract 之前取，mip 依据层自身的纹素密度选择。
        const float angle = uvRow.w;
        const mat2 rotation =
            mat2(cos(angle), sin(angle), -sin(angle), cos(angle));
        const vec2 layerUv =
            rotation * (context.texCoord * u_uvScale * uvRow.z);
        const vec2 layerDdx = dFdx(layerUv);
        const vec2 layerDdy = dFdy(layerUv);
        const float sourceResolution =
            float(pagesPerSide) * u_layerPageResolution;
        const float texelsPerPixel =
            max(length(layerDdx), length(layerDdy)) * sourceResolution;
        const float lod = log2(max(texelsPerPixel, 1.0e-8));
        const int mip = clamp(
            int(floor(lod + 0.5)),
            0,
            maxMip);
        // 层纹理是 repeat 平铺：先 fract 到 [0,1)，再按该 mip 的页数切页。
        // side 与 CPU 的 TerrainLayerPagesPerSide(sourceResolution, mip) 一致。
        // 虚拟页 = layerId * stride(mip) + pageY * side + pageX；页表大小正好是
        // stride(mip) * 256（最大层数），因此 texel 总数 / 256 就是 CPU stride。
        int usedMip = mip;
        int side = TerrainLayerPagesPerSideAtMip(pagesPerSide, usedMip);
        vec2 scaledUv = fract(layerUv) * float(side);
        ivec2 pageXY = clamp(
            ivec2(floor(scaledUv)),
            ivec2(0),
            ivec2(side - 1));
        vec2 localUv = fract(scaledUv);
        ivec2 tableSize = TerrainLayerPageTableSize(usedMip);
        int perLayerStride =
            (tableSize.x * tableSize.y) / TERRAIN_VT_MAX_LAYERS;
        int virtualPage =
            layer * perLayerStride + pageXY.y * side + pageXY.x;
        vec4 entry = TerrainLayerPageTableFetch(
            usedMip,
            ivec2(virtualPage % tableSize.x, virtualPage / tableSize.x));
        // L4a：层页按 mip 逐帧收敛，更细的 mip 可能尚未驻留。此时回退到
        // 更粗的 mip（同一层的页表项有效），而不是直接把整页显示成 avgColor。
        for (int coarser = mip + 1;
             coarser <= maxMip && entry.b < 0.5;
             ++coarser)
        {
            usedMip = coarser;
            side = TerrainLayerPagesPerSideAtMip(pagesPerSide, usedMip);
            scaledUv = fract(layerUv) * float(side);
            pageXY = clamp(
                ivec2(floor(scaledUv)),
                ivec2(0),
                ivec2(side - 1));
            localUv = fract(scaledUv);
            tableSize = TerrainLayerPageTableSize(usedMip);
            perLayerStride =
                (tableSize.x * tableSize.y) / TERRAIN_VT_MAX_LAYERS;
            virtualPage =
                layer * perLayerStride + pageXY.y * side + pageXY.x;
            entry = TerrainLayerPageTableFetch(
                usedMip,
                ivec2(virtualPage % tableSize.x, virtualPage / tableSize.x));
        }
        if (slot == 0)
            result.debugMip = float(usedMip);

        vec4 albedoLayer;
        vec4 pbrLayer;
        vec3 normalLayer;
        if (entry.b < 0.5)
        {
            // 缺页必须用 catalog 的平均色，不能回落到 layer 0。
            albedoLayer = vec4(colorRow.rgb, 1.0);
            pbrLayer = vec4(0.9, 0.0, 1.0, 0.0);
            normalLayer = worldNormal;
        }
        else
        {
            const int physicalPage = int(entry.r + 0.5);
            if (slot == 0)
                result.debugPhysicalPage = float(physicalPage);
            const int tilesPerSide = int(u_layerTilesPerAtlasSide + 0.5);
            const int pagesPerSlice = int(u_layerPagesPerAtlasSlice + 0.5);
            const int slice = physicalPage / pagesPerSlice;
            const int tileIndex = physicalPage - slice * pagesPerSlice;
            const int tileX = tileIndex % tilesPerSide;
            const int tileY = tileIndex / tilesPerSide;
            // 半 texel 内缩，防止双线性 tap 跨到相邻物理页。
            const float pageResolution = u_layerPageResolution;
            const vec2 clampedLocal = clamp(
                localUv,
                vec2(0.5 / pageResolution),
                vec2(1.0 - 0.5 / pageResolution));
            const vec2 atlasUv =
                (vec2(float(tileX), float(tileY)) * pageResolution +
                    clampedLocal * pageResolution) /
                u_layerAtlasResolution;
            const vec3 atlasCoord = vec3(atlasUv, float(slice));
            // 每个 mip 是独立物理页，atlas 本身无 mip；必须显式 LOD 0，
            // 否则隐式导数会在页边界/切片间产生错误采样。
            albedoLayer = textureLod(layerAlbedoAtlas, atlasCoord, 0.0);
            pbrLayer = textureLod(layerPbrAtlas, atlasCoord, 0.0);
            const vec3 normalTS =
                textureLod(layerNormalAtlas, atlasCoord, 0.0).xyz * 2.0 - 1.0;
            normalLayer =
                TransformMaterialNormalToWorld(context, normalTS);
        }

        const float weight = topWeights[slot];
        result.albedo += albedoLayer * weight;
        result.pbr += pbrLayer * weight;
        result.normalWorld += normalLayer * weight;
    }
    return result;
}

#endif
