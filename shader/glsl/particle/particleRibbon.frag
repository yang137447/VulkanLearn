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

layout(location = 0) out vec4 outColor;

// particle pass 只写 sceneColor；sceneDepth 是图输入，不能同时作为附件。
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
    // 与 billboard 相同的 sceneDepth/软粒子逻辑。
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

    // 条带横截面是矩形：只软化 vUV.y 两端，不做圆盘 radial 裁剪。
    float edgeWidth = mix(0.02, 0.5, clamp(vSoftness, 0.0, 1.0));
    float edge = smoothstep(0.0, edgeWidth, vUV.y) *
        smoothstep(0.0, edgeWidth, 1.0 - vUV.y);
    float alpha = edge;
    if (frame.flipbookParams.y > 0.5)
    {
        vec4 sprite = texture(particleTexture, vAtlasUV);
        outColor = vec4(
            vColor.rgb * sprite.rgb,
            vColor.a * sprite.a * alpha);
    }
    else
    {
        vec2 noiseUv = vec2(vUV.x * 5.0, vUV.y * 2.0) +
            vec2(vSeed, -vTime * 0.35);
        float erosion = smoothstep(
            0.18,
            0.82,
            Fbm(noiseUv + vec2(vTime * 0.2, 0.0)));
        outColor = vec4(
            vColor.rgb,
            vColor.a * alpha * mix(1.0, erosion, 0.6));
    }
    outColor.a *= softFade;
}
