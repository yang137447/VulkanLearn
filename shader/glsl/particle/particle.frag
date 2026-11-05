#version 450

#include "particleCommon.glsl"
#include "particleSprite.glsl"
#include "particleSceneLighting.glsl"
#include "particleSoftDepth.glsl"

layout(location = 0) in vec4 vColor;
layout(location = 1) in vec2 vUV;
layout(location = 2) in float vSoftness;
layout(location = 3) in float vSeed;
layout(location = 4) in float vTime;
layout(location = 5) in vec2 vAtlasUV;
layout(location = 6) in float vViewDepth;
layout(location = 7) in vec3 vWorldPosition;
layout(location = 8) in vec3 vLightingNormal;
layout(location = 9) in vec2 vAtlasUV2;
layout(location = 10) in float vAtlasBlend;
layout(location = 11) in float vSize;

layout(location = 0) out vec4 outColor;

// particle pass 只写 sceneColor；sceneDepth 是图输入，不能同时作为附件。
layout(set = 3, binding = 0) uniform sampler2D sceneDepth;
// 与 mesh 共用 particle pass 的 shadowMap 输入；billboard 默认 unlit，
// 只有 billboardShadows 开关打开时才按相机朝向法线调暗。
layout(set = 3, binding = 1) uniform sampler2DArrayShadow shadowMap;
// 局部光阴影图集：particle pass 的第三个 set 3 输入。点光/聚光按
// Light.shadowParams 选择 slice 与投影方式，见 common/localShadowSampling.glsl。
layout(set = 3, binding = 2) uniform sampler2DArrayShadow localShadowMap;
// overdraw 调试计数；只在片元最终写出时累加一次。
layout(std430, set = 0, binding = 5) buffer ParticleDebug
{
    uint fragments;
    uint reserved;
} particleDebug;

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
    // 像素坐标直接对应 sceneDepth 的 texel，不依赖 UV 翻转或 NDC 朝向。
    float sceneViewDepth = SampleParticleSceneViewDepth(
        sceneDepth,
        gl_FragCoord.xy,
        frame.softParams.z,
        frame.invProjection);

    // 被不透明几何遮挡的粒子直接剔除；软粒子开关只控制交线附近的 alpha 淡出。
    if (vViewDepth >= sceneViewDepth)
    {
        discard;
    }
    float softFade = 1.0;
    if (frame.softParams.y > 0.5)
    {
        softFade = clamp(
            (sceneViewDepth - vViewDepth) / max(frame.softParams.x, 0.001),
            0.0,
            1.0);
    }

    // 程序化软圆盘 + 漂移噪声：先用自包含 FBM 近似火焰/烟雾的边缘侵蚀，
    // 后续接入 flipbook 纹理时替换这一处 alpha 形状即可。
    float radius = length(vUV * 2.0 - 1.0);
    if (radius >= 1.0)
    {
        discard;
    }
    float radial = pow(
        1.0 - radius,
        mix(1.0, 3.0, clamp(vSoftness, 0.0, 1.0)));
    vec3 albedo = vColor.rgb;
    vec3 emission = vec3(0.0);
    float alpha = vColor.a * radial;
    if (frame.flipbookParams.y > 0.5)
    {
        vec4 sprite = mix(
            texture(particleTexture, vAtlasUV),
            texture(particleTexture, vAtlasUV2),
            vAtlasBlend);
        if (frame.textureModeParams.x > 0.5)
        {
            // R=Emissive, G=Occlusion, A=Opacity；B 通道不可用。
            // EOO 自发光不能被场景光照再次衰减；unlit 时只保留 R 通道，
            // 避免把恒定的作者颜色当成整张卡片的自发光底板。
            albedo = vColor.rgb * mix(1.0, sprite.g, 0.35);
            emission = vColor.rgb * sprite.r *
                frame.textureModeParams.y;
            if (frame.billboardLightParams.x < 0.5)
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
        vec2 noiseUv = vUV * 5.0 +
            vec2(vSeed, -vTime * 0.35);
        float erosion = smoothstep(
            0.18,
            0.82,
            Fbm(noiseUv + vec2(vTime * 0.2, 0.0)));
        alpha *= mix(1.0, erosion, 0.72);
    }
    if (frame.billboardLightParams.x > 0.5)
    {
        // 方向光共用同一套 CSM（以主方向光的级联为准）；点光/聚光/粒子灯
        // 当前没有局部阴影，shadow 只乘到方向光辐射度上。
        float directionalShadow = 1.0;
        if (frame.shadowParams.w > 0.5)
        {
            directionalShadow = SampleParticleCsmShadow(
                shadowMap,
                vWorldPosition,
                normalize(vLightingNormal));
        }
        albedo = CalculateParticleBillboardSceneLighting(
            localShadowMap,
            albedo,
            vWorldPosition,
            vLightingNormal,
            frame.cameraPosition.xyz,
            clamp(frame.billboardLightParams.y, 0.0, 1.0),
            clamp(frame.billboardLightParams.z, 0.0, 1.0),
            clamp(frame.billboardLightParams2.x, 0.0, 1.0),
            clamp(frame.billboardLightParams2.y, 0.02, 1.0),
            clamp(frame.billboardLightParams2.z, 0.0, 2.0),
            directionalShadow,
            vSize,
            clamp(frame.billboardLightParams2.w, 0.0, 0.25));
    }
    else if (frame.shadowParams.w > 0.5)
    {
        float shadow = SampleParticleCsmShadow(
            shadowMap,
            vWorldPosition,
            vLightingNormal);
        albedo *= mix(1.0, shadow, 0.5);
    }
    outColor = vec4(albedo + emission, alpha * softFade);
    outColor *= frame.colorTint;
    if (outColor.a > 0.0)
        atomicAdd(particleDebug.fragments, 1u);
}
