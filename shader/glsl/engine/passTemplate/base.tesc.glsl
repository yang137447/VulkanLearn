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

// 距离越近细分越高，半径外严格回到 1，保证与未细分邻接 patch 的边界一致。
float TerrainTessFactorForDistance(float distanceCm)
{
    const float radiusCm = max(u_terrainTessRadiusM, 0.0) * 100.0;
    const float maxTess = max(u_terrainTessMax, 1.0);
    if (radiusCm <= 1.0e-3)
    {
        return 1.0;
    }
    const float falloff = 1.0 - smoothstep(0.0, radiusCm, distanceCm);
    return clamp(mix(1.0, maxTess, falloff), 1.0, maxTess);
}

void main()
{
    // TCS 不做任何逐控制点变形；gl_Position 与材质 varyings 原样传给 TES。
    v2fOut[gl_InvocationID] = v2fIn[gl_InvocationID];
    gl_out[gl_InvocationID].gl_Position = gl_in[gl_InvocationID].gl_Position;

    if (gl_InvocationID == 0)
    {
        const vec3 cameraPosition = uboVP.cameraPosition;
        const vec3 p0 = v2fIn[0].worldPosition;
        const vec3 p1 = v2fIn[1].worldPosition;
        const vec3 p2 = v2fIn[2].worldPosition;
        const vec3 p3 = v2fIn[3].worldPosition;

        // 外边因子只用边的两个端点计算。相邻 patch 共享同一条边时端点相同，
        // 因此中点距离相同，equal_spacing 下边界顶点完全一致，不会产生裂缝。
        gl_TessLevelOuter[0] = TerrainTessFactorForDistance(
            length(0.5 * (p0 + p1) - cameraPosition));
        gl_TessLevelOuter[1] = TerrainTessFactorForDistance(
            length(0.5 * (p1 + p2) - cameraPosition));
        gl_TessLevelOuter[2] = TerrainTessFactorForDistance(
            length(0.5 * (p2 + p3) - cameraPosition));
        gl_TessLevelOuter[3] = TerrainTessFactorForDistance(
            length(0.5 * (p3 + p0) - cameraPosition));

        // 内部因子不参与跨 patch 边界匹配，直接用 patch 中心距离即可。
        const float insideFactor = TerrainTessFactorForDistance(
            length(0.25 * (p0 + p1 + p2 + p3) - cameraPosition));
        gl_TessLevelInner[0] = insideFactor;
        gl_TessLevelInner[1] = insideFactor;
    }
}

#endif
