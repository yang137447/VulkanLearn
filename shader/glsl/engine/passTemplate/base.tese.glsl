#ifndef VL_PASS_TEMPLATE_BASE_TESE_GLSL
#define VL_PASS_TEMPLATE_BASE_TESE_GLSL

// 文件职责：地形材质的 Tessellation Evaluation 模板。主位移来自 top-4 材质层
// layerHeight，detailHeightMap 只做可选的 terrain-UV 叠加；输出接口严格对应 base.vert。
// File responsibility: Terrain material tessellation-evaluation template. Primary
// displacement comes from the top-4 material-layer height maps; detailHeightMap is only
// an optional terrain-UV overlay; its outputs mirror base.vert exactly.

#include "../../common/commonUbo.glsl"
#include "../materialContext.glsl"
#define VL_TERRAIN_TES_STAGE 1
#include "../../materialFunction/mf_terrainLayerBlend.glsl"

// 引擎图形管线固定 FrontFace::eClockwise；细分器必须按同一绕序生成三角形，
// 否则整个地形会被背面剔除（实测 ccw 只剩退化 patch 的边缘）。
layout(quads, equal_spacing, cw) in;

layout(location = 0) in MaterialVaryings v2fIn[];
// 与 base.vert 的 `layout(location = 0) out MaterialVaryings v2f;` 完全一致，
// 这样 base.frag 不需要知道是否经过细分。
layout(location = 0) out MaterialVaryings v2f;

float TerrainDetailHeight(vec2 detailUv)
{
    // 细分阶段没有片元导数，显式 LOD 0 同时避免驱动对隐式 LOD 的分歧。
    return textureLod(detailHeightMap, detailUv, 0.0).r;
}

