#pragma once

#include <algorithm>
#include <cmath>
#include <stdexcept>
#include <string>

#include <Eigen/Dense>
#include <nlohmann/json.hpp>

namespace VL
{

// mesh 粒子的材质实例 v1 子集。这里只读取数值参数，不实例化完整
// Material/MaterialInstance：粒子管线是独立 descriptor layout，接不进
// M_*.json 的纹理/宏图。未识别参数保持忽略，缺失参数保持默认值。
struct ParticleMaterialParams
{
    Eigen::Vector4f baseColor = Eigen::Vector4f::Ones();
    Eigen::Vector3f emissive = Eigen::Vector3f::Zero();
    float emissiveStrength = 0.0f;
    float roughness = 0.5f;
    float metallic = 0.0f;
    bool valid = false;
};

inline float ReadParticleMaterialFloat(
    const nlohmann::json& value,
    const char* parameterName)
{
    if (!value.is_number() || !std::isfinite(value.get<float>()))
    {
        throw std::invalid_argument(
            std::string("Particle material parameter must be finite: ") +
            parameterName);
    }
    return value.get<float>();
}

inline Eigen::Vector3f ReadParticleMaterialColor(
    const nlohmann::json& value,
    const char* parameterName)
{
    if (!value.is_array() ||
        (value.size() != 3 && value.size() != 4))
    {
        throw std::invalid_argument(
            std::string(
                "Particle material color requires 3 or 4 components: ") +
            parameterName);
    }
    Eigen::Vector3f color;
    for (int channel = 0; channel < 3; ++channel)
    {
        color[channel] = ReadParticleMaterialFloat(
            value.at(channel), parameterName);
    }
    return color;
}

// 解析 MI_*.json。只接受 materialInstance 类型；parameters 必须是对象。
// 支持 u_baseColor/u_emissive/u_emissiveStrength/u_roughness/u_metallic。
inline ParticleMaterialParams ParseParticleMaterialInstanceJson(
    const nlohmann::json& json)
{
    if (!json.is_object() ||
        json.value("type", std::string()) != "materialInstance")
    {
        throw std::invalid_argument(
            "Particle material instance requires type=materialInstance");
    }

    ParticleMaterialParams params;
    const auto parameters = json.find("parameters");
    if (parameters == json.end())
    {
        params.valid = true;
        return params;
    }
    if (!parameters->is_object())
    {
        throw std::invalid_argument(
            "Particle material instance parameters must be an object");
    }

    for (auto parameter = parameters->begin();
         parameter != parameters->end(); ++parameter)
    {
        const std::string& name = parameter.key();
        if (name == "u_baseColor")
        {
            const Eigen::Vector3f color =
                ReadParticleMaterialColor(parameter.value(), "u_baseColor");
            params.baseColor = Eigen::Vector4f(
                color.x(), color.y(), color.z(), 1.0f);
            if (parameter.value().size() == 4)
            {
                params.baseColor.w() = ReadParticleMaterialFloat(
                    parameter.value().at(3), "u_baseColor.a");
            }
        }
        else if (name == "u_emissive" || name == "u_emissiveColor")
        {
            params.emissive = ReadParticleMaterialColor(
                parameter.value(), name.c_str());
        }
        else if (name == "u_emissiveStrength")
        {
            params.emissiveStrength = std::max(
                ReadParticleMaterialFloat(
                    parameter.value(), "u_emissiveStrength"),
                0.0f);
        }
        else if (name == "u_roughness")
        {
            params.roughness = std::clamp(
                ReadParticleMaterialFloat(
                    parameter.value(), "u_roughness"),
                0.0f,
                1.0f);
        }
        else if (name == "u_metallic")
        {
            params.metallic = std::clamp(
                ReadParticleMaterialFloat(
                    parameter.value(), "u_metallic"),
                0.0f,
                1.0f);
        }
    }
    params.valid = true;
    return params;
}

} // namespace VL
