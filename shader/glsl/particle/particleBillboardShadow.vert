#version 450

#include "particleBillboardTransform.glsl"

layout(location = 0) out vec3 vShadowWorldPosition;
layout(location = 1) out vec2 vShadowLocalUV;
layout(location = 2) out vec2 vShadowAtlasUV;
layout(location = 3) out float vShadowSeed;
layout(location = 4) out float vShadowTime;
layout(location = 5) out float vShadowColorAlpha;

void main()
{
    // 与可见 billboard 走同一个几何函数；阴影不关心顺序，固定用
    // aliveIndices，避免全局排序池发射器的 pair 表未初始化。
    ParticleBillboardVertex vertex;
    if (!BuildParticleBillboardVertex(
            uint(gl_InstanceIndex),
            uint(gl_VertexIndex),
            false,
            vertex))
    {
        // 失效实例推到必然裁剪的位置，不写任何深度。
        gl_Position = vec4(0.0, 0.0, 2.0, 1.0);
        vShadowWorldPosition = vec3(0.0);
        vShadowLocalUV = vec2(0.0);
        vShadowAtlasUV = vec2(0.0);
        vShadowSeed = 0.0;
        vShadowTime = 0.0;
        vShadowColorAlpha = 0.0;
        return;
    }

    gl_Position =
        frame.shadowViewProj * vec4(vertex.worldPosition, 1.0);
    vShadowWorldPosition = vertex.worldPosition;
    vShadowLocalUV = vertex.localUV;
    vShadowAtlasUV = vertex.atlasUV;
    vShadowSeed = vertex.seed;
    vShadowTime = vertex.time;
    vShadowColorAlpha = vertex.color.a;
}
