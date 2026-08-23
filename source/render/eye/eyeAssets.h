#pragma once

#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

#include <nlohmann/json.hpp>

#include "render/eye/eyeLodContract.h"

namespace VL
{

// Eye profile 与 caustic atlas 的尺寸、ID 和 kernel 版本共同构成 GPU 资源合同。
// 任何影响采样坐标或数值的变更都必须提升版本，不能静默消费旧 atlas。
inline constexpr uint32_t EyeProfileSchemaVersion = 1;
inline constexpr uint32_t EyeProfileVersion = 1;
inline constexpr uint32_t EyeCausticLutVersion = 1;
inline constexpr uint32_t EyeCausticLutKernelVersion = 1;
inline constexpr uint32_t EyeCausticLutWidth = 64;
inline constexpr uint32_t EyeCausticLutHeight = 64;
inline constexpr uint32_t EyeCausticLutElevationSliceCount = 16;
inline constexpr uint32_t EyeCausticLutMaximumProfileId = 15;
inline constexpr uint32_t EyeCausticLutLayerCount =
    (EyeCausticLutMaximumProfileId + 1) * EyeCausticLutElevationSliceCount;
inline constexpr uint32_t EyeCausticLutChannelCount = 4;

struct EyeProfileAsset
{
    std::string name;
    std::string assetPath;
    uint32_t schemaVersion = EyeProfileSchemaVersion;
    uint32_t profileVersion = EyeProfileVersion;
    uint32_t profileId = 0;
    uint32_t kernelVersion = EyeCausticLutKernelVersion;
    uint32_t causticLutVersion = EyeCausticLutVersion;
    float ior = 1.376f;
    float eyeRadius = 0.012f;
    float corneaRadius = 0.0078f;
    float irisDistance = 0.003f;
    float irisRadius = 0.006f;
    float pupilRadiusMin = 0.0015f;
    float pupilRadiusMax = 0.004f;
    float pupilRadius = 0.002f;
    float limbusWidth = 0.0005f;
    float causticStrength = 0.15f;
    // 运行时 profile 的所有长度已转换为米；保留 authoring 单位用于诊断。
    std::string distanceUnit = "meter";
    float worldUnitScale = 1.0f;
    std::string unit = "meter";
    std::string sourceIdentity = "vulkanlearn.eye.compute.v1";
    EyeLodContract lodContract;
};

struct EyeCausticLutMetadata
{
    uint32_t schemaVersion = EyeProfileSchemaVersion;
    uint32_t lutVersion = EyeCausticLutVersion;
    uint32_t kernelVersion = EyeCausticLutKernelVersion;
    uint32_t width = EyeCausticLutWidth;
    uint32_t height = EyeCausticLutHeight;
    uint32_t elevationSlices = EyeCausticLutElevationSliceCount;
    uint32_t maximumProfileId = EyeCausticLutMaximumProfileId;
    uint32_t layers = EyeCausticLutLayerCount;
    uint32_t channels = EyeCausticLutChannelCount;
    std::string unit = "meter";
    std::string radialCoordinate = "unit-disk[-1,1]";
    std::string elevationCoordinate = "normalized-front-light[0,1]";
    std::string channelConvention = "R=gain,G=transmission,B=coverage,A=jacobian";
    std::string computeArtifactGenerationKey;
    std::string sourceDigest;
};

EyeProfileAsset ParseEyeProfileAsset(
    const nlohmann::json& json,
    std::string_view assetPath);

std::vector<EyeProfileAsset> LoadEyeProfileAssets(
    const std::filesystem::path& resourceRoot);

void ValidateEyeProfileAsset(
    const EyeProfileAsset& asset,
    std::string_view assetPath);

nlohmann::json SerializeEyeCausticLutMetadata(
    const EyeCausticLutMetadata& metadata);

// 与 eyeCausticLut.comp 共用的首版归一化函数。有效 unit disk 的面积平均值为 1。
float EvaluateEyeCausticGain(
    float radialSquared,
    float elevation,
    float strength) noexcept;

} // namespace VL
