#version 450

#include "particleShadowCasterCommon.glsl"
#include "particleSprite.glsl"

layout(location = 0) in vec3 vShadowWorldPosition;
layout(location = 1) in vec2 vShadowUV;
layout(location = 2) in vec2 vShadowAtlasUV;
layout(location = 3) in float vShadowSeed;
layout(location = 4) in float vShadowTime;
layout(location = 5) in float vShadowColorAlpha;

void main()
{
    // 与 particleRibbon.frag 相同的横截面软化：只收 vUV.y 两端，
    // 不做圆盘 radial 裁剪。
    float edgeWidth = mix(0.02, 0.5, clamp(frame.render.z, 0.0, 1.0));
    float edge = smoothstep(0.0, edgeWidth, vShadowUV.y) *
        smoothstep(0.0, edgeWidth, 1.0 - vShadowUV.y);

    float coverage;
    if (frame.flipbookParams.y > 0.5)
    {
        coverage = texture(particleTexture, vShadowAtlasUV).a * edge;
    }
    else
    {
        vec2 noiseUv = vec2(
            vShadowUV.x * 5.0,
            vShadowUV.y * 2.0) +
            vec2(vShadowSeed, -vShadowTime * 0.35);
        float erosion = smoothstep(
            0.18,
            0.82,
            ParticleShadowFbm(
                noiseUv + vec2(vShadowTime * 0.2, 0.0)));
        coverage = edge * mix(1.0, erosion, 0.6);
    }
    float alpha =
        coverage * vShadowColorAlpha * frame.colorTint.a;
    if (alpha < kParticleShadowAlphaThreshold)
    {
        discard;
    }

    float depth;
    if (!ParticleShadowCasterDepth(vShadowWorldPosition, depth))
    {
        discard;
    }
    gl_FragDepth = depth;
}
