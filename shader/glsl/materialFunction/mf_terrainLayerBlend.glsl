#ifndef VL_MATERIAL_FUNCTION_TERRAIN_LAYER_BLEND_GLSL
#define VL_MATERIAL_FUNCTION_TERRAIN_LAYER_BLEND_GLSL

#ifndef VL_TERRAIN_TES_STAGE
#define VL_TERRAIN_TES_STAGE 0
#endif

#include "../engine/materialContext.glsl"
#include "../engine/materialInputs.glsl"
#include "../common/commonUbo.glsl"
#include "mf_normal.glsl"
#include "terrainCompositedEvaluation.glsl"
#include "terrainHeightAtlas.glsl"
#include "terrainCoverage.glsl"
#if !VL_TERRAIN_TES_STAGE
#include "mf_terrainCompositedVT.glsl"
#endif

// Phase B：每 texel 的 top-4 由 CPU 烘焙的 ID/权重图给出，shader 不再遍历全局层表。
// layerTable 只保留每层 UV 参数；后续 VT/SSBO 版本沿用同一 ID/权重语义。
const int TERRAIN_MAX_LAYERS = 256;

int TerrainSourceSlice(int globalLayer)
{
    return int(round(texelFetch(terrainLayerSourceMap, ivec2(globalLayer, 0), 0).r));
}

float TerrainSampleHeightAtlasOrSource(vec2 uv, int layer, float requestedLod)
{
    if (requestedLod == 2.0)
        return SampleTerrainHeightAtlas(
            terrainHeightAtlas, terrainHeightLayerMap, uint(layer), uv);
    return textureLod(layerHeight, vec3(uv, float(TerrainSourceSlice(layer))), requestedLod).r;
}

#if !VL_TERRAIN_TES_STAGE
vec4 TerrainLayerExtra(int layer)
{
    // catalog 现在是 256x4：row0 高度/坡度、row1 noise/blendEdge/uvScale/
    // uvRotation、row2 页数/页表基址、row3 平均色。这里只取 row1 的
    // texel 中心 (1 + 0.5) / 4 = 0.375；nearest + 无 mip，不会跨行。
    return texture(
        layerTable,
        vec2((float(layer) + 0.5) / float(TERRAIN_MAX_LAYERS), 0.375));
}
#endif

vec4 TerrainLayerCatalogRowLod0(int layer, int row)
{
    // 非 fragment 阶段没有可靠的隐式导数；catalog 是 LUT，显式 LOD0
    // 与 TerrainLayerCatalogRow 的采样结果一致。
    return textureLod(
        layerTable,
        vec2(
            (float(layer) + 0.5) / float(TERRAIN_MAX_LAYERS),
            (float(row) + 0.5) / 4.0),
        0.0);
}

vec4 TerrainLayerExtraForStage(int layer, bool allowImplicitLod)
{
#if VL_TERRAIN_TES_STAGE
    return TerrainLayerCatalogRowLod0(layer, 1);
#else
    return allowImplicitLod
        ? TerrainLayerExtra(layer)
        : TerrainLayerCatalogRowLod0(layer, 1);
#endif
}

