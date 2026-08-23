#include "hairReference.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <stdexcept>

namespace VL::Hair
{
namespace
{

constexpr float RootHeightLimit = 0.9999f;
constexpr float RootDuplicateTolerance = 1.0e-3f;
constexpr float RootResidualTolerance = 2.0e-3f;
constexpr float NumericalEpsilon = 1.0e-6f;

float ClampUnit(float value) noexcept
{
    return std::clamp(value, -1.0f, 1.0f);
}

float GetLongitudinalShift(HairPath path, float cuticleTilt) noexcept
{
    switch (path)
    {
    case HairPath::R:
        return 0.0f;
    case HairPath::TT:
        return cuticleTilt;
    case HairPath::TRT:
        return -cuticleTilt;
    }
    return 0.0f;
}

float ComputeResidual(
    HairPath path,
    float height,
    float thetaD,
    float deltaPhi,
    float ior)
{
    return WrapAngle(
        ComputeAzimuthalPathAngle(path, height, thetaD, ior) - deltaPhi);
}

void AppendRootIfUnique(
    std::vector<HairAzimuthalRoot>& roots,
    float height,
    float jacobian)
{
    if (!std::isfinite(height) || !std::isfinite(jacobian) || jacobian <= 0.0f)
    {
        return;
    }

    for (const HairAzimuthalRoot& root : roots)
    {
        if (std::abs(root.height - height) <= RootDuplicateTolerance)
        {
            return;
        }
    }
    roots.push_back({height, jacobian});
}

float DerivativeOfPathAngle(
    HairPath path,
    float height,
    float thetaD,
    float ior)
{
    const float step = 1.0e-4f;
    const float lowerHeight = std::max(-RootHeightLimit, height - step);
    const float upperHeight = std::min(RootHeightLimit, height + step);
    const float lowerAngle =
        ComputeAzimuthalPathAngle(path, lowerHeight, thetaD, ior);
    const float upperAngle =
        ComputeAzimuthalPathAngle(path, upperHeight, thetaD, ior);
    const float denominator = upperHeight - lowerHeight;
    if (!std::isfinite(lowerAngle) ||
        !std::isfinite(upperAngle) ||
        denominator <= NumericalEpsilon)
    {
        return 0.0f;
    }
    return (upperAngle - lowerAngle) / denominator;
}

float RefineRoot(
    HairPath path,
    float thetaD,
    float deltaPhi,
    float ior,
    float lowerHeight,
    float upperHeight)
{
    float lowerResidual =
        ComputeResidual(path, lowerHeight, thetaD, deltaPhi, ior);
    float upperResidual =
        ComputeResidual(path, upperHeight, thetaD, deltaPhi, ior);
    for (size_t iteration = 0; iteration < 32; ++iteration)
    {
        const float middleHeight = (lowerHeight + upperHeight) * 0.5f;
        const float middleResidual =
            ComputeResidual(path, middleHeight, thetaD, deltaPhi, ior);
        if (std::abs(middleResidual) <= RootResidualTolerance)
        {
            return middleHeight;
        }

        if (lowerResidual * middleResidual <= 0.0f)
        {
            upperHeight = middleHeight;
            upperResidual = middleResidual;
        }
        else
        {
            lowerHeight = middleHeight;
            lowerResidual = middleResidual;
        }
    }
    return (lowerHeight + upperHeight) * 0.5f;
}

} // namespace

const char* ToString(HairPath path) noexcept
{
    switch (path)
    {
    case HairPath::R:
        return "R";
    case HairPath::TT:
        return "TT";
    case HairPath::TRT:
        return "TRT";
    }
    return "Unknown";
}

HairVec3 Add(HairVec3 left, HairVec3 right) noexcept
{
    return {left.x + right.x, left.y + right.y, left.z + right.z};
}

HairVec3 Subtract(HairVec3 left, HairVec3 right) noexcept
{
    return {left.x - right.x, left.y - right.y, left.z - right.z};
}

HairVec3 Multiply(HairVec3 value, float scalar) noexcept
{
    return {value.x * scalar, value.y * scalar, value.z * scalar};
}

float Dot(HairVec3 left, HairVec3 right) noexcept
{
    return left.x * right.x + left.y * right.y + left.z * right.z;
}

HairVec3 Cross(HairVec3 left, HairVec3 right) noexcept
{
    return {
        left.y * right.z - left.z * right.y,
        left.z * right.x - left.x * right.z,
        left.x * right.y - left.y * right.x};
}

float Length(HairVec3 value) noexcept
{
    return std::sqrt(Dot(value, value));
}

HairVec3 Normalize(HairVec3 value)
{
    const float length = Length(value);
    if (!std::isfinite(length) || length <= NumericalEpsilon)
    {
        throw std::runtime_error("Hair reference cannot normalize a zero vector");
    }
    return Multiply(value, 1.0f / length);
}

HairTangentFrame BuildTangentFrame(
    HairVec3 worldNormal,
    HairVec3 worldTangent,
    float handedness)
{
    HairTangentFrame frame;
    frame.normal = Normalize(worldNormal);
    frame.tangent = Normalize(
        Subtract(
            worldTangent,
            Multiply(frame.normal, Dot(frame.normal, worldTangent))));
    frame.handedness = handedness < 0.0f ? -1.0f : 1.0f;
    frame.bitangent = Multiply(
        Cross(frame.normal, frame.tangent),
        frame.handedness);
    return frame;
}

HairAngles ComputeHairAngles(
    const HairTangentFrame& frame,
    HairVec3 incidentDirection,
    HairVec3 outgoingDirection)
{
    const HairVec3 incident = Normalize(incidentDirection);
    const HairVec3 outgoing = Normalize(outgoingDirection);
    HairAngles angles;
    angles.thetaI = std::asin(ClampUnit(Dot(incident, frame.tangent)));
    angles.thetaO = std::asin(ClampUnit(Dot(outgoing, frame.tangent)));
    angles.thetaH = (angles.thetaI + angles.thetaO) * 0.5f;
    angles.thetaD = (angles.thetaO - angles.thetaI) * 0.5f;
    angles.phiI = std::atan2(
        Dot(incident, frame.bitangent),
        Dot(incident, frame.normal));
    angles.phiO = std::atan2(
        Dot(outgoing, frame.bitangent),
        Dot(outgoing, frame.normal));
    angles.deltaPhi = WrapAngle(angles.phiO - angles.phiI);
    return angles;
}

float ComputeAzimuthalPathAngle(
    HairPath path,
    float height,
    float thetaD,
    float ior)
{
    const float effectiveCosine = std::cos(thetaD);
    if (effectiveCosine <= NumericalEpsilon || ior <= 1.0f)
    {
        return std::numeric_limits<float>::quiet_NaN();
    }

    const float effectiveIor = std::sqrt(
        std::max(
            ior * ior - std::sin(thetaD) * std::sin(thetaD),
            NumericalEpsilon)) /
        effectiveCosine;
    const float incidentGamma = std::asin(ClampUnit(height));
    const float transmittedRatio = height / effectiveIor;
    if (std::abs(transmittedRatio) > 1.0f)
    {
        return std::numeric_limits<float>::quiet_NaN();
    }
    const float transmittedGamma = std::asin(transmittedRatio);
    const float pathIndex = static_cast<float>(static_cast<uint32_t>(path));
    return 2.0f * pathIndex * transmittedGamma -
        2.0f * incidentGamma +
        pathIndex * HairPi;
}

std::vector<HairAzimuthalRoot> FindAzimuthalRoots(
    HairPath path,
    float thetaD,
    float deltaPhi,
    float ior,
    size_t sampleCount)
{
    if (sampleCount < 3 || ior <= 1.0f)
    {
        throw std::invalid_argument(
            "Hair azimuthal root search requires at least three samples and IOR > 1");
    }

    const float normalizedDeltaPhi = WrapAngle(deltaPhi);
    std::vector<HairAzimuthalRoot> roots;
    float previousHeight = -RootHeightLimit;
    float previousResidual = ComputeResidual(
        path,
        previousHeight,
        thetaD,
        normalizedDeltaPhi,
        ior);
    for (size_t sampleIndex = 1; sampleIndex < sampleCount; ++sampleIndex)
    {
        const float currentHeight =
            -RootHeightLimit +
            2.0f * RootHeightLimit *
                static_cast<float>(sampleIndex) /
                static_cast<float>(sampleCount - 1);
        const float currentResidual = ComputeResidual(
            path,
            currentHeight,
            thetaD,
            normalizedDeltaPhi,
            ior);
        if (std::isfinite(currentResidual))
        {
            if (std::abs(currentResidual) <= RootResidualTolerance)
            {
                const float derivative = std::abs(DerivativeOfPathAngle(
                    path,
                    currentHeight,
                    thetaD,
                    ior));
                AppendRootIfUnique(roots, currentHeight, derivative);
            }
            if (std::isfinite(previousResidual) &&
                previousResidual * currentResidual < 0.0f)
            {
                const float rootHeight = RefineRoot(
                    path,
                    thetaD,
                    normalizedDeltaPhi,
                    ior,
                    previousHeight,
                    currentHeight);
                const float derivative = std::abs(DerivativeOfPathAngle(
                    path,
                    rootHeight,
                    thetaD,
                    ior));
                AppendRootIfUnique(roots, rootHeight, derivative);
            }
        }
        previousHeight = currentHeight;
        previousResidual = currentResidual;
    }

    std::sort(
        roots.begin(),
        roots.end(),
        [](const HairAzimuthalRoot& left, const HairAzimuthalRoot& right) {
            return left.height < right.height;
        });
    return roots;
}

float EvaluateLongitudinalLobe(
    HairPath path,
    float thetaH,
    float cuticleTilt,
    float roughness)
{
    const float effectiveRoughness = std::max(roughness, 1.0e-4f);
    const float deviation = WrapAngle(
        thetaH - GetLongitudinalShift(path, cuticleTilt));
    const float normalizedGaussian = std::exp(
        -0.5f * deviation * deviation /
        (effectiveRoughness * effectiveRoughness));
    return normalizedGaussian /
        (std::sqrt(2.0f * HairPi) * effectiveRoughness);
}

float EvaluateDielectricFresnel(float cosineIncident, float ior)
{
    const float incidentCosine = std::abs(ClampUnit(cosineIncident));
    if (ior <= NumericalEpsilon)
    {
        throw std::invalid_argument("Hair dielectric relative IOR must be positive");
    }
    const float sineIncident = std::sqrt(
        std::max(1.0f - incidentCosine * incidentCosine, 0.0f));
    const float sineTransmitted = sineIncident / ior;
    if (sineTransmitted >= 1.0f)
    {
        return 1.0f;
    }
    const float cosineTransmitted = std::sqrt(
        std::max(1.0f - sineTransmitted * sineTransmitted, 0.0f));
    const float parallel =
        (ior * incidentCosine - cosineTransmitted) /
        (ior * incidentCosine + cosineTransmitted);
    const float perpendicular =
        (incidentCosine - ior * cosineTransmitted) /
        (incidentCosine + ior * cosineTransmitted);
    return std::clamp(
        0.5f * (parallel * parallel + perpendicular * perpendicular),
        0.0f,
        1.0f);
}

HairRgb EvaluateBeerLambert(const HairRgb& absorption, float pathLength)
{
    HairRgb transmittance{};
    for (size_t channel = 0; channel < transmittance.size(); ++channel)
    {
        transmittance[channel] = std::exp(
            -std::max(absorption[channel], 0.0f) *
            std::max(pathLength, 0.0f));
    }
    return transmittance;
}

float EvaluatePathLength(
    HairPath path,
    float height,
    float thetaD,
    float ior,
    float fiberRadius)
{
    if (path == HairPath::R)
    {
        return 0.0f;
    }
    const float effectiveCosine = std::max(std::cos(thetaD), 1.0e-4f);
    const float effectiveIor = std::sqrt(
        std::max(
            ior * ior - std::sin(thetaD) * std::sin(thetaD),
            NumericalEpsilon)) /
        effectiveCosine;
    const float transmittedHeight = height / effectiveIor;
    const float chordCosine = std::sqrt(
        std::max(1.0f - transmittedHeight * transmittedHeight, 0.0f));
    const float baseChord = 2.0f * fiberRadius * chordCosine;
    return path == HairPath::TT
        ? baseChord
        : baseChord + 4.0f * fiberRadius * chordCosine;
}

HairPathResponse EvaluateHairPath(
    HairPath path,
    const HairAngles& angles,
    const HairReferenceParameters& parameters)
{
    HairPathResponse response;
    response.path = path;
    response.longitudinal = EvaluateLongitudinalLobe(
        path,
        angles.thetaH,
        parameters.cuticleTilt,
        parameters.longitudinalRoughness);

    const std::vector<HairAzimuthalRoot> roots = FindAzimuthalRoots(
        path,
        angles.thetaD,
        angles.deltaPhi,
        parameters.ior);
    for (const HairAzimuthalRoot& root : roots)
    {
        const float pathLength = EvaluatePathLength(
            path,
            root.height,
            angles.thetaD,
            parameters.ior,
            parameters.fiberRadius);
        const float surfaceFresnel = EvaluateDielectricFresnel(
            std::cos(angles.thetaD),
            parameters.ior);
        float interfaceWeight = surfaceFresnel;
        if (path == HairPath::TT)
        {
            interfaceWeight = (1.0f - surfaceFresnel) *
                (1.0f - surfaceFresnel);
        }
        else if (path == HairPath::TRT)
        {
            const float internalCosine = std::sqrt(
                std::max(
                    1.0f -
                        std::sin(angles.thetaD) *
                            std::sin(angles.thetaD) /
                            (parameters.ior * parameters.ior),
                    0.0f));
            const float internalFresnel = EvaluateDielectricFresnel(
                internalCosine,
                1.0f / parameters.ior);
            interfaceWeight = (1.0f - surfaceFresnel) *
                (1.0f - surfaceFresnel) * internalFresnel;
        }

        const HairRgb transmittance = EvaluateBeerLambert(
            parameters.absorption,
            pathLength);
        const float azimuthalWeight =
            1.0f / std::max(root.jacobian, 1.0e-4f);
        response.azimuthal += azimuthalWeight;
        response.jacobian += root.jacobian;
        response.pathLength += pathLength * azimuthalWeight;
        response.interfaceWeight += interfaceWeight * azimuthalWeight;
        for (size_t channel = 0; channel < response.transmittance.size(); ++channel)
        {
            response.transmittance[channel] +=
                (transmittance[channel] - 1.0f) * azimuthalWeight;
            response.contribution[channel] +=
                transmittance[channel] *
                interfaceWeight *
                response.longitudinal *
                azimuthalWeight;
        }
    }
    if (!roots.empty())
    {
        const float inverseRootCount = 1.0f /
            static_cast<float>(roots.size());
        response.azimuthal *= inverseRootCount;
        response.jacobian *= inverseRootCount;
        response.pathLength *= inverseRootCount;
        response.interfaceWeight *= inverseRootCount;
        for (size_t channel = 0; channel < response.transmittance.size(); ++channel)
        {
            response.transmittance[channel] = 1.0f +
                (response.transmittance[channel] - 1.0f) * inverseRootCount;
        }
    }
    return response;
}

HairScatteringResponse EvaluateHairScattering(
    const HairTangentFrame& frame,
    HairVec3 incidentDirection,
    HairVec3 outgoingDirection,
    const HairReferenceParameters& parameters)
{
    HairScatteringResponse response;
    response.angles = ComputeHairAngles(
        frame,
        incidentDirection,
        outgoingDirection);
    response.paths = {
        EvaluateHairPath(HairPath::R, response.angles, parameters),
        EvaluateHairPath(HairPath::TT, response.angles, parameters),
        EvaluateHairPath(HairPath::TRT, response.angles, parameters)};
    return response;
}

float EvaluateMultipleScatteringBudget(
    const HairReferenceParameters& parameters,
    float remainingSingleScatteringEnergy,
    float coverage) noexcept
{
    const float remainingEnergy = std::max(remainingSingleScatteringEnergy, 0.0f);
    const float validCoverage = std::clamp(coverage, 0.0f, 1.0f);
    return std::min(
        remainingEnergy * validCoverage,
        std::max(parameters.scatter, 0.0f) *
            std::max(parameters.specular, 0.0f));
}

} // namespace VL::Hair
