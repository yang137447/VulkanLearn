#version 450

#include "decalCommon.glsl"

layout(location = 0) flat in int vInstanceIndex;

layout(location = 0) out vec4 outGBufferC;

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

    float projectionFade =
        DecalProjectionFade(local, instance.params.y);
    if (projectionFade <= 0.0)
        discard;

    vec2 decalUv = DecalLocalToUV(local);
    vec2 centered = decalUv * 2.0 - 1.0;
    float radial = 1.0 - smoothstep(0.35, 1.0, length(centered));
    float noise = DecalFbm(
        decalUv * 7.0 + vec2(instance.params.w, 0.0));
    float mottling = mix(1.0, noise, clamp(instance.params.z, 0.0, 1.0));
    float alpha = instance.params.x * projectionFade *
        radial * clamp(0.35 + 0.65 * mottling, 0.0, 1.0);
    if (alpha <= 0.001)
        discard;

    // 输出 alpha 只参与固定功能混合；pipeline 的 colorWriteMask 会保留
    // GBufferC.a（AO），不会把 AO 写成 decal opacity。
    outGBufferC = vec4(
        instance.color.rgb * mottling,
        alpha);
}