void main()
{
    const float u = gl_TessCoord.x;
    const float v = gl_TessCoord.y;
    // 控制点必须按 patch 周长顺序 p0 -> p1 -> p2 -> p3 提交；该顺序同时
    // 定义了 TCS 外边 0..3 和这里的双线性参数化。
    const float w0 = (1.0 - u) * (1.0 - v);
    const float w1 = u * (1.0 - v);
    const float w2 = u * v;
    const float w3 = (1.0 - u) * v;

    vec3 worldPosition =
        v2fIn[0].worldPosition * w0 +
        v2fIn[1].worldPosition * w1 +
        v2fIn[2].worldPosition * w2 +
        v2fIn[3].worldPosition * w3;
    vec3 worldNormal = normalize(
        v2fIn[0].worldNormal * w0 +
        v2fIn[1].worldNormal * w1 +
        v2fIn[2].worldNormal * w2 +
        v2fIn[3].worldNormal * w3);
    const vec4 previousClipPosition =
        v2fIn[0].previousClipPosition * w0 +
        v2fIn[1].previousClipPosition * w1 +
        v2fIn[2].previousClipPosition * w2 +
        v2fIn[3].previousClipPosition * w3;
    const vec4 vertexColor =
        v2fIn[0].vertexColor * w0 +
        v2fIn[1].vertexColor * w1 +
        v2fIn[2].vertexColor * w2 +
        v2fIn[3].vertexColor * w3;
    const vec2 texCoord =
        v2fIn[0].texCoord * w0 +
        v2fIn[1].texCoord * w1 +
        v2fIn[2].texCoord * w2 +
        v2fIn[3].texCoord * w3;
    const vec2 texCoord1 =
        v2fIn[0].texCoord1 * w0 +
        v2fIn[1].texCoord1 * w1 +
        v2fIn[2].texCoord1 * w2 +
        v2fIn[3].texCoord1 * w3;
    const vec4 worldTangent =
        v2fIn[0].worldTangent * w0 +
        v2fIn[1].worldTangent * w1 +
        v2fIn[2].worldTangent * w2 +
        v2fIn[3].worldTangent * w3;

    const float radiusCm = max(u_terrainTessRadiusM, 0.0) * 100.0;
    const float cameraDistanceCm =
        length(worldPosition - uboVP.cameraPosition);
    // 半径边界处位移严格归零；边界 patch 的 factor 也为 1，因此与未细分
    // 邻居共享同一组端点高度，不会出现高度缝。
    const float farFade = radiusCm <= 1.0e-3
        ? 0.0
        : 1.0 - smoothstep(0.75 * radiusCm, radiusCm, cameraDistanceCm);
    // 近处保护：相机 2 m 内把位移淡出到 0。否则低空/贴地观察时，位移后的
    // 地表可能穿过相机近裁剪面或把相机包进背面剔除区域，看起来像“近处被裁剪”。
    const float nearGuardCm = 200.0;
    const float nearFade = smoothstep(0.0, nearGuardCm, cameraDistanceCm);
    const float displacementFade = farFade * nearFade;

    // 与片元阶段共用同一份 top-4 解析；TES 没有可靠的隐式导数，因此
    // 所有层采样都走显式 LOD，并跳过 fragment 专用的 height-blend。
    MaterialFunctionContext ctx;
    ctx.clipPosition =
        v2fIn[0].clipPosition * w0 +
        v2fIn[1].clipPosition * w1 +
        v2fIn[2].clipPosition * w2 +
        v2fIn[3].clipPosition * w3;
    ctx.previousClipPosition = previousClipPosition;
    ctx.worldPosition = worldPosition;
    ctx.worldNormal = worldNormal;
    ctx.vertexColor = vertexColor;
    ctx.texCoord = texCoord;
    ctx.texCoord1 = texCoord1;
    ctx.worldTangent = worldTangent;

    if (displacementFade > 0.0 && abs(u_layerDisplacementScale) > 1.0e-4)
    {
        int ids[4];
        float weights[4];
        TerrainResolveTopLayers(ctx, false, false, ids, weights);
        const float layerDisplacementCm =
            TerrainBlendedLayerDisplacementMeters(ctx, ids, weights) *
            u_layerDisplacementScale * 100.0;
        worldPosition += worldNormal * layerDisplacementCm * displacementFade;
    }

    const float amplitudeCm = u_detailAmplitudeM * 100.0;
    // detailHeightMap 只是可选叠加：UV 与颜色基础 tiling 对齐，默认
    // amplitude 0 时完全不采样。
    if (abs(u_detailAmplitudeM) > 0.001 && displacementFade > 0.0)
    {
        const float uvScale = max(u_uvScale * u_detailUvScale, 0.001);
        const vec2 detailUv = texCoord * uvScale;
        const float centerHeight =
            TerrainDetailHeight(detailUv);
        // 参数按米书写，World 单位是 cm；沿插值世界法线位移，保证斜坡上方向正确。
        worldPosition += worldNormal *
            ((centerHeight - 0.5) * amplitudeCm * displacementFade);

        // 用 ±0.05 tile 的高度差在切线空间扰动法线；不依赖世界尺度，
        // UV 已与颜色 tiling 对齐，因此细节方向与颜色纹理一致。
        const float epsilonUv = 0.05;
        const float heightLeft = TerrainDetailHeight(
            detailUv - vec2(epsilonUv, 0.0));
        const float heightRight = TerrainDetailHeight(
            detailUv + vec2(epsilonUv, 0.0));
        const float heightDown = TerrainDetailHeight(
            detailUv - vec2(0.0, epsilonUv));
        const float heightUp = TerrainDetailHeight(
            detailUv + vec2(0.0, epsilonUv));
        const float gradientU =
            (heightRight - heightLeft) * u_detailNormalStrength;
        const float gradientV =
            (heightUp - heightDown) * u_detailNormalStrength;
        const float normalWeight = displacementFade *
            clamp(abs(u_detailAmplitudeM) * 4.0, 0.0, 1.0);
        if (normalWeight > 0.0)
        {
            const vec3 baseNormal = normalize(worldNormal);
            const vec3 tangent = normalize(worldTangent.xyz);
            const vec3 bitangent = normalize(cross(baseNormal, tangent)) *
                (worldTangent.w < 0.0 ? -1.0 : 1.0);
            const vec3 detailNormal = normalize(
                baseNormal - (tangent * gradientU + bitangent * gradientV));
            worldNormal = normalize(mix(baseNormal, detailNormal, normalWeight));
        }
    }

    const vec4 clipPosition =
        uboVP.projection * uboVP.view * vec4(worldPosition, 1.0);

    v2f.clipPosition = clipPosition;
    v2f.previousClipPosition = previousClipPosition;
    v2f.worldPosition = worldPosition;
    v2f.worldNormal = worldNormal;
    v2f.vertexColor = vertexColor;
    v2f.texCoord = texCoord;
    v2f.texCoord1 = texCoord1;
    v2f.worldTangent = worldTangent;
    gl_Position = clipPosition;
}

#endif
