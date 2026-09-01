#pragma once

#include <array>
#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>

#include <nlohmann/json.hpp>

namespace VL
{

// 这些常量同时约束 metadata、Compute atlas 和 GLSL 采样坐标；修改任一项都必须
// 提升版本并同步更新 shader/cache 合同，避免旧 LUT 被新 evaluator 静默消费。
inline constexpr uint32_t HairAzimuthalLutSchemaVersion = 1;
inline constexpr uint32_t HairAzimuthalLutVersion = 2;
inline constexpr uint32_t HairAzimuthalLutKernelVersion = 2;
inline constexpr uint32_t HairAzimuthalLutWidth = 128;
inline constexpr uint32_t HairAzimuthalLutHeight = 512;
inline constexpr uint32_t HairAzimuthalLutLayerCount = 3;
inline constexpr uint32_t HairAzimuthalLutChannelCount = 4;
inline constexpr uint32_t HairAzimuthalLutRoughnessSliceCount = 8;
inline constexpr uint32_t HairAzimuthalLutThetaDSampleCount = 64;
inline constexpr float HairAzimuthalLutDefaultIor = 1.55f;
inline constexpr float HairAzimuthalLutDefaultFiberRadius = 0.00005f;

inline constexpr std::array<const char*, HairAzimuthalLutLayerCount>
    HairAzimuthalLutPathConventions = {"R", "TT", "TRT"};

struct HairAzimuthalLutMetadata
{
    uint32_t schemaVersion = HairAzimuthalLutSchemaVersion;
    uint32_t lutVersion = HairAzimuthalLutVersion;
    uint32_t kernelVersion = HairAzimuthalLutKernelVersion;
    uint32_t width = HairAzimuthalLutWidth;
    uint32_t height = HairAzimuthalLutHeight;
    uint32_t layers = HairAzimuthalLutLayerCount;
    uint32_t channels = HairAzimuthalLutChannelCount;
    uint32_t roughnessSlices = HairAzimuthalLutRoughnessSliceCount;
    uint32_t thetaDSamples = HairAzimuthalLutThetaDSampleCount;
    float ior = HairAzimuthalLutDefaultIor;
    float fiberRadius = HairAzimuthalLutDefaultFiberRadius;
    std::string unit = "meter";
    std::string thetaDCoordinate = "normalized[-pi/2,pi/2]";
    std::string deltaPhiCoordinate = "normalized[-pi,pi]";
    std::string roughnessMapping = "slice-linear-8";
    std::string wrap = "deltaPhi-repeat-thetaD-clamp";
    std::string pathConvention = "layer0=R,layer1=TT,layer2=TRT";
    std::string sourceIdentity;
};

struct HairAzimuthalLutAsset
{
    std::string assetPath;
    std::string metadataPath;
    HairAzimuthalLutMetadata metadata;
};

// 只解析并验证作者 metadata；不会在 CPU 上生成 LUT texel。
HairAzimuthalLutMetadata ParseHairAzimuthalLutMetadata(
    const nlohmann::json& json,
    std::string_view assetPath);

HairAzimuthalLutAsset LoadHairAzimuthalLutAsset(
    const std::filesystem::path& resourceRoot);

void ValidateHairAzimuthalLutMetadata(
    const HairAzimuthalLutMetadata& metadata,
    std::string_view assetPath);

nlohmann::json SerializeHairAzimuthalLutMetadata(
    const HairAzimuthalLutMetadata& metadata);

} // namespace VL