uint TerrainResolveTopLayers(
    in MaterialFunctionContext context,
    in bool allowImplicitLod,
    in bool applyHeightBlend,
    out int ids[4],
    out float weights[4],
    out float coverageWeights[4])
{
    const vec2 coverageUv = clamp(context.texCoord1, vec2(0.0), vec2(1.0));
    ivec4 topIds;
    vec4 topWeights;
    if (u_terrainWeightMapMode > 0.5)
    {
        // 直接采逐层权重数组：权重是连续的，不需要 top-4 ID 的量化与
        // `round`，层边界的抖动/块状来自 ID 图，而不是权重本身。
        // 掩码图带 1 texel margin（grid+3），采样时内缩一个 texel。
        const ivec3 weightSize = textureSize(layerWeightMaps, 0);
        const float weightSpan = float(weightSize.x - 1);
        const vec2 weightUv =
            (vec2(1.0) + context.texCoord * (weightSpan - 2.0)) / weightSpan;
        float w0 = -1.0;
        float w1 = -1.0;
        float w2 = -1.0;
        float w3 = -1.0;
        int i0 = 0;
        int i1 = 0;
        int i2 = 0;
        int i3 = 0;
        for (int layer = 0; layer < weightSize.z; ++layer)
        {
#if VL_TERRAIN_TES_STAGE
            const float weight = textureLod(
                layerWeightMaps,
                vec3(weightUv, float(layer)),
                0.0).r;
#else
            const float weight = texture(
                layerWeightMaps, vec3(weightUv, float(layer))).r;
#endif
            if (weight > w0)
            {
                w3 = w2; i3 = i2;
                w2 = w1; i2 = i1;
                w1 = w0; i1 = i0;
                w0 = weight; i0 = layer;
            }
            else if (weight > w1)
            {
                w3 = w2; i3 = i2;
                w2 = w1; i2 = i1;
                w1 = weight; i1 = layer;
            }
            else if (weight > w2)
            {
                w3 = w2; i3 = i2;
                w2 = weight; i2 = layer;
            }
            else if (weight > w3)
            {
                w3 = weight; i3 = layer;
            }
        }
        topIds = ivec4(i0, i1, i2, i3);
        for (int slot = 0; slot < 4; ++slot)
            topIds[slot] = int(round(texelFetch(
                terrainLayerSourceMap, ivec2(topIds[slot], 1), 0).r));
        topWeights = vec4(
            max(w0, 0.0), max(w1, 0.0), max(w2, 0.0), max(w3, 0.0));
    }
    else
    {
        uvec4 coverageIds;
        uint coverageStatus;
        coverageStatus = TerrainCoverageBilinear(pageTable, layerIds, layerWeights,
            coverageUv, int(u_pageResolution), coverageIds, topWeights);
        if (coverageStatus != 0u)
        {
            for (int slot = 0; slot < 4; ++slot)
            {
                ids[slot] = 0;
                weights[slot] = 0.0;
                coverageWeights[slot] = 0.0;
            }
            return coverageStatus;
        }
        topIds = ivec4(coverageIds);
    }
    float weightSum = topWeights.x + topWeights.y + topWeights.z + topWeights.w;
    if (weightSum < 1.0e-4)
    {
        topWeights = vec4(1.0, 0.0, 0.0, 0.0);
        weightSum = 1.0;
    }
    topWeights /= weightSum;
    ids[0] = topIds.x;
    ids[1] = topIds.y;
    ids[2] = topIds.z;
    ids[3] = topIds.w;
    weights[0] = topWeights.x;
    weights[1] = topWeights.y;
    weights[2] = topWeights.z;
    weights[3] = topWeights.w;
    for (int slot = 0; slot < 4; ++slot)
        coverageWeights[slot] = weights[slot];

    // Height blend: the ID/weight atlas gives a linear cross-fade, which looks
    // muddy where two layers meet. Re-weighting by each layer's 16-bit height
    // (0..1, mean 0.5) keeps the ordering and narrows the transition, the
    // classic "height + inverted weight" scheme. strength 0 is a no-op.
#if !VL_TERRAIN_TES_STAGE
    if (applyHeightBlend && u_heightBlendStrength > 0.0)
    {
        float scores[4];
        for (int slot = 0; slot < 4; ++slot)
        {
            const int layer = ids[slot];
            if (weights[slot] <= 1.0e-5)
            {
                scores[slot] = -1.0e9;
                continue;
            }
            const vec4 extra = TerrainLayerExtra(layer);
            const float angle = extra.w;
            const mat2 rotation =
                mat2(cos(angle), sin(angle), -sin(angle), cos(angle));
            const vec2 uv =
                rotation * (context.texCoord * u_uvScale * extra.z);
            // 显式 LOD 3：这个循环里有按权重 `continue` 的发散控制流，
            // 隐式导数在非一致控制流下未定义。高度只用于比较，不需要
            // 0.25 m 的 texel 精度；3 级 mip（约 2 m）让层过渡更平滑。
            const float height = TerrainSampleHeightAtlasOrSource(uv, layer, 3.0);
            // 换算成米：每层归一化到 0..1 后直接用会让平整沙层和岩石
            // 拥有同样的起伏幅度；乘 displacementM 才能保留相对高差。
            const float displacementM =
                TerrainLayerCatalogRowLod0(layer, 3).w;
            // 高度项必须**有上限**：每层高度都归一化到 0..1，texel 级噪声
            // 足以让高度项压过权重差，出现逐 texel 的换层（近看是锯齿块）。
            // 夹到 ±0.25 后它只能在权重接近时微调过渡，不能改变主次。
            scores[slot] = TerrainCompositedHeightScore(
                height, displacementM, u_heightBlendStrength,
                1.0, weights[slot]);
        }
        TerrainCompositedHeightWeights(scores, weights);
    }
#endif
    return 0u;
}

