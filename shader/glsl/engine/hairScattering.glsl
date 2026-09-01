#ifndef VL_ENGINE_HAIR_SCATTERING_GLSL
#define VL_ENGINE_HAIR_SCATTERING_GLSL

#include "materialSurface.glsl"

const uint HAIR_PATH_R = 0u;
const uint HAIR_PATH_TT = 1u;
const uint HAIR_PATH_TRT = 2u;
const float HAIR_PI = 3.14159265358979323846;
const float HAIR_HALF_PI = 1.57079632679489661923;
const float HAIR_UE_F0 = 0.046521;
const float HAIR_UE_ONE_MINUS_F0 = 0.953479;
const float HAIR_UE_AMBIENT_AREA = 0.2;

struct HairTangentFrame
{
    vec3 normal;
    vec3 bitangent;
    vec3 tangent;
    vec3 geometricNormal;
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

struct HairUeScatteringContext
{
    float viewLightDot;
    float sinThetaL;
    float sinThetaV;
    float cosThetaD;
    float cosPhi;
    float cosHalfPhi;
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
    // NeoX Hair 的 GBuffer 语义有意不同于普通 PBR：WorldNormal 是像素
    // fiber axis，WorldTangent.xyz 是卡片几何法线。交换读取会让 HairBxDF
    // 围绕错误轴计算 R/TT/TRT，并把几何半兰伯特误用到 fiber axis。
    frame.tangent = normalize(surface.worldNormal);
    frame.geometricNormal = normalize(surface.worldTangent.xyz);
    frame.handedness = surface.worldTangent.w < 0.0 ? -1.0 : 1.0;
    vec3 projectedNormal = frame.geometricNormal - frame.tangent *
        dot(frame.geometricNormal, frame.tangent);
    float projectedNormalLengthSquared = dot(
        projectedNormal,
        projectedNormal);
    // Fiber axis 与几何法线接近平行时，使用一个稳定的世界轴生成横截面基底。
    vec3 fallbackNormal = abs(frame.tangent.z) < 0.9
        ? vec3(0.0, 0.0, 1.0)
        : vec3(0.0, 1.0, 0.0);
    projectedNormal = projectedNormalLengthSquared > 1.0e-8
        ? projectedNormal * inversesqrt(projectedNormalLengthSquared)
        : normalize(fallbackNormal - frame.tangent * dot(fallbackNormal, frame.tangent));
    frame.normal = projectedNormal;
    frame.bitangent = normalize(cross(frame.tangent, frame.normal)) *
        frame.handedness;
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

vec3 EvaluateHairBaseColorResponse(
    in MaterialSurface surface,
    float visibility)
{
    vec3 baseColor;
#if defined(RENDER_MODE_FORWARD_OPAQUE) || \
    defined(RENDER_MODE_FORWARD_EYE_INNER) || \
    defined(RENDER_MODE_FORWARD_EYE_CORNEA) || \
    defined(RENDER_MODE_TRANSPARENT_ALPHA_BLEND) || \
    defined(RENDER_MODE_TRANSPARENT_ADDITIVE)
    baseColor = surface.baseColor;
#else
    // Deferred Hair 的 GBuffer A 保存 sigma_a；按当前 Hair ABI 反解回源 BaseColor，
    // 这样 Core 与 Forward Fringe 使用同一份发色，不会让高光脱离黑色发束。
    baseColor = exp(-max(surface.hairAbsorption, vec3(0.0)) *
        (4.0 * 0.00005));
#endif
    baseColor = max(baseColor, vec3(1.0 / 255.0));
    float luminance = max(
        dot(baseColor, vec3(0.3, 0.59, 0.11)),
        1.0 / 255.0);
    vec3 normalizedBaseColor = baseColor / luminance;
    float shadowExponent = 1.0 - clamp(visibility, 0.0, 1.0);
    return sqrt(baseColor) * pow(
        normalizedBaseColor,
        vec3(shadowExponent));
}

HairUeScatteringContext BuildHairUeScatteringContext(
    vec3 tangent,
    vec3 lightDirection,
    vec3 viewDirection)
{
    HairUeScatteringContext context;
    context.viewLightDot = dot(viewDirection, lightDirection);
    context.sinThetaL = clamp(dot(tangent, lightDirection), -1.0, 1.0);
    context.sinThetaV = clamp(dot(tangent, viewDirection), -1.0, 1.0);
    context.cosThetaD = cos(0.5 * abs(
        asin(context.sinThetaV) - asin(context.sinThetaL)));

    vec3 projectedLight = lightDirection - context.sinThetaL * tangent;
    vec3 projectedView = viewDirection - context.sinThetaV * tangent;
    context.cosPhi = dot(projectedLight, projectedView) * inversesqrt(
        dot(projectedLight, projectedLight) *
            dot(projectedView, projectedView) +
        1.0e-4);
    context.cosHalfPhi = sqrt(clamp(
        0.5 + 0.5 * context.cosPhi,
        0.0,
        1.0));
    return context;
}

float EvaluateHairUeGaussian(float width, float theta)
{
    // cosHalfPhi 在严格反向时可把 R 的宽度压到零；这是几何退化，不是作者数据错误。
    float stableWidth = max(width, 1.0e-4);
    return exp(-0.5 * theta * theta / (stableWidth * stableWidth)) /
        (sqrt(2.0 * HAIR_PI) * stableWidth);
}

float EvaluateHairUeFresnel(float viewLightDot)
{
    // NeoX/UE Hair 的 R lobe 使用 half-vector Fresnel 近似，F0=0.046521
    // 与源 shader 的固定发丝 IOR 对齐；这里不是普通 PBR 的 H·V Schlick。
    float grazing = 1.0 - sqrt(clamp(
        0.5 + 0.5 * viewLightDot,
        0.0,
        1.0));
    return HAIR_UE_F0 + HAIR_UE_ONE_MINUS_F0 *
        grazing * grazing * grazing * grazing * grazing;
}

float EvaluateHairUeInternalFresnel(float cosThetaD, float eta)
{
    float interfaceCosine = 1.0 - cosThetaD * sqrt(max(1.0 - eta * eta, 0.0));
    float interfacePow5 = interfaceCosine * interfaceCosine;
    interfacePow5 *= interfacePow5 * interfaceCosine;
    return HAIR_UE_ONE_MINUS_F0 * (1.0 - interfacePow5);
}

vec3 EvaluateHairUePathColor(HairMaterialInputs hair)
{
    // UE-facing BaseColor 在材质入口只转换一次；这里用同一参考光程恢复 path tint，
    // 使 Forward/Deferred 都消费相同颜色，而不是再次把 BaseColor 乘进各条路径。
    return EvaluateHairBeerLambert(
        hair.absorption,
        4.0 * hair.fiberRadius);
}

vec3 EvaluateHairUeR(
    HairUeScatteringContext context,
    HairMaterialInputs hair,
    float specular,
    float area)
{
    float alpha = -2.0 * hair.cuticleTilt;
    float sinAlpha = sin(alpha);
    float cosAlpha = cos(alpha);
    float shift = 2.0 * sinAlpha * (
        cosAlpha * context.cosHalfPhi *
            sqrt(max(1.0 - context.sinThetaV * context.sinThetaV, 0.0)) +
        sinAlpha * context.sinThetaV);
    // alpha=-0.07（默认 cuticleTilt=0.035）正好对应源生成代码中的
    // -0.139886、0.997551、-0.069943 三个常量。
    float roughnessSquared = hair.longitudinalRoughness *
        hair.longitudinalRoughness;
    float width = (area + roughnessSquared) *
        sqrt(2.0) * context.cosHalfPhi;
    float longitudinal = EvaluateHairUeGaussian(
        width,
        context.sinThetaL + context.sinThetaV - shift);
    float azimuthal = 0.25 * context.cosHalfPhi;
    float fresnel = EvaluateHairUeFresnel(context.viewLightDot);
    return vec3(
        longitudinal * azimuthal * fresnel * specular * 2.0);
}

vec3 EvaluateHairUeTT(
    HairUeScatteringContext context,
    HairMaterialInputs hair,
    vec3 pathColor,
    float area)
{
    float alpha = hair.cuticleTilt;
    float roughnessSquared = hair.longitudinalRoughness *
        hair.longitudinalRoughness;
    float width = area + 0.5 * roughnessSquared;
    float longitudinal = EvaluateHairUeGaussian(
        width,
        context.sinThetaL + context.sinThetaV - alpha);
    float stableCosThetaD = max(context.cosThetaD, 1.0e-4);
    float nPrime = 1.19 / stableCosThetaD + 0.36 * stableCosThetaD;
    float inverseNPrime = 1.0 / nPrime;
    float eta = context.cosHalfPhi *
        (1.0 + inverseNPrime * (0.6 - 0.8 * context.cosPhi));
    float interfaceWeight = EvaluateHairUeInternalFresnel(
        stableCosThetaD,
        eta);
    float pathExponent = 0.5 *
        sqrt(max(
            1.0 - (eta * inverseNPrime) * (eta * inverseNPrime),
            0.0)) /
        stableCosThetaD;
    vec3 transmittance = pow(
        max(pathColor, vec3(1.0 / 255.0)),
        vec3(pathExponent));
    float azimuthal = exp(-3.65 * context.cosPhi - 3.98);
    return longitudinal * azimuthal *
        interfaceWeight * interfaceWeight *
        transmittance;
}

vec3 EvaluateHairUeTRT(
    HairUeScatteringContext context,
    HairMaterialInputs hair,
    vec3 pathColor,
    float area)
{
    float alpha = 4.0 * hair.cuticleTilt;
    float roughnessSquared = hair.longitudinalRoughness *
        hair.longitudinalRoughness;
    float width = area + 2.0 * roughnessSquared;
    float longitudinal = EvaluateHairUeGaussian(
        width,
        context.sinThetaL + context.sinThetaV - alpha);
    float stableCosThetaD = max(context.cosThetaD, 1.0e-4);
    float interfaceCosine = 1.0 - stableCosThetaD * 0.5;
    float interfacePow5 = interfaceCosine * interfaceCosine;
    interfacePow5 *= interfacePow5 * interfaceCosine;
    float interfaceWeight =
        HAIR_UE_ONE_MINUS_F0 - HAIR_UE_ONE_MINUS_F0 * interfacePow5;
    float internalReflectionWeight =
        HAIR_UE_F0 + HAIR_UE_ONE_MINUS_F0 * interfacePow5;
    vec3 transmittance = pow(
        max(pathColor, vec3(1.0 / 255.0)),
        vec3(0.8 / stableCosThetaD));
    float azimuthal = exp(17.0 * context.cosPhi - 16.78);
    return longitudinal * azimuthal *
        interfaceWeight * interfaceWeight * internalReflectionWeight *
        transmittance;
}

vec3 EvaluateHairUeScatterBase(
    HairUeScatteringContext context,
    HairMaterialInputs hair,
    vec3 tangent,
    vec3 fallbackNormal,
    vec3 lightDirection,
    vec3 viewDirection)
{
    float kajiyaDiffuse = 1.0 - abs(context.sinThetaL);
    vec3 projectedView = viewDirection - tangent * context.sinThetaV;
    float projectedViewLengthSquared = dot(projectedView, projectedView);
    // 视线与发丝轴重合时投影方向退化，使用 card 几何法线维持有限响应。
    vec3 fakeNormal = projectedViewLengthSquared > 1.0e-8
        ? projectedView * inversesqrt(projectedViewLengthSquared)
        : fallbackNormal;
    float wrappedNoL = clamp(
        (dot(fakeNormal, lightDirection) + 1.0) / 4.0,
        0.0,
        1.0);
    float diffuseScatter = (1.0 / HAIR_PI) *
        mix(wrappedNoL, kajiyaDiffuse, 0.33) *
        hair.scatter;
    // 源公式的 shadow/contact 项由外层 visibility 处理；不要再次用 shadow
    // 改变颜色指数，否则阴影会把发色推向白色并破坏 BaseColor 单调性。
    return vec3(diffuseScatter);
}

vec3 EvaluateHairUeScatter(
    HairUeScatteringContext context,
    HairMaterialInputs hair,
    vec3 tangent,
    vec3 fallbackNormal,
    vec3 lightDirection,
    vec3 viewDirection,
    vec3 pathColor)
{
    return sqrt(pathColor) * EvaluateHairUeScatterBase(
        context,
        hair,
        tangent,
        fallbackNormal,
        lightDirection,
        viewDirection);
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
    // LUT kernel v2 已按 CPU oracle 写入完整界面 Fresnel 与 Jacobian；
    // runtime 只乘 longitudinal lobe，禁止再次修饰 TT/TRT 路径能量。
    return pathWeight;
}

#endif
