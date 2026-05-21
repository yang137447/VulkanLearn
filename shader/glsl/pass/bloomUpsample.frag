#version 450

layout(location = 0) in vec2 inUV;
layout(location = 0) out vec4 outColor;

layout(set = 3, binding = 0) uniform sampler2D inputBloomCurrent;
layout(set = 3, binding = 1) uniform sampler2D inputBloomLow;

vec3 SampleOffset(vec2 offsetUv)
{
    return texture(inputBloomLow, inUV + offsetUv).rgb;
}

void main()
{
    vec3 current = texture(inputBloomCurrent, inUV).rgb;

    ivec2 sizePx = textureSize(inputBloomLow, 0);
    vec2 texel = 1.0 / vec2(sizePx);

    // 对更低分辨率的 bloom 做一个小型高斯式平滑，再逐级回卷到当前层。
    // 这里的目标不是单纯“放大图片”，而是把低频、大范围的 glow 平滑带回
    // 更高分辨率层，最后与当前层已有的 bloom 细节叠加，形成更自然的泛光范围。
    vec3 sum = vec3(0.0);
    sum += SampleOffset(vec2(0.0)) * 0.25;
    sum += (SampleOffset(vec2( texel.x, 0.0)) + SampleOffset(vec2(-texel.x, 0.0)) + SampleOffset(vec2(0.0,  texel.y)) + SampleOffset(vec2(0.0, -texel.y))) * 0.125;

    outColor = vec4(current + sum, 1.0);
}
