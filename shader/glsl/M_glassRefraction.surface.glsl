#ifndef VL_M_GLASS_REFRACTION_SURFACE_GLSL
#define VL_M_GLASS_REFRACTION_SURFACE_GLSL

#include "common/commonUbo.glsl"
#include "engine/materialInputs.glsl"

// ForwardTransparent cannot sample its own sceneColor attachment. The graph
// copies sceneColor into sceneColorCopy before this pass; this surface reads
// only that copy and the opaque G-buffer normal for optional perturbation.
layout(set = 3, binding = 8) uniform sampler2D sceneColorCopy;
layout(set = 3, binding = 9) uniform sampler2D gbufferA;

vec2 ProjectWorldToScreen(vec3 worldPosition)
{
    vec4 clip = uboVP.viewProjection * vec4(worldPosition, 1.0);
    float safeW = abs(clip.w) > 1e-5 ? clip.w : 1e-5;
    return clip.xy / safeW * 0.5 + 0.5;
}

vec2 ClampScreenUv(vec2 uv, vec2 screenSize)
{
    vec2 halfTexel = 0.5 / screenSize;
    return clamp(uv, halfTexel, vec2(1.0) - halfTexel);
}

vec3 SampleBackgroundNormal(vec2 uv, vec3 fallbackNormal)
{
    vec4 encoded = texture(gbufferA, uv);
    // Sky pixels keep the GBuffer clear value instead of a valid normal.
    vec3 clearDelta = abs(encoded.rgb - vec3(0.2));
    if (max(max(clearDelta.r, clearDelta.g), clearDelta.b) <= 0.01)
    {
        return fallbackNormal;
    }
    return normalize(encoded.rgb * 2.0 - 1.0);
}

vec3 SampleRefractedScene(vec2 uv, vec2 uvOffset, vec2 screenSize)
{
    vec2 centerUv = ClampScreenUv(uv + uvOffset, screenSize);
    if (u_roughness <= 0.0001)
    {
        return texture(sceneColorCopy, centerUv).rgb;
    }

    // Four extra taps are a cheap screen-space softness approximation. They
    // never widen beyond a few texels, so silhouettes stay readable.
    vec2 texel = 1.0 / screenSize;
    float radius = u_roughness * 6.0;
    vec3 color = texture(sceneColorCopy, centerUv).rgb;
    color += texture(sceneColorCopy, ClampScreenUv(centerUv + vec2(radius, 0.0) * texel, screenSize)).rgb;
    color += texture(sceneColorCopy, ClampScreenUv(centerUv - vec2(radius, 0.0) * texel, screenSize)).rgb;
    color += texture(sceneColorCopy, ClampScreenUv(centerUv + vec2(0.0, radius) * texel, screenSize)).rgb;
    color += texture(sceneColorCopy, ClampScreenUv(centerUv - vec2(0.0, radius) * texel, screenSize)).rgb;
    return color * 0.2;
}

MaterialInputs EvaluateMaterialInputs(in MaterialFunctionContext context)
{
    MaterialInputs inputs = CreateDefaultMaterialInputs();

    vec2 screenSize = vec2(textureSize(sceneColorCopy, 0));
    vec2 screenUv = gl_FragCoord.xy / screenSize;
    vec3 worldNormal = normalize(context.worldNormal);
    vec3 viewDirection = normalize(uboVP.cameraPosition - context.worldPosition);
    float nDotV = clamp(dot(worldNormal, viewDirection), 0.0, 1.0);

    // Snell refraction from camera to surface. The refracted ray exits the
    // approximate glass slab and is projected back to screen space. Total
    // internal reflection falls back to the normal direction so the sample
    // never becomes NaN at grazing angles.
    vec3 incident = -viewDirection;
    float eta = 1.0 / max(u_ior, 1.0);
    vec3 refracted = refract(incident, worldNormal, eta);
    if (dot(refracted, refracted) < 0.25)
    {
        refracted = worldNormal;
    }
    vec3 exitPosition =
        context.worldPosition +
        refracted * u_thickness * clamp(nDotV, 0.15, 1.0);
    vec2 rayOffset =
        (ProjectWorldToScreen(exitPosition) - screenUv) * u_refractionStrength;

    // Optional perturbation from the opaque surface behind the glass. The
    // branch rejects the neutral GBuffer clear value used by sky pixels.
    vec2 backgroundUv = ClampScreenUv(screenUv + rayOffset, screenSize);
    vec3 backgroundNormal = SampleBackgroundNormal(backgroundUv, worldNormal);
    vec3 backgroundNormalView = mat3(uboVP.view) * backgroundNormal;
    vec2 backgroundOffset =
        vec2(backgroundNormalView.x, -backgroundNormalView.y) *
        (u_backgroundNormalStrength * 0.01);

    vec3 refractedColor = SampleRefractedScene(
        screenUv,
        rayOffset + backgroundOffset,
        screenSize);

    float f0 = pow((u_ior - 1.0) / (u_ior + 1.0), 2.0);
    float fresnel = f0 + (1.0 - f0) * pow(1.0 - nDotV, 5.0);
    vec3 glassColor =
        refractedColor * u_glassTint.rgb +
        vec3(0.08, 0.11, 0.12) * fresnel;
    float coverage = clamp(
        u_opacity + fresnel * 0.1,
        0.0,
        1.0);

    inputs.baseColor = glassColor;
    inputs.emissiveColor = glassColor;
    inputs.opacity = coverage;
    inputs.opacityMask = coverage;
    inputs.normal = worldNormal;
    inputs.tangent = context.worldTangent;
    inputs.roughness = clamp(u_roughness, 0.0, 1.0);
    inputs.metallic = 0.0;
    inputs.specular = 0.5;
    return inputs;
}

#endif
