#version 450

#include "decalCommon.glsl"

layout(location = 0) flat in int vInstanceIndex;

layout(location = 0) out vec4 outDBufferA;
layout(location = 1) out vec4 outDBufferB;
layout(location = 2) out vec4 outDBufferC;
layout(location = 3) out vec4 outDBufferD;
layout(location = 4) out vec4 outDBufferMask;

// 代码维护的 decal 贴图槽位（没有 Material Graph）。Synchronize 保证三个
// binding 始终有合法纹理；缺失时绑定 1x1 默认值。
layout(set = 1, binding = 0) uniform sampler2D albedoMap;
layout(set = 1, binding = 1) uniform sampler2D normalMap;
layout(set = 1, binding = 2) uniform sampler2D ormMap;

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
    vec4 albedoSample = texture(albedoMap, decalUv);
    float alpha = instance.surface.w * projectionFade *
        radial * clamp(0.35 + 0.65 * mottling, 0.0, 1.0) *
        albedoSample.a;
    if (alpha <= 0.001)
        discard;

    // params.w 是作者 DecalResponse 位掩码；每个通道控制对应 DBuffer
    // 是否允许影响接收表面。
    vec4 response = DecalDecodeResponseMask(instance.params.w);

    // 局部 X 是投影深度，局部 (z,y) 是贴花平面 UV；两种法线来源最终都
    // 换算到同一局部基，再经 decalToWorld 的逆转置进入世界空间。
    vec3 localNormal;
    if (instance.normal.w > 0.5)
    {
        // 切线空间法线：xy 受 normalStrength 缩放，z 保持贴图强度。
        // 贴花平面 UV = (z,y)，因此 tangent.x -> local.z，
        // tangent.y -> local.y，tangent.z -> local.-x。
        vec3 tangentNormal =
            texture(normalMap, decalUv).xyz * 2.0 - 1.0;
        tangentNormal.xy *=
            clamp(instance.color.w, 0.0, 1.0);
        tangentNormal = normalize(tangentNormal);
        localNormal = vec3(
            -tangentNormal.z,
            tangentNormal.y,
            tangentNormal.x);
    }
    else
    {
        // 程序化法线：用有限差分从 FBM 高度场求切平面法线。
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
        localNormal = normalize(vec3(
            -1.0,
            -dHdV * bumpScale,
            -dHdU * bumpScale));
    }
    mat3 normalMatrix = transpose(inverse(mat3(instance.decalToWorld)));
    vec3 worldNormal = normalize(normalMatrix * localNormal);

    // ORM 约定：R=roughness，G=metallic，B=ambientOcclusion；作者值
    // 与贴图逐通道相乘，默认白图保持作者值。
    vec3 ormSample = texture(ormMap, decalUv).rgb;
    float roughness = instance.surface.r * ormSample.r;
    float metallic = instance.surface.g * ormSample.g;
    float ambientOcclusion = instance.surface.b * ormSample.b;

    // 每个 decal 只写自己 response 允许的缓冲区；否则重叠时另一个
    // decal 的 mask 位会把本 decal 的默认通道值泄漏给接收表面。
    // A: baseColor + color coverage；B: world normal + normal coverage；
    // C: roughness/AO + roughness coverage；D: metallic + metallic coverage。
    outDBufferA = vec4(
        instance.color.rgb * albedoSample.rgb * mottling,
        alpha * response.r);
    outDBufferC = vec4(
        roughness,
        ambientOcclusion,
        0.0,
        alpha * response.b);
    outDBufferD = vec4(
        metallic,
        0.0,
        0.0,
        alpha * response.a);
    // DBufferB 是 R16G16B16A16_SFLOAT，直接写带符号的 world normal，
    // 由固定功能 SrcAlpha/OneMinusSrcAlpha 把它预乘成 Σ N_i·w_i；
    // 合成端因此不需要 0.5 偏移编码，也不需要除以覆盖率恢复方向。
    // normalStrength 为 0 时 coverage 为 0，RGB 不参与混合。
    outDBufferB = vec4(
        worldNormal,
        DecalNormalWriteCoverage(alpha * response.g, instance.color.w));
    // 掩码只存 0/1 位，不乘 alpha；不透明但也想改 roughness 的
    // decal 不应因覆盖率衰减而丢失响应位。
    outDBufferMask = response;
}
