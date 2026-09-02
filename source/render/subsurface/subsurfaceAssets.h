#pragma once

#include <array>
#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

#include <nlohmann/json.hpp>

namespace VL
{

// 这些尺寸和 ID 上限同时定义 CPU 资产校验、Compute SSBO 数组和 GLSL lookup 布局，
// 修改时必须同步提升 schema/kernel/LUT 版本并更新合同测试。
inline constexpr uint32_t SubsurfaceProfileSchemaVersion = 1;
inline constexpr uint32_t SubsurfaceProfileKernelVersion = 1;
inline constexpr uint32_t SubsurfaceProfileMaximumId = 255;
inline constexpr uint32_t SubsurfaceProfileKernelSampleCount = 13;
inline constexpr uint32_t SubsurfaceProfileTableWidth =
    SubsurfaceProfileKernelSampleCount + 1;
inline constexpr uint32_t SubsurfaceProfileTableHeight =
    SubsurfaceProfileMaximumId + 1;

inline constexpr uint32_t PreintegratedSkinSchemaVersion = 1;
inline constexpr uint32_t PreintegratedSkinLutVersion = 1;
inline constexpr uint32_t PreintegratedSkinMaximumId = 15;
inline constexpr uint32_t PreintegratedSkinLutWidth = 128;
inline constexpr uint32_t PreintegratedSkinLutTileHeight = 64;
inline constexpr uint32_t PreintegratedSkinLutTableHeight =
    (PreintegratedSkinMaximumId + 1) *
    PreintegratedSkinLutTileHeight;

using SubsurfaceRgb = std::array<float, 3>;

enum class SubsurfaceDistanceUnit
{
    Millimeter,
    Centimeter,
    Meter,
};

enum class PreintegratedSkinOutputMode
{
    ScatteringMultiplier,
    FinalDiffuseResponse,
};

// 从 Common/Profiles/Subsurface/*.json 解析出的 profile 作者参数，供资源 loader 校验、
// SSBO 打包和材质路径解析使用；不负责计算 kernel 或创建 GPU 资源。
struct SubsurfaceProfileAsset
{
    std::string name;
    std::string assetPath;
    uint32_t profileId = 0;
    SubsurfaceRgb meanFreePathColor{};
    float meanFreePathDistance = 0.0f;
    SubsurfaceDistanceUnit distanceUnit =
        SubsurfaceDistanceUnit::Meter;
    float worldUnitScale = 1.0f;
};

// 从 Common/Profiles/SkinLuts/*.json 解析出的 skin LUT 作者参数，供资源 loader 校验、
// SSBO 打包和材质路径解析使用；不负责计算 LUT texel 或上传纹理。
struct PreintegratedSkinLutAsset
{
    std::string name;
    std::string assetPath;
    uint32_t skinLutId = 0;
    uint32_t width = PreintegratedSkinLutWidth;
    uint32_t height = PreintegratedSkinLutTileHeight;
    float thicknessMax = 0.0f;
    SubsurfaceDistanceUnit thicknessUnit =
        SubsurfaceDistanceUnit::Meter;
    float worldUnitScale = 1.0f;
    PreintegratedSkinOutputMode outputMode =
        PreintegratedSkinOutputMode::FinalDiffuseResponse;
    SubsurfaceRgb scatterColor{};
    SubsurfaceRgb transmissionColor{};
    std::string sourceIdentity;
};

// 本文件只解析并验证作者数据，不计算 profile kernel 或 skin LUT texel。
SubsurfaceDistanceUnit ParseSubsurfaceDistanceUnit(
    std::string_view value);
std::string_view ToString(SubsurfaceDistanceUnit value);
float GetDistanceUnitWorldScale(SubsurfaceDistanceUnit value);

PreintegratedSkinOutputMode ParsePreintegratedSkinOutputMode(
    std::string_view value);
std::string_view ToString(PreintegratedSkinOutputMode value);

SubsurfaceProfileAsset ParseSubsurfaceProfileAsset(
    const nlohmann::json& json,
    std::string_view assetPath);
PreintegratedSkinLutAsset ParsePreintegratedSkinLutAsset(
    const nlohmann::json& json,
    std::string_view assetPath);

std::vector<SubsurfaceProfileAsset> LoadSubsurfaceProfileAssets(
    const std::filesystem::path& resourceRoot);
std::vector<PreintegratedSkinLutAsset> LoadPreintegratedSkinLutAssets(
    const std::filesystem::path& resourceRoot);

} // namespace VL
