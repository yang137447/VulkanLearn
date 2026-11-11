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
        int(particle.source0.w + 0.5) != 2)
    {
        CullVertex();
        return;
    }
    float skeletalLifeT = ParticleMeshLifeT(particle);
    float sizeScale = ParticleMeshSizeScale(skeletalLifeT);
    vec3 localStart = particle.source0.xyz *
        frame.meshScale.xyz * frame.lodParams.x * sizeScale;
    vec3 localEnd = particle.source1.xyz *
        frame.meshScale.xyz * frame.lodParams.x * sizeScale;
    // 与三角面源一致：source 是模型空间偏移；localSpace 时粒子中心
    // 也必须先整体变换到世界。
    mat3 modelLinear = mat3(frame.localToWorld);
    vec3 center = ParticleMeshWorldCenter(particle);
    vec3 startWorld =
        center + modelLinear * localStart;
    vec3 endWorld =
        center + modelLinear * localEnd;
    vec3 tangent = endWorld - startWorld;
    float tangentLength = length(tangent);
    vec3 view = normalize(frame.cameraPosition.xyz -
        (startWorld + endWorld) * 0.5);
    vec3 side = tangentLength > 1.0e-5
        ? normalize(cross(tangent, view))
        : frame.cameraRight.xyz;
    vec2 corner = kParticleCorners[uint(gl_VertexIndex) % 6u];
    float along = corner.x * 0.5 + 0.5;
    // meshParams.y 是作者化骨段半径（局部 cm）；localSpace 还要乘模型缩放。
    float modelScale = frame.shape.z > 0.5
        ? max(length(modelLinear[0]),
            max(length(modelLinear[1]), length(modelLinear[2])))
        : 1.0;
    float radius = max(frame.meshParams.y, 0.01) *
        frame.lodParams.x * sizeScale * modelScale;
    vec3 worldPosition =
        mix(startWorld, endWorld, along) + side * (corner.y * radius);
    gl_Position = frame.viewProjection * vec4(worldPosition, 1.0);
    vViewDepth = -(frame.view * vec4(worldPosition, 1.0)).z;
    vColor = EvaluateColorCurve(skeletalLifeT);
    vColor.a *= frame.render.w;
    vSoftness = frame.render.z;
    // 骨段是圆柱薄片：两条对角三角形翻转绕序，双面朝向在片元阶段处理
    // （顶点阶段没有 gl_FrontFacing）。法线取切线与视线构成平面内的垂线，
    // 再向视线微微偏置，避免正对相机时法线退化。
    vec3 segmentAxis = tangentLength > 1.0e-5
        ? tangent / tangentLength
        : normalize(view);
    vec3 flatNormal = cross(side, segmentAxis);
    flatNormal = dot(flatNormal, flatNormal) > 1.0e-10
        ? normalize(flatNormal)
        : frame.cameraRight.xyz;
    vNormal = normalize(flatNormal + view * 1.0e-3);
    vWorldPosition = worldPosition;
    vUv = vec2(along, corner.y * 0.5 + 0.5);
    vLifeT = skeletalLifeT;
    vSeed = fract(float(particleIndex) * 0.61803398875);
}
