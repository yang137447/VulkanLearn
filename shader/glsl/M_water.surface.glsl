#ifndef VL_M_WATER_SURFACE_GLSL
#define VL_M_WATER_SURFACE_GLSL

#include "engine/materialInputs.glsl"
#include "common/commonUbo.glsl"

// 一列定向正弦波的高度场斜率：h = amplitude * sin(dot(p, dir) * frequency + phase)。
// 只返回 dh/dp，片元法线由总斜率重建，不需要求高度。
vec2 WaterWaveSlope(
    in vec2 p,
    in vec2 direction,
    in float frequency,
    in float amplitude,
    in float phase)
{
    const vec2 normalizedDirection = normalize(direction);
    return amplitude * frequency * normalizedDirection *
        cos(dot(p, normalizedDirection) * frequency + phase);
}

// M_water 是 singleWaterLayer pass 的专用水面材质。
//
// 本 pass 的深度附件同时作为 depthTest 输入，无法再采样 sceneDepth，因此不做折射/
// 厚度吸收。水体观感由三部分组成：
//   1) 很低但非零的 roughness，让环境 IBL 在掠射角接管成天空反射；
//   2) 基色保持深色吸收色，俯视时不会像一块亮蓝色塑料；
//   3) 世界空间解析波纹只扰动法线，不位移几何，并按相机距离分级淡出。
// 地形里的 River Water / Sea 层保留为水下底色，真正的水面由本材质覆盖。
MaterialInputs EvaluateMaterialInputs(in MaterialFunctionContext context)
{
    MaterialInputs inputs = CreateDefaultMaterialInputs();

    vec3 viewVector = uboVP.cameraPosition - context.worldPosition;
    float viewLength = length(viewVector);
    float viewDistanceM = viewLength * 0.01;
    vec3 viewDir = viewVector / max(viewLength, 1.0e-4);

    // 世界位置以 cm 存；波长按米定义，所以先换算到米域。
    vec2 p = context.worldPosition.xz * 0.01;

    // 远处一个像素覆盖多个波长，继续扰动只会闪烁；按波长分级淡出。
    float swellFade = 1.0 - smoothstep(40000.0, 120000.0, viewDistanceM);
    float chopFade = 1.0 - smoothstep(3000.0, 12000.0, viewDistanceM);
    float rippleFade = 1.0 - smoothstep(300.0, 1500.0, viewDistanceM);

    // 几百米尺度的斑块调制：只改碎浪强度，避免几组正弦互相干涉出规则条纹。
    float ripplePatch =
        0.55 + 0.45 * sin(p.x * 0.0047 + sin(p.y * 0.0031) * 1.7) *
        sin(p.y * 0.0039 + 0.9);

    // 波长 2*pi/frequency：两组长涌浪约 300/480 m、碎浪约 85/34 m、细波约 10/4 m。
    // 振幅按米给，斜率 = 振幅 * 频率。
    vec2 slope = vec2(0.0);
    slope += swellFade * WaterWaveSlope(p, vec2(0.86, 0.51), 0.0209, 0.55, 0.0);
    slope += swellFade * WaterWaveSlope(p, vec2(-0.62, 0.78), 0.0131, 0.80, 2.3);
    slope += chopFade * (0.5 + 0.5 * ripplePatch) *
        WaterWaveSlope(p, vec2(-0.30, 0.95), 0.0743, 0.16, 1.7);
    slope += chopFade * WaterWaveSlope(p, vec2(0.94, 0.34), 0.187, 0.055, 4.1);
    slope += rippleFade * ripplePatch *
        WaterWaveSlope(p, vec2(0.45, -0.89), 0.61, 0.012, 3.1);
    slope += rippleFade * WaterWaveSlope(p, vec2(-0.77, -0.64), 1.53, 0.0045, 5.4);
    slope *= u_waterRippleStrength;

    vec3 normal = normalize(vec3(-slope.x, 1.0, -slope.y));

    float ndv = clamp(dot(normal, viewDir), 0.0, 1.0);
    float fresnel = pow(1.0 - ndv, u_waterFresnelPower);

    // 吸收后的水体散射色始终很暗；掠射时的明亮感由 IBL 镜面反射提供，
    // 因此这里只让浅色以很低的权重参与，避免出现"亮蓝塑料"。
    inputs.baseColor =
        mix(u_waterDeepColor, u_waterShallowColor, fresnel * 0.35);
    inputs.roughness = mix(u_waterRoughness, 0.01, fresnel);
    inputs.metallic = 0.0;
    inputs.specular = u_waterSpecular;
    inputs.ambientOcclusion = 1.0;
    inputs.opacity = mix(u_waterOpacity, 1.0, fresnel);
    inputs.normal = normal;
    inputs.tangent = vec4(normalize(cross(normal, vec3(0.0, 0.0, 1.0))), 1.0);

    return inputs;
}

#endif
