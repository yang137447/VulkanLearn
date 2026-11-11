#version 450

#include "particleCommon.glsl"
#include "particleSceneLighting.glsl"
#include "particleSoftDepth.glsl"

layout(location = 0) in vec4 vColor;
layout(location = 1) in vec2 vUV;
layout(location = 2) in float vSoftness;
layout(location = 3) in float vSeed;
layout(location = 4) in float vTime;
layout(location = 5) in vec2 vAtlasUV;
layout(location = 6) in float vViewDepth;
layout(location = 7) in flat int vTextureIndex;
layout(location = 8) in vec2 vSpriteUV;
layout(location = 9) in vec2 vSoftParams;
layout(location = 10) in flat float vHasTexture;
layout(location = 11) in vec3 vWorldPosition;
layout(location = 12) in flat vec4 vLightParams;
layout(location = 13) in flat vec3 vLightDirection;
layout(location = 14) in flat vec3 vLightColor;
layout(location = 15) in flat vec3 vLightingNormal;
layout(location = 16) in flat vec4 vLightParams2;
layout(location = 17) in vec2 vAtlasUV2;
layout(location = 18) in float vAtlasBlend;
layout(location = 19) in float vSize;
layout(location = 20) in float vDensity;
layout(location = 21) in float vSoftTapRadius;
layout(location = 22) in float vEooPacked;
layout(location = 23) in float vEooEmissiveStrength;
layout(location = 24) in flat float vKind;
layout(location = 25) in flat float vSoftThickness;

layout(location = 0) out vec4 outColor;

// 与 per-emitter billboard 相同的 set 3 约定：0=sceneDepth，1=shadowMap。
// 全局路径 v1 明确不消费 shadowMap，但仍声明以保持 descriptor layout 兼容。
layout(set = 3, binding = 0) uniform sampler2D sceneDepth;
layout(set = 3, binding = 1) uniform sampler2DArrayShadow shadowMap;
// 全局 alpha 池不消费 CSM，但它与逐发射器 billboard 共用 material/descriptor
// 布局；局部光阴影仍按 set 3 binding 2 声明并参与点光/聚光采样。
layout(set = 3, binding = 2) uniform sampler2DArrayShadow localShadowMap;
layout(set = 3, binding = 3) uniform sampler2D softDepth;

layout(set = 0, binding = 3) uniform sampler2D particleTextures[8];
layout(std430, set = 0, binding = 5) buffer ParticleDebug
{
    uint fragments;
    uint reserved;
} particleDebug;

// 与 vertex 阶段相同：固定索引分支替代 sampled-image 动态索引。
vec4 SampleParticleTexture(int index, vec2 uv)
{
    if (index <= 0) return texture(particleTextures[0], uv);
    if (index == 1) return texture(particleTextures[1], uv);
    if (index == 2) return texture(particleTextures[2], uv);
    if (index == 3) return texture(particleTextures[3], uv);
    if (index == 4) return texture(particleTextures[4], uv);
    if (index == 5) return texture(particleTextures[5], uv);
    if (index == 6) return texture(particleTextures[6], uv);
    return texture(particleTextures[7], uv);
}

float Hash21(vec2 p)
{
    p = fract(p * vec2(123.34, 456.21));
    p += dot(p, p + 45.32);
    return fract(p.x * p.y);
}

float ValueNoise(vec2 p)
{
    vec2 i = floor(p);
    vec2 f = fract(p);
    vec2 u = f * f * (3.0 - 2.0 * f);
    float a = Hash21(i);
    float b = Hash21(i + vec2(1.0, 0.0));
    float c = Hash21(i + vec2(0.0, 1.0));
    float d = Hash21(i + vec2(1.0, 1.0));
    return mix(mix(a, b, u.x), mix(c, d, u.x), u.y);
}

float Fbm(vec2 p)
{
    float value = 0.0;
    float amplitude = 0.55;
    for (int octave = 0; octave < 4; ++octave)
    {
        value += ValueNoise(p) * amplitude;
        p = p * 2.03 + vec2(17.1, 9.2);
        amplitude *= 0.5;
    }
    return value;
}

