#version 450

#include "particleRibbonTransform.glsl"

layout(location = 0) out vec4 vColor;
layout(location = 1) out vec2 vUV;
layout(location = 2) out float vSoftness;
layout(location = 3) out float vSeed;
layout(location = 4) out float vTime;
layout(location = 5) out vec2 vAtlasUV;
layout(location = 6) out float vViewDepth;
layout(location = 7) out vec3 vWorldPosition;

void main()
{
    // 非索引 indirect 命令的 instanceCount 由 GPU finalize 写为存活数；
    // 每个实例固定输出一条条带的 segments * 6 个顶点。
    ParticleRibbonVertex vertex;
    if (!BuildParticleRibbonVertex(
            uint(gl_InstanceIndex),
            uint(gl_VertexIndex),
            frame.sortParams.x > 0.5,
            vertex))
    {
        gl_Position = vec4(0.0, 0.0, 2.0, 1.0);
        vColor = vec4(0.0);
        vUV = vec2(0.0);
        vSoftness = 0.0;
        vSeed = 0.0;
        vTime = 0.0;
        vAtlasUV = vec2(0.0);
        vViewDepth = 0.0;
        vWorldPosition = vec3(0.0);
        return;
    }

    gl_Position =
        frame.viewProjection * vec4(vertex.worldPosition, 1.0);
    vColor = vertex.color;
    vUV = vertex.uv;
    vSoftness = frame.render.z;
    vSeed = vertex.seed;
    vTime = vertex.time;
    vAtlasUV = vertex.atlasUV;
    vViewDepth = vertex.viewDepth;
    vWorldPosition = vertex.worldPosition;
}
