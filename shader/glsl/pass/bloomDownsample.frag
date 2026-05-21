#version 450

layout(location = 0) in vec2 inUV;
layout(location = 0) out vec4 outColor;

layout(set = 3, binding = 0) uniform sampler2D inputBloom;

vec3 SampleOffset(vec2 offsetUv)
{
    return texture(inputBloom, inUV + offsetUv).rgb;
}

void main()
{
    ivec2 sizePx = textureSize(inputBloom, 0);
    vec2 texel = 1.0 / vec2(sizePx);

    // 当前实现使用一个归一化的 3x3、近似高斯的采样核，让 bloom 在降采样时更稳定，
    // 避免直接单点采样带来的 aliasing、亮部丢失、跳变和闪烁。
    // 后续如果要进一步提升 HDR bloom 的质量，可在前 1~2 级 downsample 中引入
    // Karis average：它的重点不是单纯“更模糊”，而是抑制 fireflies，避免极亮孤立
    // 像素在低分辨率 mip 中占主导，从而让整条 bloom 金字塔更稳定。
    vec3 sum = vec3(0.0);
    sum += SampleOffset(vec2(0.0)) * 0.25;
    sum += (SampleOffset(vec2( texel.x, 0.0)) + SampleOffset(vec2(-texel.x, 0.0)) + SampleOffset(vec2(0.0,  texel.y)) + SampleOffset(vec2(0.0, -texel.y))) * 0.125;

    outColor = vec4(sum, 1.0);
}
