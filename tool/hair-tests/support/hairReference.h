#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <vector>

#include "render/hair/hairConventions.h"

namespace VL::Hair
{

// Hair reference 只负责可重复的纤维数学与路径账本，不依赖 Vulkan、Renderer 或材质缓存。
// 生产 shader/LUT 可以复用同一套命名与角度约定，但不能把本模块当作 CPU 预计算器。

inline constexpr uint32_t HairReferenceSchemaVersion = 1;
inline constexpr uint32_t HairReferenceConventionVersion = 1;

struct HairVec3
{
    float x = 0.0f;
    float y = 0.0f;
    float z = 0.0f;
};

using HairRgb = std::array<float, 3>;

enum class HairPath : uint32_t
{
    R = 0,
    TT = 1,
    TRT = 2,
};

const char* ToString(HairPath path) noexcept;

struct HairTangentFrame
{
    HairVec3 normal;
    HairVec3 bitangent;
    HairVec3 tangent;
    float handedness = 1.0f;
};

struct HairAngles
{
    float thetaI = 0.0f;
    float thetaO = 0.0f;
    float thetaH = 0.0f;
    float thetaD = 0.0f;
    float phiI = 0.0f;
    float phiO = 0.0f;
    float deltaPhi = 0.0f;
};

struct HairReferenceParameters
{
    float ior = 1.55f;
    float fiberRadius = 0.00005f;
    HairRgb absorption = {0.45f, 0.18f, 0.08f};
    float cuticleTilt = 0.0f;
    float longitudinalRoughness = 0.22f;
    float azimuthalRoughness = 0.25f;
    float specular = 0.5f;
    float scatter = 0.0f;
    float backlit = 0.0f;
};

struct HairAzimuthalRoot
{
    float height = 0.0f;
    float jacobian = 0.0f;
};

struct HairPathResponse
{
    HairPath path = HairPath::R;
    float longitudinal = 0.0f;
    float azimuthal = 0.0f;
    float jacobian = 0.0f;
    float pathLength = 0.0f;
    float interfaceWeight = 0.0f;
    HairRgb transmittance = {1.0f, 1.0f, 1.0f};
    HairRgb contribution = {0.0f, 0.0f, 0.0f};
};

struct HairScatteringResponse
{
    HairAngles angles;
    std::array<HairPathResponse, 3> paths;
};

HairVec3 Add(HairVec3 left, HairVec3 right) noexcept;
HairVec3 Subtract(HairVec3 left, HairVec3 right) noexcept;
HairVec3 Multiply(HairVec3 value, float scalar) noexcept;
float Dot(HairVec3 left, HairVec3 right) noexcept;
HairVec3 Cross(HairVec3 left, HairVec3 right) noexcept;
float Length(HairVec3 value) noexcept;
HairVec3 Normalize(HairVec3 value);

HairTangentFrame BuildTangentFrame(
    HairVec3 worldNormal,
    HairVec3 worldTangent,
    float handedness);

HairAngles ComputeHairAngles(
    const HairTangentFrame& frame,
    HairVec3 incidentDirection,
    HairVec3 outgoingDirection);

float WrapAngle(float angle) noexcept;
float ComputeAzimuthalPathAngle(
    HairPath path,
    float height,
    float thetaD,
    float ior);

std::vector<HairAzimuthalRoot> FindAzimuthalRoots(
    HairPath path,
    float thetaD,
    float deltaPhi,
    float ior,
    size_t sampleCount = 257);

float EvaluateLongitudinalLobe(
    HairPath path,
    float thetaH,
    float cuticleTilt,
    float roughness);

float EvaluateDielectricFresnel(float cosineIncident, float ior);
HairRgb EvaluateBeerLambert(const HairRgb& absorption, float pathLength);
float EvaluatePathLength(
    HairPath path,
    float height,
    float thetaD,
    float ior,
    float fiberRadius);

HairPathResponse EvaluateHairPath(
    HairPath path,
    const HairAngles& angles,
    const HairReferenceParameters& parameters);

HairScatteringResponse EvaluateHairScattering(
    const HairTangentFrame& frame,
    HairVec3 incidentDirection,
    HairVec3 outgoingDirection,
    const HairReferenceParameters& parameters);

float EvaluateMultipleScatteringBudget(
    const HairReferenceParameters& parameters,
    float remainingSingleScatteringEnergy,
    float coverage) noexcept;

} // namespace VL::Hair
