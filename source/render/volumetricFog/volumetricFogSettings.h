#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <stdexcept>
#include <string>

#include <Eigen/Dense>
#include <nlohmann/json.hpp>

namespace VL
{

// 与 shader/glsl/pass/volumetricFog.comp 的 VL_FOG_MAX_LAYERS、
// config/renderGraphConfig.json 的 volumetricFog.arrayLayers 必须一致。
inline constexpr uint32_t kMaxVolumetricFogLayers = 32;

// 场景 environment.fog 的作者参数。所有长度使用 cm（与引擎世界单位一致），
// 但密度/高度衰减按“每米”书写，避免资产里出现 0.0002 这种难读的数字；
// shader 端统一除以 100 转成每 cm。
struct FogSettings
{
    // 只有 enabled && density > 0 && intensity > 0 时才真正消耗 GPU。
    // 场景没有 fog 块时保持 false，旧场景渲染结果不变。
    bool enabled = false;
    // 消光系数（每米）。雾的透射率 T = exp(-density * path)。
    float density = 0.0f;
    // 高度雾衰减（每米）；0 表示均匀介质。
    float heightFalloff = 0.05f;
    // 密度参考高度（cm）；高于该高度密度按 exp(-falloff * (y - base)) 衰减。
    float baseHeight = 0.0f;
    // 散射反照率（线性 RGB）：决定雾把光散射成什么颜色。
    Eigen::Vector3f albedo = Eigen::Vector3f::Ones();
    // Henyey-Greenstein g，(-1, 1)；>0 前向散射，0 各向同性。
    float anisotropy = 0.5f;
    // 散射强度缩放；0 等于关闭。
    float intensity = 1.0f;
};

inline bool IsFogEnabled(const FogSettings& fog)
{
    return fog.enabled && fog.density > 0.0f && fog.intensity > 0.0f;
}

// 深度层使用指数分布：近处层密、远处层疏。near/far 是相机裁剪面（cm），
// layer 取 [0, layerCount-1]，返回该层的视图空间深度（沿相机 forward 的距离）。
inline float FogSliceViewDepthCm(
    float nearCm,
    float farCm,
    uint32_t layer,
    uint32_t layerCount)
{
    const float safeNear = std::max(nearCm, 1.0f);
    const float safeFar = std::max(farCm, safeNear + 1.0f);
    const float denominator =
        static_cast<float>(layerCount > 1 ? layerCount - 1 : 1);
    const float t = std::clamp(
        static_cast<float>(layer) / denominator, 0.0f, 1.0f);
    return safeNear * std::pow(safeFar / safeNear, t);
}

// FogSliceViewDepthCm 的逆映射：返回连续的层坐标 [0, layerCount-1]。
// 合成端用它找到相邻两层并做线性插值。
inline float FogSliceCoordinate(
    float viewDepthCm,
    float nearCm,
    float farCm,
    uint32_t layerCount)
{
    const float safeNear = std::max(nearCm, 1.0f);
    const float safeFar = std::max(farCm, safeNear + 1.0f);
    const float clampedDepth =
        std::clamp(viewDepthCm, safeNear, safeFar);
    const float denominator =
        static_cast<float>(layerCount > 1 ? layerCount - 1 : 1);
    return
        std::log(clampedDepth / safeNear) /
        std::log(safeFar / safeNear) *
        denominator;
}

// 高度雾密度倍率：exp(-falloff * (height - base))，并夹在 [0, 8]。
// 下限 0 避免雾密度出现负值；上限 8 防止相机低于 baseHeight 时密度爆炸。
inline float FogHeightDensityScale(
    float worldHeightCm,
    float baseHeightCm,
    float falloffPerMeter)
{
    if (falloffPerMeter <= 0.0f)
    {
        return 1.0f;
    }
    const float exponent =
        -(falloffPerMeter / 100.0f) *
        (worldHeightCm - baseHeightCm);
    return std::clamp(std::exp(exponent), 0.0f, 8.0f);
}

// Henyey-Greenstein 相位函数，归一化到 g=0 时为 1（各向同性）。
// 这样各向异性只改变方向分布，不会改变作者调好的整体散射亮度。
inline float HenyeyGreensteinPhase(
    float cosTheta,
    float anisotropy)
{
    const float g = std::clamp(anisotropy, -0.95f, 0.95f);
    const float clampedCos = std::clamp(cosTheta, -1.0f, 1.0f);
    const float denominator =
        1.0f + g * g - 2.0f * g * clampedCos;
    return
        (1.0f - g * g) /
        std::max(denominator * std::sqrt(std::max(denominator, 1.0e-6f)),
                 1.0e-6f);
}

namespace FogSettingsJson
{
inline float RequireFinite(
    const nlohmann::json& node,
    const char* field,
    float minValue,
    float maxValue)
{
    if (!node.contains(field) || !node.at(field).is_number())
    {
        throw std::invalid_argument(
            std::string("Fog requires finite number: ") + field);
    }
    const float value = node.at(field).get<float>();
    if (!std::isfinite(value) || value < minValue || value > maxValue)
    {
        throw std::invalid_argument(
            std::string("Fog value out of range: ") + field);
    }
    return value;
}

inline Eigen::Vector3f RequireColor(
    const nlohmann::json& node,
    const char* field)
{
    if (!node.contains(field) || !node.at(field).is_array() ||
        node.at(field).size() != 3)
    {
        throw std::invalid_argument(
            std::string("Fog requires vec3: ") + field);
    }
    Eigen::Vector3f color;
    for (int index = 0; index < 3; ++index)
    {
        const auto& component = node.at(field).at(index);
        if (!component.is_number())
        {
            throw std::invalid_argument(
                std::string("Fog color must be numeric: ") + field);
        }
        const float value = component.get<float>();
        if (!std::isfinite(value) || value < 0.0f || value > 16.0f)
        {
            throw std::invalid_argument(
                std::string("Fog color out of range: ") + field);
        }
        color[index] = value;
    }
    return color;
}
} // namespace FogSettingsJson

// 场景 JSON -> FogSettings。校验失败抛 std::invalid_argument，
// 场景校验器与 WorldBuilder 共用同一份规则，避免两条解析路径漂移。
inline FogSettings ParseFogSettingsJson(const nlohmann::json& node)
{
    if (!node.is_object())
    {
        throw std::invalid_argument("environment.fog must be object");
    }

    FogSettings fog;
    if (node.contains("enabled") && !node.at("enabled").is_boolean())
    {
        throw std::invalid_argument("Fog enabled must be boolean");
    }
    fog.enabled = node.contains("enabled")
        ? node.at("enabled").get<bool>()
        : true;
    fog.density = node.contains("density")
        ? FogSettingsJson::RequireFinite(node, "density", 0.0f, 10.0f)
        : 0.0f;
    fog.heightFalloff = node.contains("heightFalloff")
        ? FogSettingsJson::RequireFinite(
              node, "heightFalloff", 0.0f, 10.0f)
        : 0.05f;
    fog.baseHeight = node.contains("baseHeight")
        ? FogSettingsJson::RequireFinite(
              node, "baseHeight", -1.0e6f, 1.0e6f)
        : 0.0f;
    fog.albedo = node.contains("albedo")
        ? FogSettingsJson::RequireColor(node, "albedo")
        : Eigen::Vector3f::Ones();
    fog.anisotropy = node.contains("anisotropy")
        ? FogSettingsJson::RequireFinite(node, "anisotropy", -0.95f, 0.95f)
        : 0.5f;
    fog.intensity = node.contains("intensity")
        ? FogSettingsJson::RequireFinite(
              node, "intensity", 0.0f, 1000.0f)
        : 1.0f;
    return fog;
}

} // namespace VL
