#version 450

#include "decalCommon.glsl"

layout(location = 0) flat in int vInstanceIndex;

layout(location = 0) out vec4 outDBufferA;
layout(location = 1) out vec4 outDBufferB;
layout(location = 2) out vec4 outDBufferC;

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

    // DBuffer A: baseColor + color coverage；DBuffer C: roughness/metallic/AO
    // + surface coverage；DBuffer B 暂时不写 normal coverage。
    outDBufferA = vec4(
        instance.color.rgb * mottling,
        alpha);
    outDBufferC = vec4(
        instance.surface.xyz,
        alpha);
    // DBufferB 约定存编码后的 world normal（0..1）；当前源没有
    // normal-capable decal 材质，normal.w 仍为 0。
    outDBufferB = vec4(
        instance.normal.xyz * 0.5 + 0.5,
        instance.normal.w);
}
