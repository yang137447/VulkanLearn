#version 450
#include "../common/function.glsl"

layout(location = 0) in vec2 inUV;
layout(location = 0) out vec4 outColor;

layout(set = 1, binding = 0) uniform UBOMIParamters {
    // x = exposure, y = bloomStrength, z = saturation, w = toneMappingMode
    vec4 u_toneMappingParams;
};

layout(set = 3, binding = 0) uniform sampler2D sceneColor;
layout(set = 3, binding = 1) uniform sampler2D bloomColor;

// Tone Mapping Functions

vec3 LinearClamp(vec3 color) {
    return clamp(color, 0.0, 1.0);
}

// 工程版，分通道：更简单、更便宜，但高亮彩色区域更容易发生 色相偏移 或 去饱和方式不自然 。
// vec3 ToneMap_Reinhard(vec3 color) {
//     return color / (color + vec3(1.0));
// }
// 先压亮度，再保持原色相比例
vec3 ToneMap_Reinhard(vec3 color) {
    float luma = Luminance(color);
    float mappedLuma = luma / (luma + 1.0);
    return color * (mappedLuma / max(mappedLuma, 1e-5));
}

// Hable / Filmic
vec3 HablePartial(vec3 x) {
    float A = 0.15;
    float B = 0.50;
    float C = 0.10;
    float D = 0.20;
    float E = 0.02;
    float F = 0.30;
    return ((x*(A*x+C*B)+D*E)/(x*(A*x+B)+D*F))-E/F;
}

vec3 ToneMap_Hable(vec3 color) {
    vec3 white = vec3(11.2);
    vec3 curr = HablePartial(color);
    vec3 w = HablePartial(white);
    return clamp(curr / w, 0.0, 1.0);
}

// ACES fitted
vec3 ToneMap_ACES(vec3 color) {
    float a = 2.51;
    float b = 0.03;
    float c = 2.43;
    float d = 0.59;
    float e = 0.14;
    return clamp((color * (a * color + b)) / (color * (c * color + d) + e), 0.0, 1.0);
}

vec3 ApplyToneMap(vec3 color, int mode) {
    switch(mode) {
        case 0: return LinearClamp(color);
        case 1: return ToneMap_Reinhard(color);
        case 2: return ToneMap_Hable(color);
        case 3: return ToneMap_ACES(color);
        default: return ToneMap_ACES(color);
    }
}

vec3 ApplySaturation(vec3 color, float saturation) {
    float luma = dot(color, vec3(0.2126, 0.7152, 0.0722));
    return mix(vec3(luma), color, saturation);
}

void main()
{
    vec3 scene = texture(sceneColor, inUV).rgb;
    vec3 bloom = texture(bloomColor, inUV).rgb;

    float exposure = u_toneMappingParams.x;
    float bloomStrength = u_toneMappingParams.y;
    float saturation = u_toneMappingParams.z;
    int mode = int(u_toneMappingParams.w);

    vec3 hdr = scene + bloom * bloomStrength;
    vec3 exposed = hdr * exposure;
    
    vec3 color = ApplyToneMap(exposed, mode);
    color = ApplySaturation(color, saturation);
    
    outColor = vec4(color, 1.0);
}
