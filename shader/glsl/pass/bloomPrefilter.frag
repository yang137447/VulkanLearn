#version 450

layout(location = 0) in vec2 inUV;
layout(location = 0) out vec4 outColor;

layout(set = 3, binding = 0) uniform sampler2D inputColor;

float Luminance(vec3 color)
{
    return dot(color, vec3(0.2126, 0.7152, 0.0722));
}

void main()
{
    ivec2 sizePx = textureSize(inputColor, 0);
    vec2 texel = 1.0 / vec2(sizePx);

    vec3 c0 = texture(inputColor, inUV + texel * vec2(-0.5, -0.5)).rgb;
    vec3 c1 = texture(inputColor, inUV + texel * vec2( 0.5, -0.5)).rgb;
    vec3 c2 = texture(inputColor, inUV + texel * vec2(-0.5,  0.5)).rgb;
    vec3 c3 = texture(inputColor, inUV + texel * vec2( 0.5,  0.5)).rgb;
    vec3 color = 0.25 * (c0 + c1 + c2 + c3);

    float threshold = 1.0;
    float knee = 0.5;

    float l = Luminance(color);
    float soft = clamp(l - threshold + knee, 0.0, 2.0 * knee);
    soft = (soft * soft) / (4.0 * knee + 1e-5);
    float bloom = max(l - threshold, 0.0) + soft;

    vec3 result = (l > 1e-5) ? color * (bloom / l) : vec3(0.0);
    outColor = vec4(result, 1.0);
}
