#version 450

#include "particleBillboardTransform.glsl"

layout(location = 0) out vec4 vColor;
layout(location = 1) out vec2 vUV;
layout(location = 2) out float vSoftness;
layout(location = 3) out float vSeed;
layout(location = 4) out float vTime;
layout(location = 5) out vec2 vAtlasUV;
layout(location = 6) out float vViewDepth;
layout(location = 7) out vec3 vWorldPosition;
layout(location = 8) out vec3 vLightingNormal;
layout(location = 9) out vec2 vAtlasUV2;
layout(location = 10) out float vAtlasBlend;
layout(location = 11) out float vSize;

void main()
{
    // 间接绘制把实例数写到 alive；一个实例固定六个顶点，因此实例索引
    // 才是紧凑存活槽位，顶点索引只决定 quad 角点。排序开启时由共享
    // 几何函数读取 pair 表，保证 seed 仍使用原始池索引。
    ParticleBillboardVertex vertex;
    if (!BuildParticleBillboardVertex(
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
        vLightingNormal = vec3(0.0);
        vAtlasUV2 = vec2(0.0);
        vAtlasBlend = 0.0;
        vSize = 0.0;
        return;
    }

    gl_Position =
        frame.viewProjection * vec4(vertex.worldPosition, 1.0);
    vColor = vertex.color;
    vUV = vertex.localUV;
    vSoftness = frame.render.z;
    vSeed = vertex.seed;
    vTime = vertex.time;
    vAtlasUV = vertex.atlasUV;
    vAtlasUV2 = vertex.atlasUV2;
    vAtlasBlend = vertex.atlasBlend;
    vSize = vertex.size;
    vViewDepth = vertex.viewDepth;
    vWorldPosition = vertex.worldPosition;
    vLightingNormal = vertex.lightingNormal;
}
