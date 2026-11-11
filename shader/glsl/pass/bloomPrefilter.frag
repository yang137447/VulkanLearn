#version 450

layout(location = 0) in vec2 inUV;
layout(location = 0) out vec4 outColor;

layout(set = 1, binding = 0) uniform UBOMIParamters {
    // xyz 是曝光后空间的 threshold/knee/firefliesClamp，w 由活动相机写入。
    vec4 u_bloomPrefilterParams;
    // x = 1 时从自动曝光 1x1 资源读取最终曝光。
    vec4 u_exposureSettings;
};

layout(set = 3, binding = 0) uniform sampler2D inputColor;
layout(set = 3, binding = 1) uniform sampler2D autoExposure;
// 屏幕空间扰动：rg 是屏幕高度归一化的带符号偏移（1.0 = 一个屏幕高度）。
layout(set = 3, binding = 2) uniform sampler2D distortion;

float Luminance(vec3 color)
{
    return dot(color, vec3(0.2126, 0.7152, 0.0722));
}

vec2 DistortionSceneUv(vec2 uv, vec2 distortionOffset, vec2 sceneSize)
{
    // 把高度归一化偏移转成 UV：y 已是高度比例，x 还需除以宽高比，
    // 这样横纵偏移对应的像素距离相同，且在任意视口比例下语义一致。
    float aspect = sceneSize.x / max(sceneSize.y, 1.0);
    return clamp(
        uv + vec2(distortionOffset.x / aspect, distortionOffset.y),
        vec2(0.0),
        vec2(1.0));
}

void main()
{
    ivec2 sizePx = textureSize(inputColor, 0);
    vec2 texel = 1.0 / vec2(sizePx);
    // Bloom 必须从扭曲后的场景生成；这里与 toneMapping 使用同一映射公式，
    // 但 toneMapping 只偏移最终 sceneColor，不会再次扭曲 Bloom。
    vec2 sceneUV = DistortionSceneUv(
        inUV,
        texture(distortion, inUV).rg,
        vec2(sizePx));

    vec3 c0 = texture(inputColor, sceneUV + texel * vec2(-0.5, -0.5)).rgb;
    vec3 c1 = texture(inputColor, sceneUV + texel * vec2( 0.5, -0.5)).rgb;
    vec3 c2 = texture(inputColor, sceneUV + texel * vec2(-0.5,  0.5)).rgb;
    vec3 c3 = texture(inputColor, sceneUV + texel * vec2( 0.5,  0.5)).rgb;
    vec3 color = 0.25 * (c0 + c1 + c2 + c3);

    float threshold = u_bloomPrefilterParams.x;
    float knee = u_bloomPrefilterParams.y;
    float firefliesClamp = u_bloomPrefilterParams.z;
    float exposure = u_bloomPrefilterParams.w;
    if (u_exposureSettings.x > 0.5)
    {
        exposure = texture(autoExposure, vec2(0.5)).r;
    }

    // 物理亮度先按相机曝光再提取亮部，避免旧的 12 上限截掉高 EV 下的全部光晕。
    color *= exposure;

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
    // Bloom 金字塔保存曝光后的线性值；最终合成时只对 sceneColor 做曝光，
    // 避免 Bloom 再次乘曝光，也让 threshold/clamp 与最终显示亮度处于同一空间。
    outColor = vec4(result, 1.0);
}
