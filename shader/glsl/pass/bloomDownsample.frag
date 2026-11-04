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

    // Karis average：对 2x2 源 texel 做亮度加权平均，每个 tap 权重 1/(1+luma)。
    // 重点不是更模糊，而是抑制 fireflies：极亮孤立像素（火花/电弧）不会在低 mip
    // 中独占能量，从而消除 bloom 的跳变与爬行；权重归一化后整体亮度水平不变。
    const vec2 sampleOffsets[4] = vec2[](
        vec2(-0.5, -0.5),
        vec2( 0.5, -0.5),
        vec2(-0.5,  0.5),
        vec2( 0.5,  0.5));
    vec3 colorSum = vec3(0.0);
    float weightSum = 0.0;
    for (int index = 0; index < 4; ++index)
    {
        vec3 color = SampleOffset(sampleOffsets[index] * texel);
        float luma = dot(color, vec3(0.2126, 0.7152, 0.0722));
        float weight = 1.0 / (1.0 + luma);
        colorSum += color * weight;
        weightSum += weight;
    }

    outColor = vec4(colorSum / max(weightSum, 1.0e-5), 1.0);
}
