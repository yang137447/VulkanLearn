#ifndef VL_ROD_IMPLICIT_AIR
#define VL_ROD_IMPLICIT_AIR

const uint RodAirAvailable = 0u;
const uint RodAirInvalidInput = 1u;
const uint RodAirNonFinite = 2u;

struct RodImplicitAirSample
{
    uint status;
    vec3 tangent;
    vec3 relativeNormalVelocity;
    vec3 force;
    mat3 negativeVelocityDerivative;
};

bool RodAirFinite(float value)
{
    return !isnan(value) && !isinf(value);
}

bool RodAirFinite(vec3 value)
{
    return !any(isnan(value)) && !any(isinf(value));
}

bool RodFrozenAirTangent(vec3 previousOld, vec3 currentOld, vec3 nextOld,
    uint point, uint pointCount, out vec3 tangent)
{
    tangent = vec3(0);
    if (pointCount < 2u || point >= pointCount)
        return false;
    vec3 direction;
    if (point == 0u)
        direction = nextOld - currentOld;
    else if (point == pointCount - 1u)
        direction = currentOld - previousOld;
    else
    {
        vec3 first = currentOld - previousOld;
        vec3 second = nextOld - currentOld;
        float firstLength = length(first);
        float secondLength = length(second);
        if (!RodAirFinite(first) || !RodAirFinite(second) ||
            !RodAirFinite(firstLength) || !RodAirFinite(secondLength) ||
            !(firstLength > 1.0e-14) || !(secondLength > 1.0e-14))
            return false;
        direction = first / firstLength + second / secondLength;
    }
    float magnitude = length(direction);
    if (!RodAirFinite(direction) || !RodAirFinite(magnitude) || !(magnitude > 1.0e-14))
        return false;
    tangent = direction / magnitude;
    return RodAirFinite(tangent);
}

RodImplicitAirSample RodEvaluateImplicitAir(vec3 oldPositionMetres,
    vec3 candidatePositionMetres, vec3 frozenUnitTangent,
    float massKg, float stepSeconds, float betaPerMetre,
    vec3 airVelocityMetresPerSecond, float forceFraction)
{
    RodImplicitAirSample result = RodImplicitAirSample(RodAirInvalidInput,
        vec3(0), vec3(0), vec3(0), mat3(0));
    float tangentLength = length(frozenUnitTangent);
    if (!RodAirFinite(oldPositionMetres) || !RodAirFinite(candidatePositionMetres) ||
        !RodAirFinite(frozenUnitTangent) || !RodAirFinite(airVelocityMetresPerSecond) ||
        !RodAirFinite(massKg) || !(massKg > 0.0) ||
        !RodAirFinite(stepSeconds) || !(stepSeconds > 0.0) ||
        !RodAirFinite(betaPerMetre) || betaPerMetre < 0.0 ||
        !RodAirFinite(forceFraction) || forceFraction < 0.0 || forceFraction > 1.0 ||
        !RodAirFinite(tangentLength) || abs(tangentLength - 1.0) > 7.62939453125e-6)
        return result;
    result.tangent = frozenUnitTangent;
    mat3 projection = mat3(1) - outerProduct(frozenUnitTangent, frozenUnitTangent);
    vec3 velocity = (candidatePositionMetres - oldPositionMetres) / stepSeconds;
    result.relativeNormalVelocity = projection * (airVelocityMetresPerSecond - velocity);
    float magnitude = length(result.relativeNormalVelocity);
    float coefficient = massKg * betaPerMetre * forceFraction;
    result.force = coefficient * magnitude * result.relativeNormalVelocity;
    result.negativeVelocityDerivative = coefficient * magnitude * projection;
    if (magnitude > 0.0)
        result.negativeVelocityDerivative += coefficient *
            outerProduct(result.relativeNormalVelocity, result.relativeNormalVelocity) / magnitude;
    if (!RodAirFinite(coefficient) || !RodAirFinite(velocity) || !RodAirFinite(result.relativeNormalVelocity) ||
        !RodAirFinite(magnitude) || !RodAirFinite(result.force) ||
        !RodAirFinite(result.negativeVelocityDerivative[0]) ||
        !RodAirFinite(result.negativeVelocityDerivative[1]) ||
        !RodAirFinite(result.negativeVelocityDerivative[2]))
        result.status = RodAirNonFinite;
    else
        result.status = RodAirAvailable;
    return result;
}

#endif
