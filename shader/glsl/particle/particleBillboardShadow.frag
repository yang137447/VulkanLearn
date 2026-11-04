#version 450

#include "particleShadowCasterCommon.glsl"
#include "particleSprite.glsl"

layout(location = 0) in vec3 vShadowWorldPosition;
layout(location = 1) in vec2 vShadowLocalUV;
layout(location = 2) in vec2 vShadowAtlasUV;
layout(location = 3) in float vShadowSeed;
layout(location = 4) in float vShadowTime;
layout(location = 5) in float vShadowColorAlpha;

void main()
{
    // 形状覆盖率与可见 billboard 对齐：有纹理采样 alpha，无纹理用
    // 软圆盘 + FBM；再乘粒子颜色 alpha 与实例 tint。
    float coverage;
    if (frame.flipbookParams.y > 0.5)
    {
        coverage = texture(particleTexture, vShadowAtlasUV).a *
            ParticleShadowRadialAlpha(vShadowLocalUV, frame.render.z);
    }
    else
    {
        coverage = ParticleShadowProceduralAlpha(
            vShadowLocalUV,
            frame.render.z,
            vShadowSeed,
            vShadowTime);
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
