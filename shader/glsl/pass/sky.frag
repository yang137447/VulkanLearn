#version 450

#include "../common/commonUbo.glsl"

layout(location = 0) in vec2 inUV;
layout(location = 0) out vec4 outColor;

layout(std140, set = 0, binding = 4) uniform SkyParametersGPU{
    vec4 sunDirectionIntensity;
    vec4 sunColorAngularRadius;
    vec4 zenithColor;
    vec4 horizonColor;
    vec4 groundColor;
    vec4 scatteringControls;
    vec4 cloudControls;
} skyParameters;

vec3 GetViewRayWS(vec2 uv)
{
    vec2 ndc = uv * 2.0f - 1.0f;
    vec4 clipPos = vec4(ndc, 1.0f, 1.0f);
    vec4 viewPos = uboVP.invProjection * clipPos;
    vec3 viewDir = normalize(viewPos.xyz / viewPos.w);
    return normalize((uboVP.invView * vec4(viewDir, 0.0f)).xyz);
}

vec3 EvaluateProceduralSky(vec3 worldDir)
{
    float skyFactor = clamp(worldDir.y, 0.0f, 1.0f);
    float skyGradient = pow(skyFactor, skyParameters.scatteringControls.x);
    vec3 skyColor = mix(
        skyParameters.horizonColor.rgb,
        skyParameters.zenithColor.rgb,
        skyGradient);

    float groundFactor = clamp(-worldDir.y, 0.0f, 1.0f);
    vec3 lowerHemisphereColor = mix(
        skyParameters.horizonColor.rgb,
        skyParameters.groundColor.rgb,
        pow(groundFactor, skyParameters.scatteringControls.y));
    skyColor = mix(skyColor, lowerHemisphereColor, step(worldDir.y, 0.0f));

    // CPU convention: sunDirectionIntensity.xyz points from the scene toward
    // the visible sun disc. Directional lights store the opposite ray direction.
    vec3 sunDirection = normalize(skyParameters.sunDirectionIntensity.xyz);
    float sunCos = dot(normalize(worldDir), sunDirection);
    float sunAngularRadius = skyParameters.sunColorAngularRadius.w;
    float sunDisc = smoothstep(cos(sunAngularRadius), cos(sunAngularRadius * 0.35f), sunCos);
    float haloExponent = sunAngularRadius > 0.001
        ? skyParameters.scatteringControls.z / sunAngularRadius
        : skyParameters.scatteringControls.z;
    float sunHalo =
        pow(max(sunCos, 0.0f), haloExponent) *
        skyParameters.scatteringControls.w;
    vec3 sunColor =
        skyParameters.sunColorAngularRadius.rgb *
        skyParameters.sunDirectionIntensity.w;

    return skyColor + sunColor * (sunDisc + sunHalo);
}

void main()
{
    vec3 worldDir = GetViewRayWS(inUV);
    vec3 skyColor = EvaluateProceduralSky(worldDir) * uboVP.environmentIntensity;
    outColor = vec4(skyColor, 1.0f);
}
