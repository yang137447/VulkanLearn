#version 450

#include "particleMeshTransform.glsl"

layout(location = 0) in vec3 inPosition;

layout(location = 0) out vec3 vShadowWorldPosition;

void main()
{
    uint particleIndex = 0;
    Particle particle;
    if (!ResolveParticleMeshParticle(
            uint(gl_InstanceIndex),
            particleIndex,
            particle))
    {
        // 与可见 mesh pass 相同：把失效实例推到一个必然被裁剪的位置，
        // 不写任何深度。
        gl_Position = vec4(0.0, 0.0, 2.0, 1.0);
        vShadowWorldPosition = vec3(0.0);
        return;
    }

    vec3 localPosition = inPosition * frame.meshScale.xyz;
    vec3 worldPosition =
        ParticleMeshWorldPosition(particle, particleIndex, localPosition);
    gl_Position = frame.shadowViewProj * vec4(worldPosition, 1.0);
    vShadowWorldPosition = worldPosition;
}
