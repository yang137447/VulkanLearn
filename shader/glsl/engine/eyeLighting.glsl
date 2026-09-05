#ifndef VL_ENGINE_EYE_LIGHTING_GLSL
#define VL_ENGINE_EYE_LIGHTING_GLSL

// Eye evaluator 只消费 MaterialSurface 的冻结快照；Forward 与 Deferred 共用同一条账本。
struct EyeLightingResult
{
    vec3 directLighting;
    vec3 directDiffuse;
    vec3 directSpecular;
    vec3 indirectDiffuse;
    vec3 indirectSpecular;
    vec3 corneaSpecular;
    vec3 irisDirect;
    vec3 scleraDirect;
    vec3 innerIbl;
    vec3 finalColor;
    float shadowCornea;
    float shadowInner;
    float corneaFresnel;
    float transmissionIn;
    float transmissionOut;
    float irisHitDistance;
    vec2 irisUv;
    vec3 refractedViewDirection;
    float validIrisHit;
    float irisMask;
    float pupilMask;
    float limbusMask;
    float causticGain;
};

struct EyeIntersection
{
    vec3 point;
    vec3 propagationDirection;
    vec2 uv;
    float distance;
    float valid;
    float radial;
};

float EyeDielectricF0(float ior)
{
    float ratio = (1.0 - ior) / (1.0 + ior);
    return ratio * ratio;
}

float EyeFresnel(float cosine, float ior)
{
    float f0 = EyeDielectricF0(ior);
    return f0 + (1.0 - f0) * pow(
        clamp(1.0 - cosine, 0.0, 1.0),
        5.0);
}

EyeIntersection ResolveEyeIntersection(in MaterialSurface surface)
{
    EyeIntersection result;
    result.point = surface.worldPosition;
    result.propagationDirection =
        vec3(0.0, 0.0, -1.0);
    result.uv = surface.modelInputs.eye.irisUv;
    result.distance = surface.modelInputs.eye.irisHitDistance;
    result.valid = surface.modelInputs.eye.validIrisHit;
    result.radial = length((result.uv - vec2(0.5)) * 2.0);
    if (result.valid <= 0.5)
    {
        result.uv = vec2(0.0);
        result.distance = 0.0;
        result.radial = 1.0;
        return result;
    }

    vec3 viewDirectionToCamera = normalize(
        uboVP.cameraPosition - surface.worldPosition);
    result.propagationDirection = refract(
        -viewDirectionToCamera,
        surface.modelInputs.eye.corneaNormal,
        1.0 / surface.modelInputs.eye.corneaIor);
    vec3 irisPlanePoint = surface.worldPosition -
        surface.modelInputs.eye.irisPlaneNormal *
        surface.modelInputs.eye.irisDistance;
    result.point = surface.worldPosition +
        result.propagationDirection * result.distance;
    // Commit-time GBuffer validation and Base Pass geometry use the same plane.
    // Re-check the direction here so stale/invalid pixels become a neutral hit.
    float planeError = dot(
        surface.modelInputs.eye.irisPlaneNormal,
        result.point - irisPlanePoint);
    if (abs(planeError) > max(
            surface.modelInputs.eye.irisRadius * 0.05,
            1.0e-5))
    {
        result.valid = 0.0;
        result.uv = vec2(0.0);
        result.distance = 0.0;
        result.point = surface.worldPosition;
        result.radial = 1.0;
    }
    return result;
}

vec3 SampleEyeIrisColor(in MaterialSurface surface)
{
    return surface.modelInputs.eye.irisColor;
}

float SampleEyeIrisMask(in MaterialSurface surface)
{
    return surface.modelInputs.eye.irisMask *
        surface.modelInputs.eye.validIrisHit;
}

vec3 SampleEyeIrisNormal(in MaterialSurface surface)
{
    return normalize(surface.modelInputs.eye.irisNormal);
}

vec3 SampleEyeScleraColor(in MaterialSurface surface)
{
    return surface.modelInputs.eye.scleraColor;
}

