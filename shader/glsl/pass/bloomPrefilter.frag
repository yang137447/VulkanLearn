#version 450

layout(location = 0) in vec2 inUV;
layout(location = 0) out vec4 outColor;

layout(set = 1, binding = 0) uniform UBOMIParamters {
    // x = threshold, y = knee, z = firefliesClamp, w = reserved
    vec4 u_bloomPrefilterParams;
};

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

    float threshold = u_bloomPrefilterParams.x;
    float knee = u_bloomPrefilterParams.y;
    float firefliesClamp = u_bloomPrefilterParams.z;

    float l = Luminance(color);
    if (l > firefliesClamp)
    {
        // 这里先做一次亮度截断，尽早抑制极亮孤立像素，避免它们在后续的降采样 /
        // 上采样链路里被不断扩散。它和 Karis average 的目标相近，但职责不同：
        // 这里是 prefilter 阶段的硬限制，Karis average 则是 downsample 阶段更稳定的 HDR 聚合。
        color *= firefliesClamp / max(l, 1e-5);
        l = firefliesClamp;
    }

    // 在 threshold 附近加入 soft knee，让亮部进入 bloom 的过程更平滑，
    // 避免硬阈值带来的突变边缘，也能减轻部分噪点导致的异常 bloom。
    float soft = clamp(l - threshold + knee, 0.0, 2.0 * knee);
    soft = (soft * soft) / (4.0 * knee + 1e-5);
    float bloom = max(l - threshold, 0.0) + soft;

    vec3 result = (l > 1e-5) ? color * (bloom / l) : vec3(0.0);
    outColor = vec4(result, 1.0);
}
