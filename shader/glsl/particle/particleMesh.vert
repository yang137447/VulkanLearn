#version 450

#include "particleCommon.glsl"
#include "particleStorageRO.glsl"
#include "particleSorted.glsl"
#include "particleAlive.glsl"
#include "particleMeshTransform.glsl"
#include "particleSkeletalSkin.glsl"

layout(location = 0) in vec3 inPosition;
layout(location = 1) in vec3 inNormal;
layout(location = 2) in vec4 inColor;
layout(location = 3) in vec2 inTexCoord;
// Source meshes may omit tangents; the fragment shader then reconstructs the
// basis from screen-space derivatives of world position and UV.
layout(location = 4) in vec4 inTangent;

layout(location = 0) out vec4 vColor;
layout(location = 1) out float vSoftness;
layout(location = 2) out float vViewDepth;
// 世界空间法线；仅 particleMesh.frag 的方向光 Lambert 使用。
layout(location = 4) out vec3 vNormal;
// 世界空间位置；仅 mesh frag 的 CSM 级联选择使用。
layout(location = 5) out vec3 vWorldPosition;
// 世界空间切线（xyz）与 MikkTSpace handedness（sign）；无法解析时长度为 0。
layout(location = 6) out vec3 vTangent;
layout(location = 7) out float vTangentSign;
layout(location = 8) out vec2 vUv;
layout(location = 9) out float vLifeT;
layout(location = 10) out float vSeed;

// 与 vertex descriptor 的 billboard/ribbon 路径共用 binding 2，保证
// descriptor set layout 完全一致；mesh v1 不采样纹理。
layout(set = 0, binding = 2) uniform sampler2D particleTexture;
// mesh 专用材质 set：数值 UV 变换与四张贴图。只有 mesh 管线创建该 set，
// billboard/ribbon/beam 继续使用不含 set 1 的共享 layout。
layout(std140, set = 1, binding = 0) uniform ParticleMaterialUniform
{
    // xy = uvScale, zw = uvOffset
    vec4 uvScaleOffset;
    // x = hasAlbedo, y = hasNormal, z = hasOrm, w = hasEmissive
    vec4 textureFlags;
    // x = alphaClipThreshold, y = ambientOcclusion, z = normalStrength
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
    vTangent = vec3(0.0);
    vTangentSign = 1.0;
    vUv = vec2(0.0);
    vLifeT = 0.0;
    vSeed = 0.0;
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
    vec3 localPosition = inPosition;
    vec3 localNormal = inNormal;
    vec4 localTangent = inTangent;
#if defined(VL_SKINNED_MESH) && VL_SKINNED_MESH
    ParticleSkinVertex(
        inPosition,
        inNormal,
        inTangent,
        localPosition,
        localNormal,
        localTangent);
#endif
    localPosition *= frame.meshScale.xyz;
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
        localNormal);
    vWorldPosition = worldPosition;
    // UV 变换与贴图采样只在 mesh 材质 set 中存在；无 MI 时是恒等变换。
    vUv = inTexCoord * material.uvScaleOffset.xy +
        material.uvScaleOffset.zw;
    vLifeT = ParticleMeshLifeT(particle);
    vSeed = fract(float(particleIndex) * 0.61803398875);
    float tangentLength = length(localTangent.xyz);
    vTangent = tangentLength > 1.0e-5
        ? ParticleMeshWorldTangent(
            particle,
            particleIndex,
            localTangent.xyz / tangentLength)
        : vec3(0.0);
    vTangentSign = localTangent.w < 0.0 ? -1.0 : 1.0;
    // 溶解把面片推离以粒子重心为中心的球面：中心点位移为 0，边缘按
    // dissolve 参数抬升，保证溶解后轮廓向外扩张而不是整体平移。
    // CPU 已逐帧算出溶解外壳推进参数（puffParams），这里只做一次缩放；
    // 未启用溶解时 z=0，等价于原顶点位置。范围由 CPU 保证有界。
    vec3 dissolvePosition = localPosition *
        (1.0 + (frame.puffParams.z > 0.5 ? frame.puffParams.y : 0.0));
    bool puffMoved = dissolvePosition != localPosition;
    if (puffMoved)
    {
        vec3 pushedPosition = ParticleMeshWorldPosition(
            particle,
            particleIndex,
            dissolvePosition);
        gl_Position = frame.viewProjection * vec4(pushedPosition, 1.0);
        vViewDepth = -(frame.view * vec4(pushedPosition, 1.0)).z;
        vWorldPosition = pushedPosition;
    }
}
