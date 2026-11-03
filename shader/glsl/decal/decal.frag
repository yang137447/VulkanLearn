#version 450

#include "decalCommon.glsl"

layout(location = 0) flat in int vInstanceIndex;

layout(location = 0) out vec4 outDBufferA;
layout(location = 1) out vec4 outDBufferB;
layout(location = 2) out vec4 outDBufferC;
layout(location = 3) out vec4 outDBufferD;
layout(location = 4) out vec4 outDBufferMask;

layout(set = 3, binding = 0) uniform sampler2D sceneDepth;

void main()
{
    vec2 screenSize = vec2(textureSize(sceneDepth, 0));
    vec2 screenUv = gl_FragCoord.xy / screenSize;
    vec3 worldPosition =
        DecalReconstructWorldPosition(sceneDepth, screenUv);

    DecalInstance instance =
        decalInstances.instances[vInstanceIndex];
    vec4 localH =
        instance.worldToDecal * vec4(worldPosition, 1.0);
    vec3 local = localH.xyz / localH.w;
    if (any(greaterThan(abs(local), vec3(1.0))))
        discard;

    // params.x = edge softness，params.y = noise strength，params.z = seed；
    // surface.w = 作者 opacity。不要把这些分量按旧版紧凑布局换位使用。
    float projectionFade =
        DecalProjectionFade(local, instance.params.x);
    if (projectionFade <= 0.0)
        discard;

    vec2 decalUv = DecalLocalToUV(local);
    vec2 centered = decalUv * 2.0 - 1.0;
    float radial = 1.0 - smoothstep(0.35, 1.0, length(centered));
    float noise = DecalFbm(
        decalUv * 7.0 + vec2(instance.params.z, 0.0));
    float mottling = mix(1.0, noise, clamp(instance.params.y, 0.0, 1.0));
    float alpha = instance.surface.w * projectionFade *
        radial * clamp(0.35 + 0.65 * mottling, 0.0, 1.0);
    if (alpha <= 0.001)
        discard;

    // params.w 是作者 DecalResponse 位掩码；每个通道控制对应 DBuffer
    // 是否允许影响接收表面。
    vec4 response = DecalDecodeResponseMask(instance.params.w);

    // 程序化法线：局部 X 是投影深度，局部 (z,y) 是贴花平面 UV。
    // 用有限差分从 FBM 高度场求切平面法线，再经 decalToWorld 的
    // 逆转置变换到世界空间（法线不做平移）。
    float seed = instance.params.z;
    float eps = 1.0 / 256.0;
    float height = DecalFbm(decalUv * 7.0 + vec2(seed));
    float heightU = DecalFbm(
        (decalUv + vec2(eps, 0.0)) * 7.0 + vec2(seed));
    float heightV = DecalFbm(
        (decalUv + vec2(0.0, eps)) * 7.0 + vec2(seed));
    float dHdU = (heightU - height) / eps;
    float dHdV = (heightV - height) / eps;
    // normalStrength 直接缩放坡度；0 时退化为投影体平坦法线。
    float bumpScale = instance.color.w * 0.12;
    vec3 localNormal = normalize(vec3(
        -1.0,
        -dHdV * bumpScale,
        -dHdU * bumpScale));
    mat3 normalMatrix = transpose(inverse(mat3(instance.decalToWorld)));
    vec3 worldNormal = normalize(normalMatrix * localNormal);

    // 每个 decal 只写自己 response 允许的缓冲区；否则重叠时另一个
    // decal 的 mask 位会把本 decal 的默认通道值泄漏给接收表面。
    // A: baseColor + color coverage；B: 编码 world normal + normal
    // coverage；C: roughness/AO + roughness coverage；D: metallic +
    // metallic coverage。
    outDBufferA = vec4(
        instance.color.rgb * mottling,
        alpha * response.r);
    outDBufferC = vec4(
        instance.surface.r,
        instance.surface.b,
        0.0,
        alpha * response.b);
    outDBufferD = vec4(
        instance.surface.g,
        0.0,
        0.0,
        alpha * response.a);
    // normalStrength 为 0 时不写 normal coverage，避免平坦法线仍覆盖
    // 接收表面；大于 0 时 normal coverage 与颜色/表面覆盖率一致。
    float normalEnabled = step(1.0e-4, instance.color.w);
    outDBufferB = vec4(
        worldNormal * 0.5 + 0.5,
        alpha * response.g * normalEnabled);
    // 掩码只存 0/1 位，不乘 alpha；不透明但也想改 roughness 的
    // decal 不应因覆盖率衰减而丢失响应位。
    outDBufferMask = response;
}