uint TerrainResolveTopLayers(
    in MaterialFunctionContext context,
    in bool allowImplicitLod,
    in bool applyHeightBlend,
    out int ids[4],
    out float weights[4])
{
    float coverageWeights[4];
    return TerrainResolveTopLayers(
        context, allowImplicitLod, applyHeightBlend, ids, weights, coverageWeights);
}

float TerrainBlendedLayerDisplacementMeters(
    in MaterialFunctionContext context,
    in int ids[4],
    in float weights[4])
{
    float displacement = 0.0;
    for (int slot = 0; slot < 4; ++slot)
    {
        if (weights[slot] <= 0.0)
            continue;
        const int layer = ids[slot];
        const vec4 extra = TerrainLayerExtraForStage(layer, false);
        const vec2 uv =
            context.texCoord * u_uvScale * extra.z * u_detailUvScale;
        const float height = TerrainSampleHeightAtlasOrSource(uv, layer, 2.0);
        const float layerDisplacementM =
            TerrainLayerCatalogRowLod0(layer, 3).w;
        displacement += weights[slot] * (height - 0.5) * layerDisplacementM;
    }
    return displacement;
}

#if !VL_TERRAIN_TES_STAGE
void TerrainWeightedDetail(
    in MaterialFunctionContext context,
    in int ids[4], in float coverageWeights[4],
    vec2 baseUvDx, vec2 baseUvDy,
    out vec3 detailAlbedo, out vec3 detailNormalWorld, out vec3 macroAlbedo)
{
    detailAlbedo = vec3(0);
    detailNormalWorld = vec3(0);
    macroAlbedo = vec3(0);
    for (int slot = 0; slot < 4; ++slot)
    {
        const float weight = coverageWeights[slot];
        if (weight <= 0.0)
            continue;
        const int layer = ids[slot];
        const int slice = TerrainSourceSlice(layer);
        const vec4 extra = TerrainLayerCatalogRowLod0(layer, 1);
        const float detailScale = u_uvScale * extra.z * u_detailUvScale;
        const vec2 detailUv = context.texCoord * detailScale;
        detailAlbedo += weight * textureGrad(layerAlbedo,
            vec3(detailUv, float(slice)), baseUvDx * detailScale, baseUvDy * detailScale).rgb;
        const vec3 normalTS = textureGrad(layerNormal,
            vec3(detailUv, float(slice)), baseUvDx * detailScale, baseUvDy * detailScale).xyz * 2.0 - 1.0;
        detailNormalWorld += weight * TransformMaterialNormalToWorld(context, normalTS);
        const float macroScale = u_uvScale * u_macroUvScale;
        macroAlbedo += weight * textureGrad(layerAlbedo,
            vec3(context.texCoord * macroScale, float(slice)),
            baseUvDx * macroScale, baseUvDy * macroScale).rgb;
    }
    detailNormalWorld = dot(detailNormalWorld, detailNormalWorld) > 1.0e-8
        ? normalize(detailNormalWorld) : normalize(context.worldNormal);
}

