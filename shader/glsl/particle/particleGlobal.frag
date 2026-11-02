#version 450

#include "particleCommon.glsl"

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

layout(location = 0) out vec4 outColor;

// 与 per-emitter billboard 相同的 set 3 约定：0=sceneDepth，1=shadowMap。
// 全局路径 v1 明确不消费 shadowMap，但仍声明以保持 descriptor layout 兼容。
layout(set = 3, binding = 0) uniform sampler2D sceneDepth;
layout(set = 3, binding = 1) uniform sampler2DArrayShadow shadowMap;

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
    float deviceDepth = texture(
        sceneDepth,
        gl_FragCoord.xy / vec2(textureSize(sceneDepth, 0))).r;
    vec4 sceneView =
        frame.invProjection * vec4(0.0, 0.0, deviceDepth, 1.0);
    float sceneViewDepth = -sceneView.z / sceneView.w;
    if (vViewDepth >= sceneViewDepth)
    {
        discard;
    }

    float softFade = 1.0;
    if (vSoftParams.y > 0.5)
    {
        softFade = clamp(
            (sceneViewDepth - vViewDepth) /
                max(vSoftParams.x, 0.001),
            0.0,
            1.0);
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
    if (vHasTexture > 0.5)
    {
        vec4 sprite = SampleParticleTexture(textureIndex, vAtlasUV);
        outColor = vec4(
            vColor.rgb * sprite.rgb,
            vColor.a * sprite.a * radial);
    }
    else
    {
        vec2 noiseUv = vUV * 5.0 + vec2(vSeed, -vTime * 0.35);
        float erosion = smoothstep(
            0.18,
            0.82,
            Fbm(noiseUv + vec2(vTime * 0.2, 0.0)));
        outColor = vec4(
            vColor.rgb,
            vColor.a * radial * mix(1.0, erosion, 0.72));
    }
    outColor.a *= softFade;
    outColor.rgb = EvaluateParticleBillboardLighting(
        outColor.rgb,
        vWorldPosition,
        vLightingNormal,
        vLightParams,
        vLightDirection,
        vLightColor,
        0.0,
        shadowMap);
    if (outColor.a > 0.0)
        atomicAdd(particleDebug.fragments, 1u);
}
