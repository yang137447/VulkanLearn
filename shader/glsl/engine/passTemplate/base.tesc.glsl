#ifndef VL_PASS_TEMPLATE_BASE_TESC_GLSL
#define VL_PASS_TEMPLATE_BASE_TESC_GLSL

// 文件职责：地形材质的 Tessellation Control 模板。只做 quad patch 控制点透传，
// 并按相机距离写入外边/内部细分因子；不读取材质表面、不写 GBuffer，也不位移顶点。
// File responsibility: Terrain material tessellation-control template. It only forwards
// quad-patch control points and writes distance-based outer/inner factors; it does not
// evaluate material surfaces, write GBuffers, or displace vertices.

#include "../../common/commonUbo.glsl"
#include "../materialContext.glsl"

layout(vertices = 4) out;

// 顶点阶段与 TCS 的接口保持一致；TES 侧再按同样的 MaterialVaryings 结构接收。
layout(location = 0) in MaterialVaryings v2fIn[];
layout(location = 0) out MaterialVaryings v2fOut[];

// 细分因子按 patch 边在屏幕上的像素长度计算：近处自然高、远处按目标像素数
// 降到 1，而不是按世界距离继续细分。u_terrainTessRadiusM 仍是作者上限，
// 在 0.8R..R 平滑退回 1，避免 LOD0 边界硬跳。
float TerrainTessFactorForEdge(vec3 edgeStart, vec3 edgeEnd)
{
    const float maxTess = max(u_terrainTessMax, 1.0);
    const float targetPixels = clamp(u_terrainTessTargetPixels, 1.0, 16.0);
    const vec2 viewportPixels = max(uboVP.viewportParams.xy, vec2(1.0));
    const vec4 clipStart = uboVP.viewProjection * vec4(edgeStart, 1.0);
    const vec4 clipEnd = uboVP.viewProjection * vec4(edgeEnd, 1.0);
    // 任一端点在相机后方时 NDC 会翻转；直接退回 1，避免生成巨大因子。
    if (clipStart.w <= 0.05 || clipEnd.w <= 0.05) return 1.0;
    const vec2 ndcStart = clipStart.xy / clipStart.w;
    const vec2 ndcEnd = clipEnd.xy / clipEnd.w;
    const float edgePixels = length((ndcEnd - ndcStart) * 0.5 * viewportPixels);

    const float radiusCm = max(u_terrainTessRadiusM, 0.0) * 100.0;
    const float midDistance = length(
        0.5 * (edgeStart + edgeEnd) - uboVP.cameraPosition);
    const float radiusFade = radiusCm <= 1.0e-3
        ? 0.0
        : 1.0 - smoothstep(0.8 * radiusCm, radiusCm, midDistance);
    const float factor = clamp(edgePixels / targetPixels, 1.0, maxTess);
    return mix(1.0, factor, radiusFade);
}

void main()
{
    // TCS 不做任何逐控制点变形；gl_Position 与材质 varyings 原样传给 TES。
    v2fOut[gl_InvocationID] = v2fIn[gl_InvocationID];
    gl_out[gl_InvocationID].gl_Position = gl_in[gl_InvocationID].gl_Position;

    if (gl_InvocationID == 0)
    {
        const vec3 p0 = v2fIn[0].worldPosition;
        const vec3 p1 = v2fIn[1].worldPosition;
        const vec3 p2 = v2fIn[2].worldPosition;
        const vec3 p3 = v2fIn[3].worldPosition;

        // 外边因子只用边的两个端点计算。相邻 patch 共享同一条边时端点相同，
        // 因此屏幕投影相同，equal_spacing 下边界顶点完全一致，不会产生裂缝。
        const float edge01 = TerrainTessFactorForEdge(p0, p1);
        const float edge12 = TerrainTessFactorForEdge(p1, p2);
        const float edge23 = TerrainTessFactorForEdge(p2, p3);
        const float edge30 = TerrainTessFactorForEdge(p3, p0);
        gl_TessLevelOuter[0] = edge01;
        gl_TessLevelOuter[1] = edge12;
        gl_TessLevelOuter[2] = edge23;
        gl_TessLevelOuter[3] = edge30;

        // 内部因子不参与跨 patch 边界匹配；取四条边的最大值，避免内部比
        // 屏幕空间目标更稀。
        const float insideFactor = max(max(edge01, edge12), max(edge23, edge30));
        gl_TessLevelInner[0] = insideFactor;
        gl_TessLevelInner[1] = insideFactor;
    }
}

#endif
