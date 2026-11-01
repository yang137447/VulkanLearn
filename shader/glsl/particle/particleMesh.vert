#version 450

#include "particleCommon.glsl"
#include "particleStorageRO.glsl"
#include "particleSorted.glsl"
#include "particleAlive.glsl"

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

mat3 RotateAroundAxis(vec3 axis, float angle)
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

void main()
{
    uint count = uint(particleBuffer.particles.length());
    uint drawIndex = uint(gl_InstanceIndex);
    if (drawIndex >= count)
    {
        CullVertex();
        return;
    }

    // 与 billboard/ribbon 相同的排序映射：instance 是排序槽位，
    // pair.y 才是原始粒子池索引。
    uint particleIndex = drawIndex;
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
        CullVertex();
        return;
    }

    Particle particle = particleBuffer.particles[particleIndex];
    if (!IsParticleAlive(particle))
    {
        CullVertex();
        return;
    }

    float lifeT = clamp(
        particle.positionAge.w /
            max(particle.velocityLife.w, 1.0e-4),
        0.0,
        1.0);

    // 非 localSpace 的模拟位置/速度在 spawn 时已经变换到世界空间；
    // mesh v1 拒绝 localSpace，因此这里不再重复乘 localToWorld。
    vec3 center = particle.positionAge.xyz;
    vec3 cameraForward = normalize(cross(
        frame.cameraUp.xyz,
        frame.cameraRight.xyz));
    mat3 cameraBasis = mat3(
        frame.cameraRight.xyz,
        frame.cameraUp.xyz,
        -cameraForward);

    int alignment = int(frame.meshParams.x + 0.5);
    mat3 basis = mat3(frame.localToWorld);
    if (alignment == 1)
    {
        basis = cameraBasis;
    }
    else if (alignment == 2)
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
                basis = mat3(right, up, forward);
            }
            else
            {
                basis = cameraBasis;
            }
        }
        else
        {
            basis = cameraBasis;
        }
    }

    // v1 的粒子 rotation 绕朝向基的局部 Y 轴（mesh up）旋转；
    // rotationRandom 使用稳定原始粒子索引相位。
    float rotationDegrees =
        frame.rotationParams.x +
        frame.rotationParams.y * particle.positionAge.w;
    if (frame.rotationParams.z > 0.5)
    {
        rotationDegrees += fract(float(particleIndex) * 0.61803398875) *
            360.0;
    }
    vec3 rotationAxis = normalize(basis[1]);
    mat3 rotation = RotateAroundAxis(
        rotationAxis,
        radians(rotationDegrees));
    vec3 localPosition = inPosition * frame.meshScale.xyz;
    vec3 worldPosition =
        center + rotation * basis * localPosition;
    gl_Position = frame.viewProjection * vec4(worldPosition, 1.0);
    vViewDepth = -(frame.view * vec4(worldPosition, 1.0)).z;

    vColor = EvaluateColorCurve(lifeT) * inColor;
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
    vNormal = normalize(rotation * basis * inNormal);
    vWorldPosition = worldPosition;
}
