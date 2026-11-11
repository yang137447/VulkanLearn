#ifndef VL_MATERIAL_FUNCTION_TERRAIN_LAYER_VT_GLSL
#define VL_MATERIAL_FUNCTION_TERRAIN_LAYER_VT_GLSL

#include "../engine/materialContext.glsl"
#include "mf_normal.glsl"
#include "terrainLayerVTMath.glsl"

// L3 层纹理 VT：top-4 层号仍来自覆盖页的 ID/权重图，但像素改为
//   layerId -> catalog page 参数 -> 每 mip 页表 -> 物理页 -> atlas 采样。
// catalog 是 M_terrain 的 layerTable（256x4 RGBA32F）：
//   row1 = noise/blendEdge/uvScale/uvRotation（L2 之前已有）
//   row2 = pagesPerSide(mip0)/pageTableBase/sourceSet/flags
//   row3 = avgColor.rgb/reserved
// 所有页表/atlas 都是 L2 在 World 级创建的共享资源。L4a 起页驻留由覆盖页
// feedback 逐帧驱动，本文件在选中 mip 缺页时回退到更粗的 mip。
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

        // 层 UV/mip/页号一律走 terrainLayerVTMath.glsl：feedback pass 用
        // 同一份实现，页号才能与真正采样的页一一对应。
        // 虚拟页 = layerId * stride(mip) + pageY * side + pageX；页表大小正好是
        // stride(mip) * 256（最大层数），因此 texel 总数 / 256 就是 CPU stride。
        const vec2 layerUv =
            TerrainLayerUvForSlot(context.texCoord, u_uvScale, uvRow);
        const int autoMip = TerrainLayerMipForUv(
            layerUv,
            pagesPerSide,
            maxMip,
            u_layerPageResolution,
            vec2(1.0));
        // L4e：`u_layerForceMip` 是 console 的调试覆盖（-1 = 自动）。
        // targetMip 是"想采哪一级"，usedMip 是缺页回退后实际采到的级别。
        const int targetMip =
            TerrainLayerTargetMip(autoMip, u_layerForceMip, maxMip);
        int usedMip = targetMip;
        TerrainLayerVTAddress address =
            TerrainLayerAddressAtMip(layerUv, pagesPerSide, usedMip);
        ivec2 tableSize = TerrainLayerPageTableSize(usedMip);
        int perLayerStride =
            (tableSize.x * tableSize.y) / TERRAIN_VT_MAX_LAYERS;
        int virtualPage = layer * perLayerStride + address.pageIndex;
        vec4 entry = TerrainLayerPageTableFetch(
            usedMip,
            ivec2(virtualPage % tableSize.x, virtualPage / tableSize.x));
        // L4a：层页按 mip 逐帧收敛，更细的 mip 可能尚未驻留。此时回退到
        // 更粗的 mip（同一层的页表项有效），而不是直接把整页显示成 avgColor。
        for (int coarser = targetMip + 1;
             coarser <= maxMip && entry.b < 0.5;
             ++coarser)
        {
            usedMip = coarser;
            address =
                TerrainLayerAddressAtMip(layerUv, pagesPerSide, usedMip);
            tableSize = TerrainLayerPageTableSize(usedMip);
            perLayerStride =
                (tableSize.x * tableSize.y) / TERRAIN_VT_MAX_LAYERS;
            virtualPage =
                layer * perLayerStride + address.pageIndex;
            entry = TerrainLayerPageTableFetch(
                usedMip,
                ivec2(virtualPage % tableSize.x, virtualPage / tableSize.x));
        }
        if (slot == 0)
            result.debugMip = float(usedMip);

        vec4 albedoLayer;
        vec4 pbrLayer;
        vec3 normalLayer;
        // L4e：向粗回退时混向 avgColor / 中性 pbr / 几何法线，避免页级 pop。
        // 完整驻留（gap=0）时系数为 0，与之前的采样路径逐字节一致。
        // normal 的混合方向是"丢掉粗 mip 不存在的细节"，回到几何法线。
        const float coarseBlend =
            TerrainLayerFallbackBlend(targetMip, usedMip);
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
                address.localUv,
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
        albedoLayer.rgb =
            mix(albedoLayer.rgb, colorRow.rgb, coarseBlend);
        // pbr 约定 x=roughness / y=metallic / z=AO；缺页的中性值是
        // roughness 0.9、metallic 0、AO 1，与 L3 的无效页一致。
        pbrLayer = mix(pbrLayer, vec4(0.9, 0.0, 1.0, 0.0), coarseBlend);
        const vec3 blendedNormal =
            mix(normalLayer, worldNormal, coarseBlend);
        normalLayer = dot(blendedNormal, blendedNormal) > 1.0e-8
            ? normalize(blendedNormal) : worldNormal;

        const float weight = topWeights[slot];
        result.albedo += albedoLayer * weight;
        result.pbr += pbrLayer * weight;
        result.normalWorld += normalLayer * weight;
    }
    return result;
}

#endif
