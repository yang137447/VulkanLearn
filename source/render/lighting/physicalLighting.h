#pragma once

#include <cmath>
#include <limits>
#include <stdexcept>
#include <string>
#include <nlohmann/json.hpp>

namespace VL
{
// 作者使用厘米和显式光度单位；加载边界换算为 lux/cd/cd-m^-2。
// 渲染端只消费规范量，不再猜测缺失单位，也不把 RGB 光度值冒充光谱瓦特。
inline constexpr double WorldUnitsPerMeter = 100.0;
inline constexpr double LightingPi = 3.14159265358979323846;

inline float CandelaToCentimeterDistanceIntensity(float candela)
{
    // 仅在 GPU 上传边界吸收米到厘米的平方因子；World/离线输入仍保持 cd。
    // I / r_m^2 = (I * 100^2) / r_cm^2，避免逐像素换算距离单位。
    return candela * static_cast<float>(WorldUnitsPerMeter * WorldUnitsPerMeter);
}

inline void ValidateWorldUnit(const nlohmann::json& scene)
{
    if (!scene.contains("worldUnit") || scene.at("worldUnit") != "cm")
        throw std::invalid_argument("Scene requires worldUnit=\"cm\"; other units are unsupported");
}

inline double ReadPhysicalNumber(const nlohmann::json& node, const char* field,
                                 double minimum, double maximum)
{
    if (!node.contains(field) || !node.at(field).is_number())
        throw std::invalid_argument(std::string("Missing physical parameter: ") + field);
    const double value = node.at(field).get<double>();
    if (!std::isfinite(value) || value < minimum || value > maximum)
        throw std::invalid_argument(std::string("Invalid physical parameter: ") + field);
    return value;
}

inline double SpotEffectiveSolidAngle(double innerRadians, double outerRadians)
{
    // 必须与 shader 的 smoothstep 角度过渡保持同一积分，否则 lm 输入
    // 在内外锥变化时会悄悄改变总能量。中点积分足够稳定，且只在加载边界执行。
    constexpr int sampleCount = 256;
    const double range = outerRadians - innerRadians;
    double integral = 0.0;
    for (int sample = 0; sample < sampleCount; ++sample)
    {
        const double t = (static_cast<double>(sample) + 0.5) /
            static_cast<double>(sampleCount);
        const double angle = innerRadians + range * t;
        const double smooth = t * t * (3.0 - 2.0 * t);
        integral += (1.0 - smooth) * std::sin(angle);
    }
    return 2.0 * LightingPi * (
        1.0 - std::cos(innerRadians) +
        range * integral / static_cast<double>(sampleCount));
}

inline float ReadCanonicalLightIntensity(const nlohmann::json& light)
{
    const auto type = light.at("type").get<std::string>();
    if (!light.contains("intensityUnit") || !light.at("intensityUnit").is_string())
        throw std::invalid_argument("Light requires intensityUnit; Unitless is unsupported");
    const auto unit = light.at("intensityUnit").get<std::string>();
    double value = ReadPhysicalNumber(light, "intensity", 0.0, 1.0e12);
    const auto& color = light.at("color");
    if (!color.is_array() || color.size() != 3)
        throw std::invalid_argument("Light color must be a linear RGB vec3");
    for (const auto& channel : color)
        if (!channel.is_number() || !std::isfinite(channel.get<double>()) ||
            channel.get<double>() < 0.0 || channel.get<double>() > 1.0)
            throw std::invalid_argument("Light color must be finite linear RGB in [0,1]");

    if (type == "directionalLight")
    {
        if (unit != "lux")
            throw std::invalid_argument("Directional light requires lux");
    }
    else if (type == "pointLight" || type == "spotLight" || type == "squareAreaLight")
    {
        if (unit != "cd" && unit != "lm")
            throw std::invalid_argument("Local light requires cd or lm");
        double solidAngle = 4.0 * LightingPi;
        if (type == "spotLight")
        {
            const double outer = ReadPhysicalNumber(light, "cone_angle_outer", 0.1, 89.0);
            const double inner = ReadPhysicalNumber(light, "cone_angle_inner", 0.0, 89.0);
            if (inner >= outer)
                throw std::invalid_argument("Spot requires 0 <= inner < outer < 90 degrees (half angles)");
            solidAngle = SpotEffectiveSolidAngle(
                inner * LightingPi / 180.0, outer * LightingPi / 180.0);
        }
        if (type == "squareAreaLight")
        {
            const double sideMeters =
                ReadPhysicalNumber(light, "size", 0.001, 1.0e8) / WorldUnitsPerMeter;
            // 单面 Lambert 发射器：Phi = pi*A*L；cd 表示正面法线方向的 I=A*L。
            value /= sideMeters * sideMeters;
            solidAngle = LightingPi;
        }
        if (unit == "lm") value /= solidAngle;
    }
    else
        throw std::invalid_argument("Unsupported physical light type: " + type);
    if (!std::isfinite(value) || value > std::numeric_limits<float>::max())
        throw std::invalid_argument("Converted light intensity is not representable");
    return static_cast<float>(value);
}
}