MaterialInputs EvaluateMFTerrainInputs(in MaterialFunctionContext context)
{
    MaterialInputs inputs = CreateDefaultMaterialInputs();
    const vec2 baseUvDx = dFdx(context.texCoord);
    const vec2 baseUvDy = dFdy(context.texCoord);
    const vec3 worldNormal = normalize(context.worldNormal);
    int ids[4];
    float weights[4];
    float coverageWeights[4];
    const uint coverageStatus = TerrainResolveTopLayers(
        context, true, false, ids, weights, coverageWeights);
    const ivec4 topIds = ivec4(ids[0], ids[1], ids[2], ids[3]);
    const TerrainCompositedFootprint compositedFootprint = TerrainCompositedFootprintFromGradients(
        baseUvDx, baseUvDy, u_compositedResolution, u_compositedMaxMip);
    const TerrainCompositedVTResult composited = MF_TerrainCompositedVTAtFootprint(
        compositedPageTable, compositedAlbedo, compositedNormal, compositedOrm,
        context.texCoord, u_compositedResolution, u_compositedMaxMip,
        u_compositedRepeat, u_compositedNormalRG,
        transpose(inverse(mat3(uboM.model))), compositedFootprint);
    vec4 albedo = composited.albedo;
    vec4 pbr = composited.orm;
    vec3 blendedNormal = composited.normalWorld;
    const float invalidPage = 1.0 - composited.valid;
    vec3 detailAlbedo;
    vec3 detailNormalWorld;
    vec3 macroAlbedo;
    TerrainWeightedDetail(context, ids, coverageWeights, baseUvDx, baseUvDy,
        detailAlbedo, detailNormalWorld, macroAlbedo);

    // 参数按米定义；World 单位是 cm。
    const float viewDistance =
        length(context.worldPosition - uboVP.cameraPosition);
    const float detailStart = u_detailFadeStartM * 100.0;
    const float detailEnd = max(u_detailFadeEndM * 100.0, detailStart + 1.0);
    float detailFade = 1.0 - clamp(
        (viewDistance - detailStart) / (detailEnd - detailStart),
        0.0,
        1.0);
    detailFade *= detailFade;
    const float detailWeight = coverageStatus == 0u ? u_detailStrength * detailFade : 0.0;

    const float detailLuma =
        dot(detailAlbedo, vec3(0.299, 0.587, 0.114));
    albedo.rgb *= mix(
        1.0,
        clamp(detailLuma * 2.0, 0.6, 1.4),
        detailWeight);
    const vec3 baseNormal = dot(blendedNormal, blendedNormal) > 1.0e-8
        ? normalize(blendedNormal) : worldNormal;
    blendedNormal = mix(baseNormal, detailNormalWorld, detailWeight);

    // macro 不随距离淡出，专门压制远处的平铺重复感。
    const float macroLuma =
        dot(macroAlbedo, vec3(0.299, 0.587, 0.114));
    albedo.rgb *= mix(
        1.0,
        clamp(0.75 + macroLuma, 0.7, 1.3),
        coverageStatus == 0u ? u_macroStrength : 0.0);

    // 全局（非平铺）图：世界对齐的宏观色 + AO + 湿度。
    // 这些图在打包时已按本地形朝向旋转过，shader 不再做任何翻转。
    const vec2 globalUv =
        (context.worldPosition.xz - u_globalOriginCm) /
        max(u_globalSizeCm, 1.0);
    const vec2 globalClamped = clamp(globalUv, vec2(0.0), vec2(1.0));
    const vec3 macroColour = texture(globalColour, globalClamped).rgb;
    const vec4 globalMasks = texture(globalMasks, globalClamped);
    // 直接向宏观色混合，而不是只传色相：烘好的图本身已经带了大尺度绿/紫
    // 分布，除以亮度会把蓝通道压到 0.4 左右，叠加干燥草地后整体发褐。
    // 1.5 的增益补偿 bake 进去的遮蔽。
    //
    // 全局图只有 2 m/texel，恒定权重会把近处 0.25 m/texel 的 tiling 细节
    // 抹平；因此按相机距离从 near 权重淡入到 full 权重：近处靠 tiling，
    // 远处才让宏观色接管（同时压制平铺重复）。
    const float globalViewDistanceM =
        length(context.worldPosition - uboVP.cameraPosition) * 0.01;
    const float globalFadeStartM = u_globalFadeStartM;
    const float globalFadeEndM =
        max(u_globalFadeEndM, globalFadeStartM + 1.0);
    const float macroFade = smoothstep(
        globalFadeStartM, globalFadeEndM, globalViewDistanceM);
    const float macroWeight = mix(
        u_globalColourWeightNear, u_globalColourWeight, macroFade);
    albedo.rgb = mix(albedo.rgb, macroColour * 1.5, macroWeight);

    if (coverageStatus != 0u && composited.valid < 0.5)
    {
        inputs.baseColor = vec3(0.18);
        inputs.roughness = 0.9;
        inputs.ambientOcclusion = 1.0;
        inputs.normal = worldNormal;
        inputs.tangent = context.worldTangent;
        return inputs;
    }

    // 3 只输出 blended albedo；4/5 仍需落到各自的 page/mip 调试分支。
    if (u_debugMode > 2.5 && u_debugMode < 3.5)
    {
        inputs.baseColor = vec3(0.0);
        inputs.emissiveColor = albedo.rgb;
        inputs.roughness = 1.0;
        inputs.metallic = 0.0;
        inputs.ambientOcclusion = 1.0;
        inputs.normal = worldNormal;
        inputs.tangent = context.worldTangent;
        return inputs;
    }
    if (u_debugMode > 0.5 && u_debugMode < 1.5)
    {
        inputs.baseColor = vec3(0.0);
        inputs.emissiveColor = vec3(weights[0], weights[1], weights[2]);
        inputs.roughness = 1.0;
        inputs.metallic = 0.0;
        inputs.ambientOcclusion = 1.0;
        inputs.normal = worldNormal;
        inputs.tangent = context.worldTangent;
        return inputs;
    }
    if (u_debugMode > 4.5)
    {
        inputs.baseColor = vec3(0.0);
        inputs.emissiveColor = vec3(composited.targetMip / max(u_compositedMaxMip, 1.0));
        inputs.roughness = 1.0;
        inputs.metallic = 0.0;
        inputs.ambientOcclusion = 1.0;
        inputs.normal = worldNormal;
        inputs.tangent = context.worldTangent;
        return inputs;
    }
    if (u_debugMode > 3.5)
    {
        inputs.baseColor = vec3(0.0);
        inputs.emissiveColor = vec3(composited.valid);
        inputs.roughness = 1.0;
        inputs.metallic = 0.0;
        inputs.ambientOcclusion = 1.0;
        inputs.normal = worldNormal;
        inputs.tangent = context.worldTangent;
        return inputs;
    }
    if (u_debugMode >= 1.5)
    {
        inputs.baseColor = vec3(0.0);
        inputs.emissiveColor = vec3(
            float(topIds.x),
            float(topIds.y),
            float(topIds.z)) / float(TERRAIN_MAX_LAYERS - 1);
        inputs.roughness = 1.0;
        inputs.metallic = 0.0;
        inputs.ambientOcclusion = 1.0;
        inputs.normal = worldNormal;
        inputs.tangent = context.worldTangent;
        return inputs;
    }

    // 缺页输出中性表面（0.18 灰、roughness 0.9、几何法线）；有效页的
    // invalidPage=0，mix 的尾项严格为 0，输出与原值逐位一致。
    const float wetness =
        clamp(globalMasks.g * u_globalWetnessStrength, 0.0, 1.0);
    const float globalAo =
        mix(1.0, globalMasks.r, u_globalAoStrength);
    inputs.baseColor =
        mix(albedo.rgb, vec3(0.18), invalidPage) * (1.0 - 0.3 * wetness);
    // ORM 打包（与 UE/Substance 一致）：R=AO、G=roughness、B=metallic。
    inputs.ambientOcclusion = mix(pbr.r, 1.0, invalidPage) * globalAo;
    inputs.roughness =
        mix(mix(pbr.g, 0.9, invalidPage), 0.08, wetness * 0.8);
    inputs.metallic = mix(pbr.b, 0.0, invalidPage);
    inputs.opacity = 1.0;
    inputs.opacityMask = 1.0;
    inputs.normal = normalize(mix(blendedNormal, worldNormal, invalidPage));
    inputs.tangent = OrthonormalizeMaterialTangent(inputs.normal, context.worldTangent);
    return inputs;
}
#endif

#endif