vec3 ApplyEyeScleraDiffuseApproximation(
    vec3 diffuse,
    vec3 normal,
    vec3 lightDirectionToSource,
    float profileId)
{
    // MVP 的 wrap 近似保留为低阶 fallback；完整 screen-space profile pass
    // 在 composition 阶段只处理该 diffuse ledger，不会模糊 cornea specular。
    float profileWeight = step(0.5, profileId);
    float wrapWidth = mix(0.18, 0.30, profileWeight);
    float nDotL = max(dot(normal, lightDirectionToSource), 0.0);
    float wrapped = clamp(
        (nDotL + wrapWidth) / (1.0 + wrapWidth),
        0.0,
        1.0);
    float ratio = nDotL > 1.0e-4 ? wrapped / nDotL : 0.0;
    return diffuse * ratio;
}

float SampleEyeCaustic(
    in MaterialSurface surface,
    vec2 irisUv,
    vec3 lightDirectionToSource,
    float activeMask,
    out float transmission)
{
    transmission = 1.0;
    float profileId = surface.modelInputs.eye.causticProfileId;
    if (activeMask <= 0.0 ||
        surface.modelInputs.eye.validIrisHit <= 0.5 ||
        profileId < 0.5 || profileId > 15.0 ||
        irisUv.x < 0.0 || irisUv.x > 1.0 ||
        irisUv.y < 0.0 || irisUv.y > 1.0)
    {
        return 1.0;
    }

    float elevation = clamp(
        lightDirectionToSource.z * 0.5 + 0.5,
        0.0,
        1.0);
    float layer = floor(profileId + 0.5) * 16.0 +
        elevation * 15.0;
    vec4 lookup = texture(eyeCausticLut, vec3(irisUv, layer));
    float strength = surface.modelInputs.eye.causticStrength;
    transmission = mix(1.0, lookup.g, strength);
    return mix(1.0, lookup.r, strength);
}

EyeLightingResult CreateDefaultEyeLightingResult()
{
    EyeLightingResult result;
    result.directLighting = vec3(0.0);
    result.directDiffuse = vec3(0.0);
    result.directSpecular = vec3(0.0);
    result.indirectDiffuse = vec3(0.0);
    result.indirectSpecular = vec3(0.0);
    result.corneaSpecular = vec3(0.0);
    result.irisDirect = vec3(0.0);
    result.scleraDirect = vec3(0.0);
    result.innerIbl = vec3(0.0);
    result.finalColor = vec3(0.0);
    result.shadowCornea = 1.0;
    result.shadowInner = 1.0;
    result.corneaFresnel = 0.0;
    result.transmissionIn = 1.0;
    result.transmissionOut = 1.0;
    result.irisHitDistance = 0.0;
    result.irisUv = vec2(0.0);
    result.refractedViewDirection = vec3(0.0, 0.0, -1.0);
    result.validIrisHit = 0.0;
    result.irisMask = 0.0;
    result.pupilMask = 0.0;
    result.limbusMask = 0.0;
    result.causticGain = 1.0;
    return result;
}

