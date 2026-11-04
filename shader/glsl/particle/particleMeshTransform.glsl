#ifndef PARTICLE_MESH_TRANSFORM_GLSL
#define PARTICLE_MESH_TRANSFORM_GLSL

#include "particleCommon.glsl"
#include "particleStorageRO.glsl"
#include "particleSorted.glsl"
#include "particleAlive.glsl"

// mesh 粒子的可见实例解析与实例变换只保留这一份：可见性 pass
// (particleMesh.vert) 与阴影 caster pass (particleShadow.vert) 必须生成
// 完全一致的世界位置，否则阴影会与可见网格错位。
mat3 ParticleMeshRotateAroundAxis(vec3 axis, float angle)
{
    float s = sin(angle);
    float c = cos(angle);
    float oneMinusC = 1.0 - c;
    return mat3(
        c + axis.x * axis.x * oneMinusC,
        axis.y * axis.x * oneMinusC + axis.z * s,
        axis.z * axis.x * oneMinusC - axis.y * s,
        axis.x * axis.y * oneMinusC - axis.z * s,
        c + axis.y * axis.y * oneMinusC,
        axis.z * axis.y * oneMinusC + axis.x * s,
        axis.x * axis.z * oneMinusC + axis.y * s,
        axis.y * axis.z * oneMinusC - axis.x * s,
        c + axis.z * axis.z * oneMinusC);
}

// 排序开启时 instance 是排序槽位，pair.y 才是原始粒子池索引；关闭时
// aliveIndices 直接给出紧凑存活索引。两条路径都必须与 mesh 可见 pass 一致。
bool ResolveParticleMeshParticle(
    uint drawIndex,
    out uint particleIndex,
    out Particle particle)
{
    particleIndex = 0;
    particle = Particle(
        vec4(0.0),
        vec4(0.0),
        vec4(0.0),
        vec4(0.0),
        vec4(0.0));

    uint count = uint(particleBuffer.particles.length());
    if (drawIndex >= count)
    {
        return false;
    }

    particleIndex = drawIndex;
    if (frame.sortParams.x > 0.5)
    {
        particleIndex = uint(sortedPairs.pairs[drawIndex].y);
    }
    else
    {
        particleIndex = aliveIndices.indices[drawIndex];
    }
    if (particleIndex >= count)
    {
        return false;
    }

    particle = particleBuffer.particles[particleIndex];
    return IsParticleAlive(particle);
}

float ParticleMeshLifeT(Particle particle)
{
    return clamp(
        particle.positionAge.w /
            max(particle.velocityLife.w, 1.0e-4),
        0.0,
        1.0);
}

// 朝向基与 particleMesh.vert 的历史语义一致：world 用发射器 localToWorld，
// camera 用相机基，velocity 用速度方向；速度退化时回退相机基。
mat3 ParticleMeshOrientation(Particle particle)
{
    vec3 cameraForward = normalize(cross(
        frame.cameraUp.xyz,
        frame.cameraRight.xyz));
    mat3 cameraBasis = mat3(
        frame.cameraRight.xyz,
        frame.cameraUp.xyz,
        -cameraForward);

    int alignment = int(frame.meshParams.x + 0.5);
    if (alignment == 1)
    {
        return cameraBasis;
    }
    if (alignment == 2)
    {
        vec3 forward = particle.velocityLife.xyz;
        float forwardLength = length(forward);
        if (forwardLength > 1.0e-5)
        {
            forward /= forwardLength;
            vec3 right = cross(vec3(0.0, 1.0, 0.0), forward);
            float rightLength = length(right);
            if (rightLength > 1.0e-5)
            {
                right /= rightLength;
                vec3 up = normalize(cross(forward, right));
                return mat3(right, up, forward);
            }
        }
        return cameraBasis;
    }
    return mat3(frame.localToWorld);
}

// 逐粒子 rotation 绕朝向基的局部 Y 轴；random 相位使用稳定原始粒子索引，
// 因此排序或可见性变化不会让同一粒子突然换角度。
mat3 ParticleMeshRotation(Particle particle, uint particleIndex)
{
    mat3 basis = ParticleMeshOrientation(particle);
    float rotationDegrees =
        frame.rotationParams.x +
        frame.rotationParams.y * particle.positionAge.w;
    if (frame.rotationParams.z > 0.5)
    {
        rotationDegrees += fract(float(particleIndex) * 0.61803398875) *
            360.0;
    }
    return ParticleMeshRotateAroundAxis(
        normalize(basis[1]),
        radians(rotationDegrees));
}

vec3 ParticleMeshWorldPosition(
    Particle particle,
    uint particleIndex,
    vec3 localPosition)
{
    mat3 basis = ParticleMeshOrientation(particle);
    mat3 rotation = ParticleMeshRotation(particle, particleIndex);
    return particle.positionAge.xyz + rotation * basis * localPosition;
}

vec3 ParticleMeshWorldNormal(
    Particle particle,
    uint particleIndex,
    vec3 localNormal)
{
    mat3 basis = ParticleMeshOrientation(particle);
    mat3 rotation = ParticleMeshRotation(particle, particleIndex);
    return normalize(rotation * basis * localNormal);
}

// 切线使用与位置/法线完全相同的朝向基与逐粒子旋转；非均匀 meshScale 与
// 现有法线路径一样不做逆转置，保持既有近似语义。
vec3 ParticleMeshWorldTangent(
    Particle particle,
    uint particleIndex,
    vec3 localTangent)
{
    mat3 basis = ParticleMeshOrientation(particle);
    mat3 rotation = ParticleMeshRotation(particle, particleIndex);
    return rotation * basis * localTangent;
}

#endif
