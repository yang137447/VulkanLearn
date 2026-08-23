#ifndef VL_ENGINE_HAIR_SCATTERING_GLSL
#define VL_ENGINE_HAIR_SCATTERING_GLSL

#include "materialSurface.glsl"

const uint HAIR_PATH_R = 0u;
const uint HAIR_PATH_TT = 1u;
const uint HAIR_PATH_TRT = 2u;
const float HAIR_PI = 3.14159265358979323846;
const float HAIR_HALF_PI = 1.57079632679489661923;

struct HairTangentFrame
{
    vec3 normal;
    vec3 bitangent;
    vec3 tangent;
    float handedness;
};

struct HairAngles
{
    float thetaI;
    float thetaO;
    float thetaH;
    float thetaD;
    float phiI;
    float phiO;
    float deltaPhi;
};

struct HairVisibilityInputs
{
    float fiberVisibility;
    float selfShadow;
    float transmittance;
    float lod;
    float clod;
    float density;
};

HairVisibilityInputs BuildHairVisibilityInputs(HairMaterialInputs hair)
{
    HairVisibilityInputs visibility;
    // Card 路径暂时没有独立 strands backend；保留接口并使用作者 coverage/density，
    // self-shadow/transmittance/LOD 由后续 visibility provider 注入。
    visibility.fiberVisibility = hair.coverage;
    visibility.selfShadow = 1.0;
    visibility.transmittance = 1.0;
    visibility.lod = 0.0;
    visibility.clod = 0.0;
    visibility.density = hair.density;
    return visibility;
}

float EvaluateHairVisibility(HairVisibilityInputs visibility)
{
    return visibility.fiberVisibility *
        visibility.selfShadow *
        visibility.transmittance;
}

struct HairAzimuthalSample
{
    float response;
    float pathLength;
    float interfaceWeight;
    float jacobian;
    vec2 coordinates;
};

HairTangentFrame BuildHairTangentFrame(in MaterialSurface surface)
{
    HairTangentFrame frame;
    frame.normal = normalize(surface.worldNormal);
    frame.tangent = normalize(
        surface.worldTangent.xyz -
        frame.normal * dot(frame.normal, surface.worldTangent.xyz));
    frame.handedness = surface.worldTangent.w < 0.0 ? -1.0 : 1.0;
    frame.bitangent = normalize(cross(frame.normal, frame.tangent)) * frame.handedness;
    return frame;
}

float WrapHairAngle(float angle)
{
    return angle - 2.0 * HAIR_PI * floor((angle + HAIR_PI) / (2.0 * HAIR_PI));
}

HairAngles ComputeHairAngles(
    in HairTangentFrame frame,
    vec3 incidentDirection,
    vec3 outgoingDirection)
{
    vec3 incident = normalize(incidentDirection);
    vec3 outgoing = normalize(outgoingDirection);
    HairAngles angles;
    angles.thetaI = asin(clamp(dot(incident, frame.tangent), -1.0, 1.0));
    angles.thetaO = asin(clamp(dot(outgoing, frame.tangent), -1.0, 1.0));
    angles.thetaH = 0.5 * (angles.thetaI + angles.thetaO);
    angles.thetaD = 0.5 * (angles.thetaO - angles.thetaI);
    angles.phiI = atan(
        dot(incident, frame.bitangent),
        dot(incident, frame.normal));
    angles.phiO = atan(
        dot(outgoing, frame.bitangent),
        dot(outgoing, frame.normal));
    angles.deltaPhi = WrapHairAngle(angles.phiO - angles.phiI);
    return angles;
}

