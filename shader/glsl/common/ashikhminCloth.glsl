#ifndef VL_COMMON_ASHIKHMIN_CLOTH_GLSL
#define VL_COMMON_ASHIKHMIN_CLOTH_GLSL

#include "defines.glsl"
#include "clothAshikhminConstants.glsl"

// Ashikhmin, Premoze, Shirley 2000 runtime closure.
//
// The paper keeps p(h) analytic and replaces the per-pixel hemisphere
// integrals g(k) and R(k) with coarse tables.  VL bakes the tables offline for
// the three paper parameter sets; one material variant compiles exactly one
// VL_CLOTH_CLOSURE_* table set.
//
// Local frame convention follows the offline evaluator:
//   x = transverse direction (fiber rotation applied around n)
//   y = cross(n, x) (fiber direction)
//   z = n
// Satin's cylindrical Gaussian depends on h.x / h.z, not h.y / h.z.

struct AshikhminClothFrame
{
    vec3 x;
    vec3 y;
    vec3 n;
};

AshikhminClothFrame BuildAshikhminClothFrame(
    in vec3 normal,
    in vec4 tangent,
    in float rotationRadians)
{
    AshikhminClothFrame frame;
    frame.n = normalize(normal);
    vec3 base = tangent.xyz;
    vec3 projected = base - frame.n * dot(frame.n, base);
    float projectedLength = length(projected);
    if (projectedLength < 1.0e-5)
    {
        projected = abs(frame.n.z) < 0.999
            ? normalize(cross(frame.n, vec3(0.0, 0.0, 1.0)))
            : vec3(1.0, 0.0, 0.0);
    }
    else
    {
        projected /= projectedLength;
    }
    vec3 helper = cross(frame.n, projected);
    frame.x = normalize(
        projected * cos(rotationRadians) +
        helper * sin(rotationRadians));
    frame.y = normalize(cross(frame.n, frame.x));
    return frame;
}

float AshikhminClothDensity(in vec3 hLocal)
{
#if defined(VL_CLOTH_CLOSURE_SATIN)
    if (hLocal.z <= 0.0)
    {
        return 0.0;
    }
    float slope = hLocal.x / hLocal.z;
    float flatTerm = VL_CLOTH_NORM_FLAT * exp(
        -(slope * slope) /
        (VL_CLOTH_SIGMA_FLAT * VL_CLOTH_SIGMA_FLAT));
    float endTerm = VL_CLOTH_NORM_ENDS * exp(
        -(slope * slope) /
        (VL_CLOTH_SIGMA_ENDS * VL_CLOTH_SIGMA_ENDS));
    return
        VL_CLOTH_FLAT_WEIGHT * flatTerm +
        (1.0 - VL_CLOTH_FLAT_WEIGHT) * endTerm;
#elif defined(VL_CLOTH_CLOSURE_VELVET)
    float noH = hLocal.z;
    if (noH <= 0.0 || noH >= 1.0)
    {
        return 0.0;
    }
    float sin2 = 1.0 - noH * noH;
    return VL_CLOTH_DENSITY_NORMALIZATION * exp(
        -(noH * noH) /
        (VL_CLOTH_SIGMA * VL_CLOTH_SIGMA * sin2));
#elif defined(VL_CLOTH_CLOSURE_SLANTED_VELVET)
    float cosine = cos(VL_CLOTH_SLANT_ANGLE);
    float sine = sin(VL_CLOTH_SLANT_ANGLE);
    vec3 local = vec3(
        cosine * hLocal.x - sine * hLocal.z,
        hLocal.y,
        sine * hLocal.x + cosine * hLocal.z);
    if (local.z <= 0.0)
    {
        return 0.0;
    }
    float slope = local.x / local.z;
    return VL_CLOTH_DENSITY_NORMALIZATION * exp(
        -(slope * slope) /
        (VL_CLOTH_SIGMA * VL_CLOTH_SIGMA));
#else
    return 0.0;
#endif
}

vec4 AshikhminClothConstants()
{
#if defined(VL_CLOTH_CLOSURE_VELVET)
    return texelFetch(clothLut, ivec2(192, 0), 0);
#elif defined(VL_CLOTH_CLOSURE_SATIN)
    return texelFetch(clothLut, ivec2(193, 0), 0);
#else
    return texelFetch(clothLut, ivec2(194, 0), 0);
#endif
}

vec2 AshikhminClothLutUv(in vec3 localDirection)
{
    float mu = clamp(localDirection.z, 0.0, 1.0) * 63.0;
#if defined(VL_CLOTH_CLOSURE_VELVET)
    return vec2((mu + 0.5) / VL_CLOTH_LUT_SIZE, 0.5 / VL_CLOTH_LUT_SIZE);
#elif defined(VL_CLOTH_CLOSURE_SATIN)
    float phi = clamp(
        atan(abs(localDirection.y), abs(localDirection.x)) / (PI * 0.5),
        0.0,
        1.0) * 63.0;
    return vec2(
        (VL_CLOTH_LUT_TILE + phi + 0.5) / VL_CLOTH_LUT_SIZE,
        (mu + 0.5) / VL_CLOTH_LUT_SIZE);
#else
    float phi = clamp(
        atan(abs(localDirection.y), localDirection.x) / PI,
        0.0,
        1.0) * 63.0;
    return vec2(
        (phi + 0.5) / VL_CLOTH_LUT_SIZE,
        (VL_CLOTH_LUT_TILE + mu + 0.5) / VL_CLOTH_LUT_SIZE);
#endif
}

