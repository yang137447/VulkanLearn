#ifndef PARTICLE_SHADOW_CASTER_COMMON_GLSL
#define PARTICLE_SHADOW_CASTER_COMMON_GLSL

#include "particleCommon.glsl"

// billboard/ribbon 是透明卡片，没有几何厚度。阴影端用固定阈值的
// alpha test 近似 UE 的 masked shadow。烟/尘卡片是半透明的，阈值太低
// 会把整张卡写成实心剪影；取 0.55 只保留较密的芯部，避免大块黑板。
// 真正的半透明软阴影需要逐灯 transmittance 累积，当前二进制 shadow map
// 只能做到 masked 近似。阈值只作用于形状覆盖率，不改变可见粒子合成。
const float kParticleShadowAlphaThreshold = 0.35;

// 与 particle.frag / particleRibbon.frag 相同的程序化边缘侵蚀。
// 阴影 pass 不声明可见 pass 的 sceneDepth/光照输入，因此这里保留一份
// 只读噪声；可见着色代码未被改动。
float ParticleShadowHash21(vec2 p)
{
    p = fract(p * vec2(123.34, 456.21));
    p += dot(p, p + 45.32);
    return fract(p.x * p.y);
}

float ParticleShadowValueNoise(vec2 p)
{
    vec2 i = floor(p);
    vec2 f = fract(p);
    vec2 u = f * f * (3.0 - 2.0 * f);
    float a = ParticleShadowHash21(i);
    float b = ParticleShadowHash21(i + vec2(1.0, 0.0));
    float c = ParticleShadowHash21(i + vec2(0.0, 1.0));
    float d = ParticleShadowHash21(i + vec2(1.0, 1.0));
    return mix(mix(a, b, u.x), mix(c, d, u.x), u.y);
}

float ParticleShadowFbm(vec2 p)
{
    float value = 0.0;
    float amplitude = 0.55;
    for (int octave = 0; octave < 4; ++octave)
    {
        value += ParticleShadowValueNoise(p) * amplitude;
        p = p * 2.03 + vec2(17.1, 9.2);
        amplitude *= 0.5;
    }
    return value;
}

// 与可见 billboard 相同的软圆盘；半径 >= 1 时完全没有覆盖。
float ParticleShadowRadialAlpha(vec2 uv, float softness)
{
    float radius = length(uv * 2.0 - 1.0);
    if (radius >= 1.0)
    {
        return 0.0;
    }
    return pow(
        1.0 - radius,
        mix(1.0, 3.0, clamp(softness, 0.0, 1.0)));
}

// 可见 billboard 无纹理分支：软圆盘再乘 0.72 权重的 FBM 侵蚀。
float ParticleShadowProceduralAlpha(
    vec2 uv,
    float softness,
    float seed,
    float time)
{
    float radial = ParticleShadowRadialAlpha(uv, softness);
    if (radial <= 0.0)
    {
        return 0.0;
    }
    vec2 noiseUv = uv * 5.0 + vec2(seed, -time * 0.35);
    float erosion = smoothstep(
        0.18,
        0.82,
        ParticleShadowFbm(noiseUv + vec2(time * 0.2, 0.0)));
    return radial * mix(1.0, erosion, 0.72);
}

// 与 mesh caster 完全一致的深度约定：
// x = 0 透视（CSM / 聚光），直接写硬件插值深度；
// x = 1 点光八面体，按象限丢弃并写方向长度归一化深度。
// 返回 false 表示该片元不属于当前象限，调用方必须 discard。
bool ParticleShadowCasterDepth(
    vec3 worldPosition,
    out float depth)
{
    depth = gl_FragCoord.z;
    if (frame.shadowCasterParams.x <= 0.5)
    {
        return true;
    }

    int octant = int(frame.shadowCasterParams.y + 0.5);
    vec3 lightToPosition =
        worldPosition - frame.shadowLightPosition.xyz;
    float sx = (octant & 1) != 0 ? 1.0 : -1.0;
    float sy = (octant & 2) != 0 ? 1.0 : -1.0;
    float sz = (octant & 4) != 0 ? 1.0 : -1.0;
    if (lightToPosition.x * sx < 0.0 ||
        lightToPosition.y * sy < 0.0 ||
        lightToPosition.z * sz < 0.0)
    {
        return false;
    }
    depth = clamp(
        length(lightToPosition) /
            max(frame.shadowCasterParams.z, 1.0e-4),
        0.0,
        1.0);
    return true;
}

#endif
