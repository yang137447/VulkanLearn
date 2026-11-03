#version 450

#include "particleCommon.glsl"
#include "particleSprite.glsl"

layout(location = 0) in vec4 vColor;
layout(location = 1) in vec2 vUV;
layout(location = 2) in float vSoftness;
layout(location = 3) in float vSeed;
layout(location = 4) in float vTime;
layout(location = 5) in vec2 vAtlasUV;
layout(location = 6) in float vViewDepth;

// rgb = 带符号的屏幕空间 UV 偏移（已乘覆盖率），a = 覆盖率。
// 该 pass 不写 sceneColor，toneMapping 采样本附件后偏移场景 UV。
layout(location = 0) out vec4 outDistortion;

// distortion pass 只把 sceneDepth 作为图输入；没有 shadowMap 绑定。
layout(set = 3, binding = 0) uniform sampler2D sceneDepth;

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
    // 被不透明几何遮挡的扰动粒子不产生偏移，避免把背景像素拖进前景。
    float deviceDepth = texture(
        sceneDepth,
        gl_FragCoord.xy / vec2(textureSize(sceneDepth, 0))).r;
    vec4 sceneView = frame.invProjection * vec4(0.0, 0.0, deviceDepth, 1.0);
    float sceneViewDepth = -sceneView.z / sceneView.w;
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

    float radius = length(vUV * 2.0 - 1.0);
    if (radius >= 1.0)
    {
        discard;
    }
    float radial = pow(
        1.0 - radius,
        mix(1.0, 3.0, clamp(vSoftness, 0.0, 1.0)));

    vec2 offsetDirection;
    float coverage;
    // y = distortion 纹理解码成功；加载失败时即使绑定了 1x1 白图也走
    // 程序化方向场，避免白图被解码成固定的 (1,1) 偏移。
    if (frame.flipbookParams.y > 0.5 &&
        frame.distortionParams.y > 0.5)
    {
        // 纹理 R/G 存切线空间噪声向量；线性采样下解码到 [-1,1]。
        // 覆盖率仍来自 alpha，避免把颜色贴图误当成偏移。
        vec4 sprite = texture(particleTexture, vAtlasUV);
        offsetDirection = sprite.rg * 2.0 - 1.0;
        coverage = vColor.a * sprite.a * radial;
    }
    else
    {
        // 无纹理时用 FBM 的有限差分近似局部梯度，得到会随寿命/时间
        // 流动的扰动方向；归一化前保留小幅度，避免中心出现零向量。
        vec2 noiseUv = vUV * 3.0 +
            vec2(vSeed * 7.0, -vTime * 0.45);
        const float epsilon = 0.01;
        float heightCenter = Fbm(noiseUv);
        vec2 gradient = vec2(
            Fbm(noiseUv + vec2(epsilon, 0.0)) - heightCenter,
            Fbm(noiseUv + vec2(0.0, epsilon)) - heightCenter);
        offsetDirection = gradient / max(epsilon, 1.0e-4);
        coverage = vColor.a * radial *
            smoothstep(0.12, 0.72, heightCenter);
    }

    // 偏移长度限幅，避免重叠粒子把 UV 拖出屏幕太远；strength 由作者控制，
    // 最终 rgb 还会被固定管线按 alpha 再乘一次覆盖率。
    float directionLength = length(offsetDirection);
    if (directionLength > 1.0e-5)
    {
        offsetDirection /= directionLength;
        offsetDirection *= min(directionLength, 1.0);
    }
    coverage = clamp(coverage * softFade, 0.0, 1.0);

    outDistortion = vec4(
        offsetDirection * frame.distortionParams.x,
        0.0,
        coverage);
}
