#version 450

// 磨砂玻璃背板：在 UI 面板矩形内，把已经合成好的画面做一次低频模糊。
// 不采样 live framebuffer：调用方先把 swapchain 内容 blit 到半分辨率离屏图。
layout(set = 0, binding = 0) uniform sampler2D backdrop;

layout(push_constant) uniform GlassParams
{
    vec4 rectA;  // xy = 左上角像素，zw = 尺寸像素
    vec4 rectB;
    vec4 params; // x = 半径A(dp)，y = 半径B(dp)，z = 视口宽，w = 视口高
    vec4 blur;   // x = 模糊半径（backdrop texel 数）
} glass;

layout(location = 0) out vec4 outColor;

float RoundedRectCoverage(vec2 p, vec4 rect, float radius)
{
    vec2 halfSize = rect.zw * 0.5;
    vec2 center = rect.xy + halfSize;
    vec2 d = abs(p - center) - max(halfSize - vec2(radius), vec2(0.0));
    float distanceToEdge =
        length(max(d, vec2(0.0))) + min(max(d.x, d.y), 0.0) - radius;
    return clamp(0.5 - distanceToEdge, 0.0, 1.0);
}

void main()
{
    vec2 frag = gl_FragCoord.xy;
    // RmlUi 的绝对布局坐标已经按 Vulkan viewport 使用底部原点；
    // 这里直接使用 gl_FragCoord，避免把矩形再翻转一次。
    vec2 pixel = frag;
    float coverage = max(
        RoundedRectCoverage(pixel, glass.rectA, glass.params.x),
        RoundedRectCoverage(pixel, glass.rectB, glass.params.y));
    if (coverage <= 0.001)
    {
        discard;
    }

    vec2 uv = frag / vec2(glass.params.z, glass.params.w);
    vec2 texel = 1.0 / vec2(textureSize(backdrop, 0));
    float radius = max(glass.blur.x, 0.0);
    vec3 color = texture(backdrop, uv).rgb * 0.24;
    color += texture(backdrop, uv + vec2(1.0, 0.0) * texel * radius).rgb * 0.12;
    color += texture(backdrop, uv + vec2(-1.0, 0.0) * texel * radius).rgb * 0.12;
    color += texture(backdrop, uv + vec2(0.0, 1.0) * texel * radius).rgb * 0.12;
    color += texture(backdrop, uv + vec2(0.0, -1.0) * texel * radius).rgb * 0.12;
    vec2 diagonal = normalize(vec2(1.0, 1.0)) * radius;
    color += texture(backdrop, uv + diagonal * texel).rgb * 0.07;
    color += texture(backdrop, uv - diagonal * texel).rgb * 0.07;
    color += texture(backdrop, uv + vec2(diagonal.x, -diagonal.y) * texel).rgb * 0.07;
    color += texture(backdrop, uv + vec2(-diagonal.x, diagonal.y) * texel).rgb * 0.07;

    outColor = vec4(color, coverage);
}
