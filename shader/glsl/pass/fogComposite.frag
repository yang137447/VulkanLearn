#version 450

#include "../common/commonUbo.glsl"

layout(location = 0) in vec2 inUV;
// rgb = in-scatter（预乘），a = 1 - transmittance。
// 管线使用 One / OneMinusSrcAlpha 混合：
//   dst' = fog.rgb + dst * (1 - fog.a) = S + sceneColor * T
layout(location = 0) out vec4 outColor;

layout(set = 0, binding = 1) uniform sampler2DArray fogVolume;
layout(set = 0, binding = 2) uniform sampler2D sceneDepth;

float FogSliceCoordinate(float viewDepthCm, float nearCm, float farCm,
    float layerCount)
{
    // 与 volumetricFog.comp / volumetricFogSettings.h 的指数分布互逆。
    float safeNear = max(nearCm, 1.0);
    float safeFar = max(farCm, safeNear + 1.0);
    float clampedDepth = clamp(viewDepthCm, safeNear, safeFar);
    return
        log(clampedDepth / safeNear) /
        max(log(safeFar / safeNear), 1.0e-6) *
        max(layerCount - 1.0, 1.0);
}

void main()
{
    vec2 ndc = inUV * 2.0 - 1.0;
    float deviceDepth = texture(sceneDepth, inUV).r;
    vec4 sceneView = uboVP.invProjection * vec4(ndc, deviceDepth, 1.0);
    float sceneViewDepth = -sceneView.z / max(sceneView.w, 1.0e-6);

    float nearCm = max(uboVP.fogParams2.x, 1.0);
    float farCm = max(uboVP.fogParams2.y, nearCm + 1.0);
    float layerCount = clamp(uboVP.fogParams2.z, 2.0, 32.0);

    float coordinate = FogSliceCoordinate(
        sceneViewDepth, nearCm, farCm, layerCount);
    float baseLayer = floor(coordinate);
    float fraction = coordinate - baseLayer;
    int layer0 = clamp(int(baseLayer), 0, int(layerCount) - 1);
    int layer1 = min(layer0 + 1, int(layerCount) - 1);

    vec4 fog0 = texture(fogVolume, vec3(inUV, float(layer0)));
    vec4 fog1 = texture(fogVolume, vec3(inUV, float(layer1)));
    // 相邻层线性插值：两者都存的是“从相机累计到该层”的结果，
    // 因而插值近似等于在像素真实深度处截断积分。
    vec4 fog = mix(fog0, fog1, clamp(fraction, 0.0, 1.0));
    outColor = vec4(max(fog.rgb, vec3(0.0)), clamp(fog.a, 0.0, 1.0));
}
