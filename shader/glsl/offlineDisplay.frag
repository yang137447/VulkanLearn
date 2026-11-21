#version 450

layout(set = 0, binding = 0) uniform sampler2D film;
layout(push_constant) uniform DisplaySettings
{
    float exposure;
    int encodeSrgb;
} settings;
layout(location = 0) in vec2 uv;
layout(location = 0) out vec4 outputColor;

void main()
{
    vec3 linear = max(texture(film, uv).rgb * settings.exposure, vec3(0.0));
    vec3 mapped = clamp(
        (linear * (2.51 * linear + 0.03)) /
        (linear * (2.43 * linear + 0.59) + 0.14), 0.0, 1.0);
    if (settings.encodeSrgb != 0)
        mapped = mix(12.92 * mapped,
                     1.055 * pow(mapped, vec3(1.0 / 2.4)) - 0.055,
                     greaterThan(mapped, vec3(0.0031308)));
    outputColor = vec4(mapped, 1.0);
}
