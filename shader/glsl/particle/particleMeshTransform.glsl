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
    particle.positionAge = vec4(0.0);
    particle.velocityLife = vec4(0.0);
    for (int historyIndex = 0; historyIndex < 16; ++historyIndex)
    {
        particle.history[historyIndex] = vec4(0.0);
    }
    particle.source0 = vec4(0.0);
    particle.source1 = vec4(0.0);
    particle.source2 = vec4(0.0);

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

// localSpace 粒子的模拟位置是发射器局部坐标，必须先整体变换到世界；
// 非 localSpace 保持不变。
vec3 ParticleMeshWorldCenter(Particle particle)
{
    return frame.shape.z > 0.5
        ? (frame.localToWorld *
            vec4(particle.positionAge.xyz, 1.0)).xyz
        : particle.positionAge.xyz;
}

// mesh size 曲线乘数：lifeT=0 处归一化为 1，sizeScale 是作者倍率。
float ParticleMeshSizeScale(float lifeT)
{
    if (frame.meshParams.z < 0.5)
    {
        return 1.0;
    }
    float base = EvaluateSizeCurve(0.0);
    if (!(base > 1.0e-5))
    {
        base = 1.0;
    }
    return EvaluateSizeCurve(lifeT) / base * frame.meshScale.w;
}

// camera/velocity 朝向不消费 localToWorld 作为朝向基；localSpace 时
// 仍需要模型线性部分把网格局部偏移/方向带到世界。
vec3 ParticleMeshLocalOffset(vec3 localPosition)
{
    if (frame.shape.z > 0.5 &&
        int(frame.meshParams.x + 0.5) != 0)
    {
        return mat3(frame.localToWorld) * localPosition;
    }
    return localPosition;
}

vec3 ParticleMeshLocalDirection(vec3 localDirection)
{
    return ParticleMeshLocalOffset(localDirection);
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
        if (frame.shape.z > 0.5)
        {
            forward = mat3(frame.localToWorld) * forward;
        }
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
    return ParticleMeshWorldCenter(particle) +
        rotation * basis * ParticleMeshLocalOffset(localPosition);
}

vec3 ParticleMeshWorldNormal(
    Particle particle,
    uint particleIndex,
    vec3 localNormal)
{
    mat3 basis = ParticleMeshOrientation(particle);
    mat3 rotation = ParticleMeshRotation(particle, particleIndex);
    return normalize(rotation * basis *
        ParticleMeshLocalDirection(localNormal));
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
    return rotation * basis *
        ParticleMeshLocalDirection(localTangent);
}

#endif
