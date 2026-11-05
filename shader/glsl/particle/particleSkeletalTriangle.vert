#version 450

#include "particleCommon.glsl"
#include "particleStorageRO.glsl"
#include "particleSorted.glsl"
#include "particleAlive.glsl"
#include "particleMeshTransform.glsl"

layout(location = 0) out vec4 vColor;
layout(location = 1) out float vSoftness;
layout(location = 2) out float vViewDepth;
layout(location = 4) out vec3 vNormal;
layout(location = 5) out vec3 vWorldPosition;
layout(location = 8) out vec2 vUv;
layout(location = 9) out float vLifeT;
layout(location = 10) out float vSeed;

layout(std140, set = 1, binding = 0) uniform ParticleMaterialUniform
{
    vec4 uvScaleOffset;
    vec4 textureFlags;
    vec4 materialParams;
} material;

void CullVertex()
{
    gl_Position = vec4(0.0, 0.0, 2.0, 1.0);
    vColor = vec4(0.0);
    vSoftness = 0.0;
    vViewDepth = 0.0;
    vNormal = vec3(0.0);
    vWorldPosition = vec3(0.0);
    vUv = vec2(0.0);
    vLifeT = 0.0;
    vSeed = 0.0;
}

void main()
{
    uint particleIndex = 0;
    Particle particle;
    if (!ResolveParticleMeshParticle(
            uint(gl_InstanceIndex),
            particleIndex,
            particle) ||
        int(particle.source0.w + 0.5) != 1)
    {
        CullVertex();
        return;
    }
    int vertex = int(gl_VertexIndex % 3u);
    vec3 sourcePosition = vertex == 0
        ? particle.source0.xyz
        : (vertex == 1 ? particle.source1.xyz : particle.source2.xyz);
    // 三角面源在 spawn 时与骨骼网格 model space 对齐（面片重心 + 模型空间偏移），
    // 粒子当前位置已在模拟空间；meshScale 是作者对模型局部的逐轴缩放。
    vec3 worldPosition = particle.positionAge.xyz +
        mat3(frame.localToWorld) * (sourcePosition * frame.meshScale.xyz);
    gl_Position = frame.viewProjection * vec4(worldPosition, 1.0);
    vViewDepth = -(frame.view * vec4(worldPosition, 1.0)).z;
    vColor = EvaluateColorCurve(ParticleMeshLifeT(particle));
    vColor.a *= frame.render.w;
    vSoftness = frame.render.z;
    vec3 e1 = particle.source1.xyz - particle.source0.xyz;
    vec3 e2 = particle.source2.xyz - particle.source0.xyz;
    vec3 localNormal = cross(e1, e2);
    if (dot(localNormal, localNormal) <= 1.0e-12)
    {
        localNormal = vec3(0.0, 1.0, 0.0);
    }
    // 面法线只做模型空间到世界空间的旋转/缩放，不再叠加逐粒子朝向基：
    // 三角面粒子的意义就是把原网格表面的朝向原样带到世界空间。
    vNormal = normalize(
        mat3(frame.localToWorld) * normalize(localNormal));
    vWorldPosition = worldPosition;
    vUv = vec2(
        vertex == 0 ? 0.0 : (vertex == 1 ? 1.0 : 0.5),
        vertex == 2 ? 1.0 : 0.0);
    vLifeT = ParticleMeshLifeT(particle);
    vSeed = fract(float(particleIndex) * 0.61803398875);
}