float AshikhminClothLookupG(in vec3 localDirection)
{
    return texture(clothLut, AshikhminClothLutUv(localDirection)).r;
}

float AshikhminClothLookupR(in vec3 localDirection, in float f0)
{
    vec4 lutSample = texture(clothLut, AshikhminClothLutUv(localDirection));
    return lutSample.g + f0 * (lutSample.b - lutSample.g);
}

float AshikhminClothCorrelation(in vec3 localV, in vec3 localL)
{
    vec2 viewTangent = localV.xy;
    vec2 lightTangent = localL.xy;
    float viewLength = length(viewTangent);
    float lightLength = length(lightTangent);
    if (viewLength <= 1.0e-10 || lightLength <= 1.0e-10)
    {
        // The paper does not define a correlation value for a degenerate
        // projection; VL uses the uncorrelated limit.
        return 0.0;
    }
    float cosine = clamp(
        dot(viewTangent, lightTangent) / (viewLength * lightLength),
        -1.0,
        1.0);
    float angle = acos(cosine);
    float width = 15.0 * PI / 180.0;
    return exp(-0.5 * angle * angle / (width * width));
}

float AshikhminClothCorrelatedFactor(
    in vec3 localV,
    in vec3 localL,
    in float pV,
    in float pL)
{
    float t = AshikhminClothCorrelation(localV, localL);
    return (1.0 - t) + t / max(max(pV, pL), 1.0e-4);
}

vec3 EvaluateAshikhminClothLight(
    in vec3 normal,
    in vec3 view,
    in vec3 lightDirection,
    in vec3 radiance,
    in vec3 diffuseColor,
    in vec3 specularTint,
    in float f0,
    in AshikhminClothFrame frame,
    out vec3 outDiffuse,
    out vec3 outSpecular)
{
    outDiffuse = vec3(0.0);
    outSpecular = vec3(0.0);

    float nDotL = dot(normal, lightDirection);
    float nDotV = dot(normal, view);
    if (nDotL <= 0.0 || nDotV <= 0.0)
    {
        return vec3(0.0);
    }

    vec3 halfVector = normalize(view + lightDirection);
    vec3 localH = vec3(
        dot(halfVector, frame.x),
        dot(halfVector, frame.y),
        dot(halfVector, frame.n));
    float density = AshikhminClothDensity(localH);
    // p(h)=0 only removes the specular lobe. The diffuse term is the
    // remaining directional energy and stays alive, matching
    // satinBrdf/velvetBrdf/slantedVelvetBrdf in vl_offline_core. Returning here
    // used to kill diffuse as well, which painted black stripes on every fold
    // whose half vector fell outside the slanted-velvet lobe.

    vec3 localV = vec3(
        dot(view, frame.x),
        dot(view, frame.y),
        dot(view, frame.n));
    vec3 localL = vec3(
        dot(lightDirection, frame.x),
        dot(lightDirection, frame.y),
        dot(lightDirection, frame.n));
    vec4 constants = AshikhminClothConstants();
    float eta = constants.x;
    float gV = max(AshikhminClothLookupG(localV), 1.0e-6);
    float gL = max(AshikhminClothLookupG(localL), 1.0e-6);
    float pV = nDotV * eta / gV;
    float pL = nDotL * eta / gL;

    float visibility = 1.0;
#if !defined(VL_CLOTH_CLOSURE_SATIN)
    visibility = AshikhminClothCorrelatedFactor(localV, localL, pV, pL);
#endif

    float fresnel = f0 + (1.0 - f0) * pow(
        1.0 - max(dot(view, halfVector), 0.0),
        5.0);
    float specular = density * eta * fresnel *
        visibility / (4.0 * gV * gL);

    float rV = AshikhminClothLookupR(localV, f0);
    float rL = AshikhminClothLookupR(localL, f0);
    float c = mix(constants.y, constants.z, f0);
    float diffuse = c * max(1.0 - rV, 0.0) * max(1.0 - rL, 0.0);

    outDiffuse = diffuseColor * diffuse * radiance * nDotL;
    outSpecular = specularTint * specular * radiance * nDotL;
    return outDiffuse + outSpecular;
}

vec3 EvaluateAshikhminClothIndirect(
    in vec3 normal,
    in vec3 view,
    in AshikhminClothFrame frame,
    in vec3 diffuseColor,
    in vec3 specularTint,
    in float f0,
    in vec3 averageRadiance,
    out vec3 outDiffuse,
    out vec3 outSpecular)
{
    vec3 localNormal = vec3(
        dot(normal, frame.x),
        dot(normal, frame.y),
        dot(normal, frame.n));
    float reflectance = AshikhminClothLookupR(localNormal, f0);
    outSpecular = averageRadiance * reflectance * specularTint;
    outDiffuse = averageRadiance * diffuseColor * max(1.0 - reflectance, 0.0);
    return outDiffuse + outSpecular;
}

#endif
