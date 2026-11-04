#version 450

#include "particleCommon.glsl"
#include "particleStorageRO.glsl"
#include "particleSorted.glsl"
#include "particleAlive.glsl"
#include "particleMeshTransform.glsl"

layout(location = 0) in vec3 inPosition;
layout(location = 1) in vec3 inNormal;
layout(location = 2) in vec4 inColor;
layout(location = 3) in vec2 inTexCoord;

layout(location = 0) out vec4 vColor;
layout(location = 1) out float vSoftness;
layout(location = 2) out float vViewDepth;
// 世界空间法线；仅 particleMesh.frag 的方向光 Lambert 使用。
layout(location = 4) out vec3 vNormal;
// 世界空间位置；仅 mesh frag 的 CSM 级联选择使用。
layout(location = 5) out vec3 vWorldPosition;

// 与 vertex descriptor 的 billboard/ribbon 路径共用 binding 2，保证
// descriptor set layout 完全一致；mesh v1 不采样纹理。
layout(set = 0, binding = 2) uniform sampler2D particleTexture;

void CullVertex()
{
    gl_Position = vec4(0.0, 0.0, 2.0, 1.0);
    vColor = vec4(0.0);
    vSoftness = 0.0;
    vViewDepth = 0.0;
    vNormal = vec3(0.0);
    vWorldPosition = vec3(0.0);
}

void main()
{
    uint drawIndex = uint(gl_InstanceIndex);
    uint particleIndex = 0;
    Particle particle;
    if (!ResolveParticleMeshParticle(drawIndex, particleIndex, particle))
    {
        CullVertex();
        return;
    }

    // 非 localSpace 的模拟位置/速度在 spawn 时已经变换到世界空间；
    // mesh v1 拒绝 localSpace，因此这里不再重复乘 localToWorld。
    vec3 localPosition = inPosition * frame.meshScale.xyz;
    vec3 worldPosition =
        ParticleMeshWorldPosition(particle, particleIndex, localPosition);
    gl_Position = frame.viewProjection * vec4(worldPosition, 1.0);
    vViewDepth = -(frame.view * vec4(worldPosition, 1.0)).z;

    vColor = EvaluateColorCurve(ParticleMeshLifeT(particle)) * inColor;
    vColor.a *= frame.render.w;
    // 纹理尺寸门控只用于保留 binding 2；mesh v1 不采样纹理。
    float textureGate = clamp(
        float(textureSize(particleTexture, 0).x),
        0.0,
        1.0);
    vColor.rgb *= textureGate;
    vSoftness = frame.render.z;
    // 局部法线先乘逐粒子旋转，再乘当前朝向基（world/camera/velocity），
    // 与位置使用同一套变换，保证 Lambert 着色与粒子朝向一致。
    vNormal = ParticleMeshWorldNormal(
        particle,
        particleIndex,
        inNormal);
    vWorldPosition = worldPosition;
}
