#ifndef VL_MATERIAL_FUNCTION_TERRAIN_LAYER_BLEND_GLSL
#define VL_MATERIAL_FUNCTION_TERRAIN_LAYER_BLEND_GLSL

#include "../engine/materialContext.glsl"
#include "../engine/materialInputs.glsl"
#include "../common/commonUbo.glsl"
#include "mf_normal.glsl"
#include "mf_terrainLayerVT.glsl"

// Phase B：每 texel 的 top-4 由 CPU 烘焙的 ID/权重图给出，shader 不再遍历全局层表。
// layerTable 只保留每层 UV 参数；后续 VT/SSBO 版本沿用同一 ID/权重语义。
const int TERRAIN_MAX_LAYERS = 256;

vec4 TerrainLayerExtra(int layer)
{
    // catalog 现在是 256x4：row0 高度/坡度、row1 noise/blendEdge/uvScale/
    // uvRotation、row2 页数/页表基址、row3 平均色。这里只取 row1 的
    // texel 中心 (1 + 0.5) / 4 = 0.375；nearest + 无 mip，不会跨行。
    return texture(
        layerTable,
        vec2((float(layer) + 0.5) / float(TERRAIN_MAX_LAYERS), 0.375));
}

MaterialInputs EvaluateMFTerrainInputs(in MaterialFunctionContext context)
{
    MaterialInputs inputs = CreateDefaultMaterialInputs();
    const vec3 worldNormal = normalize(context.worldNormal);
    const int pageTableSize = int(u_pageTableSize);
    // 页索引保持原表达式；全局 UV 恰为 1.0 时 floor 会得到 size，clamp 回最后一页。
    const vec2 rawPageUv = context.texCoord1 * u_pageTableSize;
    const ivec2 tile = clamp(
        ivec2(floor(rawPageUv)),
        ivec2(0),
        ivec2(pageTableSize - 1));
    vec4 page = texelFetch(pageTable, tile, 0);
    // 缺页掩码在 remap 前保存；有效页走与原 shader 完全相同的采样路径，
    // 缺页只在最终输出处替换成中性表面，避免分支改变纹理采样/隐式 LOD。
    const float invalidPage = page.a < 0.5 ? 1.0 : 0.0;
    // 专用 fallback 物理页（页表最后一页）：只用于保持采样路径完整，
    // 绝不回退到另一个地形的 page 0；最终输出由 invalidPage 替换为中性表面。
    if (page.a < 0.5)
        page = vec4(float(pageTableSize - 1) / 255.0, float(pageTableSize - 1) / 255.0, 0.0, 1.0);
    const ivec2 pageCoord = ivec2(round(page.xy * 255.0));
    vec2 localUv = fract(rawPageUv);
    // 权重图使用线性过滤；内缩半 texel 防止跨 page 采样。
    localUv = clamp(
        localUv,
        vec2(0.5 / u_pageResolution),
        vec2(1.0 - 0.5 / u_pageResolution));
    // 全局 UV 恰为 1.0 时 fract 回绕到 0，会采样最后一页的第一列；
    // 用 step 选择该页最后一列。非边界片元的 localUv 与旧版逐位相同，
    // 因此隐式 LOD/导数也不变。
    localUv = mix(
        localUv,
        vec2(1.0 - 0.5 / u_pageResolution),
        step(vec2(float(pageTableSize)), rawPageUv));
    const float atlasResolution = u_pageTableSize * u_pageResolution;
    const vec2 atlasUv =
        (vec2(pageCoord) * u_pageResolution + localUv * u_pageResolution) /
        atlasResolution;
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
            const float weight = texture(
                layerWeightMaps, vec3(weightUv, float(layer))).r;
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
        topWeights = vec4(
            max(w0, 0.0), max(w1, 0.0), max(w2, 0.0), max(w3, 0.0));
    }
    else
    {
        topIds = ivec4(round(texture(layerIds, atlasUv).rgba * 255.0));
        topWeights = texture(layerWeights, atlasUv);
    }
    float weightSum = topWeights.x + topWeights.y + topWeights.z + topWeights.w;
    if (weightSum < 1.0e-4)
    {
        topWeights = vec4(1.0, 0.0, 0.0, 0.0);
        weightSum = 1.0;
    }
    topWeights /= weightSum;
    int ids[4] = int[4](topIds.x, topIds.y, topIds.z, topIds.w);
    float weights[4] = float[4](topWeights.x, topWeights.y, topWeights.z, topWeights.w);

    // Height blend: the ID/weight atlas gives a linear cross-fade, which looks
    // muddy where two layers meet. Re-weighting by each layer's 16-bit height
    // (0..1, mean 0.5) keeps the ordering and narrows the transition, the
    // classic "height + inverted weight" scheme. strength 0 is a no-op.
    if (u_heightBlendStrength > 0.0)
    {
        // 近处一个 texel 覆盖很多像素，逐 texel 的高度差会让层归属来回
        // 翻转（近看是一块块锯齿）；中远景一个 texel 已经小于像素，
        // 这种翻转自然消失。因此只在 8–45 m 之外启用高度加权。
        const float blendDistanceM =
            length(context.worldPosition - uboVP.cameraPosition) * 0.01;
        const float heightBlendFade =
            smoothstep(8.0, 45.0, blendDistanceM);
        float scores[4];
        float scoreMax = -1.0e9;
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
            const float height =
                textureLod(layerHeight, vec3(uv, float(layer)), 3.0).r;
            // 换算成米：每层归一化到 0..1 后直接用会让平整沙层和岩石
            // 拥有同样的起伏幅度；乘 displacementM 才能保留相对高差。
            const float displacementM =
                TerrainLayerCatalogRow(layer, 3).w;
            const float heightMeters = (height - 0.5) * displacementM;
            // 高度项必须**有上限**：每层高度都归一化到 0..1，texel 级噪声
            // 足以让高度项压过权重差，出现逐 texel 的换层（近看是锯齿块）。
            // 夹到 ±0.25 后它只能在权重接近时微调过渡，不能改变主次。
            const float heightTerm = clamp(
                heightMeters * u_heightBlendStrength, -0.25, 0.25);
            scores[slot] =
                heightTerm * heightBlendFade + (1.0 - weights[slot]);
            scoreMax = max(scoreMax, scores[slot]);
        }
        float total = 0.0;
        for (int slot = 0; slot < 4; ++slot)
        {
            float weight = weights[slot];
            if (weight > 1.0e-5)
                weight *= clamp(scores[slot] - scoreMax + 1.0, 0.0, 1.0);
            weights[slot] = weight;
            total += weight;
        }
        if (total > 1.0e-5)
        {
            for (int slot = 0; slot < 4; ++slot)
                weights[slot] /= total;
        }
        else
        {
            weights[0] = 1.0;
            weights[1] = 0.0;
            weights[2] = 0.0;
            weights[3] = 0.0;
        }
        topWeights = vec4(weights[0], weights[1], weights[2], weights[3]);
    }

    vec4 albedo = vec4(0.0);
    vec4 pbr = vec4(0.0);
    vec3 blendedNormal = vec3(0.0);
    float vtDebugPage = 0.0;
    float vtDebugMip = 0.0;
    if (u_terrainLayerMode > 0.5)
    {
        // L3：VT 路径。top-4 层号/权重仍由覆盖页给出；像素改走
        // catalog -> 每 mip 页表 -> 物理页 -> atlas。mode 0 不进入此分支。
        const TerrainLayerVTResult vt = AccumulateTerrainLayerVT(
            context,
            topIds,
            topWeights,
            worldNormal);
        albedo = vt.albedo;
        pbr = vt.pbr;
        blendedNormal = vt.normalWorld;
        vtDebugPage = vt.debugPhysicalPage;
        vtDebugMip = vt.debugMip;
    }
    else
    {
        for (int slot = 0; slot < 4; ++slot)
        {
            const int layer = ids[slot];
            const vec4 extra = TerrainLayerExtra(layer);
            const float angle = extra.w;
            const mat2 rotation = mat2(cos(angle), sin(angle), -sin(angle), cos(angle));
            const vec2 uv = rotation * (context.texCoord * u_uvScale * extra.z);
            const vec3 layerUv = vec3(uv, float(layer));
            albedo += texture(layerAlbedo, layerUv) * weights[slot];
            pbr += texture(layerPbr, layerUv) * weights[slot];
            const vec3 normalTS = texture(layerNormal, layerUv).xyz * 2.0 - 1.0;
            blendedNormal += TransformMaterialNormalToWorld(context, normalTS) * weights[slot];
        }
    }

    // 远景 macro + 近景 detail 统一后处理：mode 0 直接采样数组，
    // mode 1 也仍用 layerAlbedo/layerNormal 数组做这两层，不走 VT 页表。
    const int detailLayer = ids[0];
    const vec4 detailExtra = TerrainLayerExtra(detailLayer);
    const vec2 detailUv =
        context.texCoord * u_uvScale * detailExtra.z * u_detailUvScale;
    const vec3 detailAlbedo =
        texture(layerAlbedo, vec3(detailUv, float(detailLayer))).rgb;
    const vec3 detailNormalTS =
        texture(layerNormal, vec3(detailUv, float(detailLayer))).xyz * 2.0 - 1.0;
    const vec3 detailNormalWorld =
        TransformMaterialNormalToWorld(context, detailNormalTS);

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
    const float detailWeight = u_detailStrength * detailFade;

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
    const vec2 macroUv = context.texCoord * u_uvScale * u_macroUvScale;
    const vec3 macroAlbedo =
        texture(layerAlbedo, vec3(macroUv, float(detailLayer))).rgb;
    const float macroLuma =
        dot(macroAlbedo, vec3(0.299, 0.587, 0.114));
    albedo.rgb *= mix(
        1.0,
        clamp(0.75 + macroLuma, 0.7, 1.3),
        u_macroStrength);

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

    if (u_debugMode > 2.5)
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
        // 5 = VT mip（0..6）。mode 0 没有页表 mip，输出 0。
        inputs.baseColor = vec3(0.0);
        inputs.emissiveColor = vec3(vtDebugMip / float(TERRAIN_VT_MAX_MIP));
        inputs.roughness = 1.0;
        inputs.metallic = 0.0;
        inputs.ambientOcclusion = 1.0;
        inputs.normal = worldNormal;
        inputs.tangent = context.worldTangent;
        return inputs;
    }
    if (u_debugMode > 3.5)
    {
        // 4 = VT 物理页号（按 L2 默认 1023 个可用槽位归一化）。
        inputs.baseColor = vec3(0.0);
        inputs.emissiveColor = vec3(vtDebugPage / 1023.0);
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