void main()
{
    float sceneViewDepth = SampleParticleSoftViewDepth(
        softDepth,
        sceneDepth,
        gl_FragCoord.xy,
        vSoftTapRadius,
        frame.invProjection,
        vSoftThickness);
    if (vViewDepth >= sceneViewDepth)
    {
        discard;
    }

    float softFade = 1.0;
    if (vSoftParams.y > 0.5)
    {
        softFade = ParticleSoftFade(
            sceneViewDepth - vViewDepth,
            vSoftParams.x,
            vSoftThickness);
    }

    if (vKind > 0.5)
    {
        // Ribbon：矩形横截面只软化 v 两端，不做 billboard 圆盘裁剪。
        float edgeWidth = mix(
            0.02, 0.5, clamp(vSoftness, 0.0, 1.0));
        float edge = smoothstep(0.0, edgeWidth, vUV.y) *
            smoothstep(0.0, edgeWidth, 1.0 - vUV.y);
        float alpha = vColor.a * edge;
        vec3 albedo = vColor.rgb;
        vec3 emission = vec3(0.0);
        if (vHasTexture > 0.5)
        {
            vec4 sprite = SampleParticleTexture(
                clamp(vTextureIndex, 0, 7), vAtlasUV);
            if (vEooPacked > 0.5)
            {
                albedo = vColor.rgb * mix(1.0, sprite.g, 0.35);
                emission = vColor.rgb * sprite.r *
                    vEooEmissiveStrength;
                if (vLightParams.x < 0.5)
                {
                    albedo = vec3(0.0);
                }
            }
            else
            {
                albedo = vColor.rgb * sprite.rgb;
            }
            alpha *= sprite.a;
        }
        else
        {
            vec2 noiseUv = vec2(vUV.x * 5.0, vUV.y * 2.0) +
                vec2(vSeed, -vTime * 0.35);
            float erosion = smoothstep(
                0.18,
                0.82,
                Fbm(noiseUv + vec2(vTime * 0.2, 0.0)));
            alpha *= mix(1.0, erosion, 0.6);
        }
        if (vLightParams.x > 0.5)
        {
            albedo = CalculateParticleBillboardSceneLighting(
                localShadowMap,
                albedo,
                vWorldPosition,
                vLightingNormal,
                frame.cameraPosition.xyz,
                clamp(vLightParams.y, 0.0, 1.0),
                clamp(vLightParams.z, 0.0, 1.0),
                clamp(vLightParams2.x, 0.0, 1.0),
                clamp(vLightParams2.y, 0.02, 1.0),
                clamp(vLightParams2.z, 0.0, 2.0),
                1.0,
                vSize,
                vDensity);
        }
        outColor = vec4(albedo + emission, alpha * softFade);
        if (outColor.a > 0.0)
            atomicAdd(particleDebug.fragments, 1u);
        return;
    }

    float radius = length(vUV * 2.0 - 1.0);
    if (radius >= 1.0)
    {
        discard;
    }
    float radial = pow(
        1.0 - radius,
        mix(1.0, 3.0, clamp(vSoftness, 0.0, 1.0)));
    int textureIndex = clamp(vTextureIndex, 0, 7);
    vec3 albedo = vColor.rgb;
    vec3 emission = vec3(0.0);
    float alpha = vColor.a * radial;
    if (vHasTexture > 0.5)
    {
        vec4 sprite = mix(
            SampleParticleTexture(textureIndex, vAtlasUV),
            SampleParticleTexture(textureIndex, vAtlasUV2),
            vAtlasBlend);
        if (vEooPacked > 0.5)
        {
            // R=Emissive, G=Occlusion, A=Opacity；B 通道不可用。
            albedo = vColor.rgb * mix(1.0, sprite.g, 0.35);
            emission = vColor.rgb * sprite.r * vEooEmissiveStrength;
            if (vLightParams.x < 0.5)
            {
                albedo = vec3(0.0);
            }
        }
        else
        {
            albedo = vColor.rgb * sprite.rgb;
        }
        alpha *= sprite.a;
    }
    else
    {
        vec2 noiseUv = vUV * 5.0 + vec2(vSeed, -vTime * 0.35);
        float erosion = smoothstep(
            0.18,
            0.82,
            Fbm(noiseUv + vec2(vTime * 0.2, 0.0)));
        alpha *= mix(1.0, erosion, 0.72);
    }
    if (vLightParams.x > 0.5)
    {
        // 全局池当前没有逐发射器 CSM 绑定（v1 明确不消费 shadowMap），
        // 因此这里只做无阴影的完整场景光照；开关关闭时保持 unlit 原输出。
        albedo = CalculateParticleBillboardSceneLighting(
            localShadowMap,
            albedo,
            vWorldPosition,
            vLightingNormal,
            frame.cameraPosition.xyz,
            clamp(vLightParams.y, 0.0, 1.0),
            clamp(vLightParams.z, 0.0, 1.0),
            clamp(vLightParams2.x, 0.0, 1.0),
            clamp(vLightParams2.y, 0.02, 1.0),
            clamp(vLightParams2.z, 0.0, 2.0),
            1.0,
            vSize,
            vDensity);
    }
    outColor = vec4(albedo + emission, alpha * softFade);
    if (outColor.a > 0.0)
        atomicAdd(particleDebug.fragments, 1u);
}
