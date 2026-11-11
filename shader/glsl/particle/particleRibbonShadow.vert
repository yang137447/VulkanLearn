#version 450

#include "particleRibbonTransform.glsl"

layout(location = 0) out vec3 vShadowWorldPosition;
layout(location = 1) out vec2 vShadowUV;
layout(location = 2) out vec2 vShadowAtlasUV;
layout(location = 3) out float vShadowSeed;
layout(location = 4) out float vShadowTime;
layout(location = 5) out float vShadowColorAlpha;

void main()
{
    // 与可见 ribbon 共用历史点/宽度/相机构建；阴影顺序无关，固定走
    // aliveIndices。
    ParticleRibbonVertex vertex;
    if (!BuildParticleRibbonVertex(
            uint(gl_InstanceIndex),
            uint(gl_VertexIndex),
            false,
            vertex))
    {
        gl_Position = vec4(0.0, 0.0, 2.0, 1.0);
        vShadowWorldPosition = vec3(0.0);
        vShadowUV = vec2(0.0);
        vShadowAtlasUV = vec2(0.0);
        vShadowSeed = 0.0;
        vShadowTime = 0.0;
        vShadowColorAlpha = 0.0;
        return;
    }

    gl_Position =
        frame.shadowViewProj * vec4(vertex.worldPosition, 1.0);
    vShadowWorldPosition = vertex.worldPosition;
    vShadowUV = vertex.uv;
    vShadowAtlasUV = vertex.atlasUV;
    vShadowSeed = vertex.seed;
    vShadowTime = vertex.time;
    vShadowColorAlpha = vertex.color.a;
}
