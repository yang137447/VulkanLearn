#ifndef VL_DECAL_COMMON_GLSL
#define VL_DECAL_COMMON_GLSL

// Decal pass 的实例数据：decalToWorld 已包含作者尺寸的半尺寸缩放，
// worldToDecal 是它的逆矩阵。局部 cube 的 [-1,1] 范围就是投影体。
struct DecalInstance
{
    mat4 decalToWorld;
    mat4 worldToDecal;
    // xyz = baseColor，w = normalStrength（0 表示程序化法线不生效）。
    vec4 color;
    // x = roughness, y = metallic, z = ambientOcclusion, w = opacity.
    vec4 surface;
    // x = edge softness, y = noise strength, z = seed,
    // w = DecalResponse 位掩码（color/normal/roughness/metallic）。
    vec4 params;
    // xyz 保留；w = 1 表示 normalMap 已成功加载，shader 走贴图法线，
    // 否则走程序化 FBM 法线。
    vec4 normal;
};

// 把 C++ DecalResponse 的 uint32 位掩码（以 float 传入 SSBO）解码为
// (color, normal, roughness, metallic) 四个 0/1 通道。
vec4 DecalDecodeResponseMask(float packedMask)
{
    // [0,15] 在 float 中可精确表示；clamp 防止作者/上传数据越界。
    int mask = int(clamp(packedMask, 0.0, 15.0) + 0.5);
    return vec4(
        float(mask & 1),
        float((mask >> 1) & 1),
        float((mask >> 2) & 1),
        float((mask >> 3) & 1));
}

layout(std140, set = 0, binding = 0) uniform DecalFrame
{
    mat4 viewProjection;
    mat4 invViewProjection;
} decalFrame;

layout(std430, set = 0, binding = 1) readonly buffer DecalInstances
{
    DecalInstance instances[];
} decalInstances;

float DecalHash21(vec2 p)
{
    p = fract(p * vec2(123.34, 456.21));
    p += dot(p, p + 45.32);
    return fract(p.x * p.y);
}

float DecalValueNoise(vec2 p)
{
    vec2 cell = floor(p);
    vec2 local = fract(p);
    vec2 weight = local * local * (3.0 - 2.0 * local);
    float a = DecalHash21(cell);
    float b = DecalHash21(cell + vec2(1.0, 0.0));
    float c = DecalHash21(cell + vec2(0.0, 1.0));
    float d = DecalHash21(cell + vec2(1.0, 1.0));
    return mix(
        mix(a, b, weight.x),
        mix(c, d, weight.x),
        weight.y);
}

float DecalFbm(vec2 p)
{
    float value = 0.0;
    float amplitude = 0.55;
    for (int octave = 0; octave < 4; ++octave)
    {
        value += DecalValueNoise(p) * amplitude;
        p = p * 2.03 + vec2(17.1, 9.2);
        amplitude *= 0.5;
    }
    return value;
}

// 返回 world-space 像素位置；调用方负责把 sceneDepth 已转为 [0,1] UV。
vec3 DecalReconstructWorldPosition(
    sampler2D sceneDepth,
    vec2 screenUv)
{
    float deviceDepth = texture(sceneDepth, screenUv).r;
    vec4 clip = decalFrame.invViewProjection *
        vec4(screenUv * 2.0 - 1.0, deviceDepth, 1.0);
    return clip.xyz / clip.w;
}

// 局部 X 是投影深度，局部 Z/Y 是贴花平面；与 UE DeferredDecal 的
// SwizzlePos.zyx 语义保持一致。
vec2 DecalLocalToUV(vec3 localPosition)
{
    return localPosition.zy * 0.5 + 0.5;
}

float DecalProjectionFade(vec3 localPosition, float softness)
{
    float edge = max(
        abs(localPosition.x),
        max(abs(localPosition.y), abs(localPosition.z)));
    float soft = clamp(softness, 0.01, 0.999);
    return 1.0 - smoothstep(soft, 1.0, edge);
}

// normalStrength 为 0（或极小）时不允许覆盖接收表面法线；DBufferB 的
// coverage 保持 0，deferred 合成端会直接跳过法线响应。
// 与 source/render/decal/decalNormalBlend.h 的 DecalNormalWriteCoverage 同构。
float DecalNormalWriteCoverage(float coverage, float normalStrength)
{
    return coverage * step(1.0e-4, normalStrength);
}

#endif
