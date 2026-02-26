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

    vec3 sum = vec3(0.0);
    sum += SampleOffset(vec2(0.0)) * 0.25;
    sum += (SampleOffset(vec2( texel.x, 0.0)) + SampleOffset(vec2(-texel.x, 0.0)) + SampleOffset(vec2(0.0,  texel.y)) + SampleOffset(vec2(0.0, -texel.y))) * 0.125;
    sum += (SampleOffset(texel * vec2( 1.0,  1.0)) + SampleOffset(texel * vec2(-1.0,  1.0)) + SampleOffset(texel * vec2( 1.0, -1.0)) + SampleOffset(texel * vec2(-1.0, -1.0))) * 0.0625;

    outColor = vec4(sum, 1.0);
}