EyeLightingResult ShadeEyeSurface(in MaterialSurface surface)
{
    EyeLightingResult result = CreateDefaultEyeLightingResult();
    EyeMaterialInputs eye = surface.modelInputs.eye;
    vec3 corneaNormal = normalize(eye.corneaNormal);
    vec3 viewDirectionToCamera = normalize(
        uboVP.cameraPosition - surface.worldPosition);
    vec3 irisPlaneNormal = normalize(eye.irisPlaneNormal);
    EyeIntersection intersection = ResolveEyeIntersection(surface);

    result.irisHitDistance = intersection.distance;
    result.irisUv = intersection.uv;
    result.refractedViewDirection = intersection.propagationDirection;
    result.validIrisHit = intersection.valid;
    result.irisMask = SampleEyeIrisMask(surface);
    result.pupilMask = eye.pupilMask * result.irisMask;
    result.limbusMask = eye.limbusMask * intersection.valid;

    float viewCosine = max(
        dot(corneaNormal, viewDirectionToCamera),
        0.0);
    result.corneaFresnel = EyeFresnel(viewCosine, eye.corneaIor);
    result.transmissionOut = 1.0 - result.corneaFresnel;

    vec3 innerNormal = SampleEyeIrisNormal(surface);
    vec3 irisColor = SampleEyeIrisColor(surface);
    vec3 scleraColor = SampleEyeScleraColor(surface);
    vec3 lightDirectionToSource = corneaNormal;
    if (uboLight.directionalLightCount > 0)
    {
        lightDirectionToSource = normalize(
            -uboLight.lights[uboLight.directionalLightOffset].directionPad.xyz);
    }
    float lightCosine = max(
        dot(corneaNormal, lightDirectionToSource),
        0.0);
    result.transmissionIn = 1.0 - EyeFresnel(lightCosine, eye.corneaIor);

    int corneaCascadeIndex = 0;
    result.shadowCornea = CalculateCsmShadow(
        shadowMap,
        surface.worldPosition,
        corneaNormal,
        corneaCascadeIndex);
    int innerCascadeIndex = 0;
    result.shadowInner = intersection.valid > 0.5
        ? CalculateCsmShadow(
            shadowMap,
            intersection.point,
            innerNormal,
            innerCascadeIndex)
        : result.shadowCornea;
    result.shadowInner *= surface.modelInputs.eye.contactVisibility;
    result.shadowCornea *= surface.modelInputs.eye.ciliaVisibility;

    LightingLobes corneaLobes = CalculateDirectLightingLobes(
        corneaNormal,
        surface.worldPosition,
        uboVP.cameraPosition,
        vec3(0.0),
        surface.roughness,
        0.0,
        0.5);
    float f0Scale = EyeDielectricF0(eye.corneaIor) / 0.04;
    result.corneaSpecular = corneaLobes.specular * f0Scale *
        result.shadowCornea;

    vec3 irisResponseColor = irisColor * (1.0 - result.pupilMask);
    irisResponseColor *= mix(
        1.0,
        0.72,
        result.limbusMask * result.irisMask);
    LightingLobes irisLobes = CalculateDirectLightingLobes(
        innerNormal,
        intersection.point,
        uboVP.cameraPosition,
        irisResponseColor,
        0.8,
        0.0,
        0.5);
    LightingLobes scleraLobes = CalculateDirectLightingLobes(
        irisPlaneNormal,
        intersection.point,
        uboVP.cameraPosition,
        scleraColor,
        0.9,
        0.0,
        0.5);
    scleraLobes.diffuse = ApplyEyeScleraDiffuseApproximation(
        scleraLobes.diffuse,
        irisPlaneNormal,
        lightDirectionToSource,
        eye.scleraProfileId);

    float causticTransmission = 1.0;
    result.causticGain = SampleEyeCaustic(
        surface,
        intersection.uv,
        lightDirectionToSource,
        result.irisMask,
        causticTransmission);
    result.irisDirect = irisLobes.diffuse * result.irisMask *
        result.transmissionIn * causticTransmission *
        result.transmissionOut * result.causticGain * result.shadowInner;
    result.scleraDirect = scleraLobes.diffuse *
        (1.0 - result.irisMask) *
        result.transmissionIn * result.transmissionOut *
        result.shadowInner;
    result.directDiffuse = result.irisDirect + result.scleraDirect;
    result.directSpecular = result.corneaSpecular;
    result.directLighting = result.directDiffuse + result.directSpecular;

    result.innerIbl =
        (CalculateDiffuseIbl(
            irisPlaneNormal,
            scleraColor,
            0.0) * (1.0 - result.irisMask) +
         CalculateDiffuseIbl(
            innerNormal,
            irisResponseColor,
            0.0) * result.irisMask) *
        result.transmissionIn * result.transmissionOut;
    result.indirectDiffuse = result.innerIbl;
    result.indirectSpecular = CalculateSpecularIblWithF0(
        corneaNormal,
        viewDirectionToCamera,
        vec3(EyeDielectricF0(eye.corneaIor)),
        surface.roughness);
    result.indirectSpecular *= result.transmissionOut;
    result.finalColor =
        result.directLighting +
        (result.indirectDiffuse + result.indirectSpecular) *
            surface.ambientOcclusion +
        surface.emissiveColor;
    return result;
}

#endif