float EvaluateHairDielectricFresnel(float cosineIncident, float relativeIor)
{
    float cosine = abs(clamp(cosineIncident, -1.0, 1.0));
    float sine = sqrt(max(1.0 - cosine * cosine, 0.0));
    float transmittedSine = sine / relativeIor;
    if (transmittedSine >= 1.0)
    {
        return 1.0;
    }
    float transmittedCosine = sqrt(max(1.0 - transmittedSine * transmittedSine, 0.0));
    float parallel =
        (relativeIor * cosine - transmittedCosine) /
        (relativeIor * cosine + transmittedCosine);
    float perpendicular =
        (cosine - relativeIor * transmittedCosine) /
        (cosine + relativeIor * transmittedCosine);
    return clamp(
        0.5 * (parallel * parallel + perpendicular * perpendicular),
        0.0,
        1.0);
}

vec3 EvaluateHairBeerLambert(vec3 absorption, float pathLength)
{
    return exp(-max(absorption, vec3(0.0)) * max(pathLength, 0.0));
}

float HairPathLongitudinalShift(uint path, float cuticleTilt)
{
    if (path == HAIR_PATH_TT)
    {
        return cuticleTilt;
    }
    if (path == HAIR_PATH_TRT)
    {
        return -cuticleTilt;
    }
    return 0.0;
}

float EvaluateHairLongitudinalLobe(
    uint path,
    float thetaH,
    float cuticleTilt,
    float roughness)
{
    float width = max(roughness, 0.02);
    float deviation = WrapHairAngle(
        thetaH - HairPathLongitudinalShift(path, cuticleTilt));
    return exp(-0.5 * deviation * deviation / (width * width)) /
        (sqrt(2.0 * HAIR_PI) * width);
}

HairAzimuthalSample SampleHairAzimuthalLut(
    HairAngles angles,
    HairMaterialInputs hair,
    uint path)
{
    float thetaDCoordinate =
        clamp(angles.thetaD / HAIR_PI + 0.5, 0.0, 1.0);
    float roughnessCoordinate = clamp(
        hair.azimuthalRoughness,
        0.0,
        1.0);
    vec2 coordinates = vec2(
        fract(angles.deltaPhi / (2.0 * HAIR_PI) + 0.5),
        (roughnessCoordinate * 7.0 * 64.0 + thetaDCoordinate * 63.0 + 0.5) /
            (8.0 * 64.0));
    vec4 sampleValue = texture(hairAzimuthalLut, vec3(coordinates, float(path)));
    HairAzimuthalSample sampleResult;
    sampleResult.response = sampleValue.r;
    sampleResult.pathLength = sampleValue.g;
    sampleResult.interfaceWeight = sampleValue.b;
    sampleResult.jacobian = sampleValue.a;
    sampleResult.coordinates = coordinates;
    return sampleResult;
}

vec3 EvaluateHairPathTransmittance(
    uint path,
    HairAzimuthalSample azimuthal,
    HairMaterialInputs hair)
{
    if (path == HAIR_PATH_R)
    {
        // R 是表面反射，不进入纤维内部，也不读取 absorption。
        return vec3(1.0);
    }
    float radiusScale = hair.fiberRadius / 0.00005;
    return EvaluateHairBeerLambert(
        hair.absorption,
        azimuthal.pathLength * radiusScale);
}

float EvaluateHairPathWeight(
    uint path,
    HairAngles angles,
    HairMaterialInputs hair,
    HairAzimuthalSample azimuthal)
{
    float longitudinal = EvaluateHairLongitudinalLobe(
        path,
        angles.thetaH,
        hair.cuticleTilt,
        hair.longitudinalRoughness);
    float pathWeight = longitudinal * azimuthal.response;
    if (path == HAIR_PATH_TT)
    {
        float internalCosine = sqrt(max(
            1.0 -
                sin(angles.thetaD) * sin(angles.thetaD) /
                    (hair.ior * hair.ior),
            0.0));
        float internalFresnel = EvaluateHairDielectricFresnel(
            internalCosine,
            1.0 / hair.ior);
        pathWeight *= mix(1.0, 1.0 - internalFresnel, 0.25);
    }
    else if (path == HAIR_PATH_TRT)
    {
        pathWeight *= 0.75;
    }
    return pathWeight;
}

#endif
