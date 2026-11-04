#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <stdexcept>
#include <string>

#include <Eigen/Dense>
#include <nlohmann/json.hpp>

#include "particles/particleMesh.h"

namespace VL
{

// mesh 粒子独占的 descriptor set 1 合同。billboard/ribbon/beam 不含该 set。
inline constexpr uint32_t kParticleMaterialSetIndex = 1;
inline constexpr uint32_t kParticleMaterialUniformBinding = 0;
inline constexpr uint32_t kParticleMaterialAlbedoBinding = 1;
inline constexpr uint32_t kParticleMaterialNormalBinding = 2;
inline constexpr uint32_t kParticleMaterialOrmBinding = 3;
inline constexpr uint32_t kParticleMaterialEmissiveBinding = 4;
inline constexpr uint32_t kParticleMaterialTextureCount = 4;

// mesh 粒子的材质实例 v1 子集。这里不实例化完整 Material/MaterialInstance：
// 粒子管线是独立 descriptor layout，但允许 MI 复用 M_pbr 的贴图槽位命名
// （albedoMap/normalMap/pbrParamMap/emissionMap），由 ParticleRenderer
// 直接把 T_*.json 绑定到 mesh 专用 set 1。未识别参数保持忽略，
// 缺失参数保持默认值。
struct ParticleMaterialParams
{
    Eigen::Vector4f baseColor = Eigen::Vector4f::Ones();
    Eigen::Vector3f emissive = Eigen::Vector3f::Zero();
    float emissiveStrength = 0.0f;
    float roughness = 0.5f;
    float metallic = 0.0f;
    float ambientOcclusion = 1.0f;
    // 0 表示关闭 alpha test；M_pbr 的 u_alphaClipThreshold 同名透传。
    float alphaClipThreshold = 0.0f;
    // 贴图法线强度：0 保留几何法线，1 使用完整切线空间法线。
    float normalStrength = 1.0f;
    // 资源根相对路径；空字符串表示该槽位回退默认纹理。
    std::string albedoMap;
    std::string normalMap;
    std::string ormMap;
    std::string emissiveMap;
    // UV = texCoord * uvScale + uvOffset。MI 顶层 uvScale/uvOffset 与
    // parameters 的 u_uvScale/u_uvOffset 等价，后者优先。
    Eigen::Vector2f uvScale = Eigen::Vector2f::Ones();
    Eigen::Vector2f uvOffset = Eigen::Vector2f::Zero();
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

inline Eigen::Vector2f ReadParticleMaterialVector2(
    const nlohmann::json& value,
    const char* parameterName)
{
    if (!value.is_array() || value.size() != 2)
    {
        throw std::invalid_argument(
            std::string(
                "Particle material vector requires 2 components: ") +
            parameterName);
    }
    return Eigen::Vector2f(
        ReadParticleMaterialFloat(value.at(0), parameterName),
        ReadParticleMaterialFloat(value.at(1), parameterName));
}

inline std::string ReadParticleMaterialTexturePath(
    const nlohmann::json& value,
    const char* slotName)
{
    if (value.is_null())
    {
        return {};
    }
    if (!value.is_string())
    {
        throw std::invalid_argument(
            std::string(
                "Particle material texture path must be a string: ") +
            slotName);
    }
    const std::string path = value.get<std::string>();
    if (path.empty())
    {
        return {};
    }
    if (!IsValidParticleMeshRelativePath(path))
    {
        throw std::invalid_argument(
            std::string(
                "Particle material texture must be a resource-root-relative "
                ".json path: ") +
            slotName + " = " + path);
    }
    return path;
}

// 解析 MI_*.json。只接受 materialInstance 类型；parameters 必须是对象。
// 数值参数支持 u_baseColor/u_tintColor/u_emissive/u_emissiveStrength/
// u_roughness/u_metallic/u_pbrFactors/u_ambientOcclusion/u_alphaClipThreshold/
// u_normalStrength/u_uvScale/u_uvOffset；贴图槽位支持
// albedoMap/normalMap/ormMap(pbrParamMap)/emissiveMap(emissionMap)。
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
    bool emissiveSpecified = false;
    bool emissiveStrengthSpecified = false;
    if (json.contains("uvScale"))
    {
        params.uvScale = ReadParticleMaterialVector2(
            json.at("uvScale"), "uvScale");
    }
    if (json.contains("uvOffset"))
    {
        params.uvOffset = ReadParticleMaterialVector2(
            json.at("uvOffset"), "uvOffset");
    }
    const auto textures = json.find("textures");
    if (textures != json.end())
    {
        if (!textures->is_object())
        {
            throw std::invalid_argument(
                "Particle material instance textures must be an object");
        }
        for (auto texture = textures->begin();
             texture != textures->end(); ++texture)
        {
            const std::string& slot = texture.key();
            if (slot == "albedoMap")
            {
                params.albedoMap = ReadParticleMaterialTexturePath(
                    texture.value(), "albedoMap");
            }
            else if (slot == "normalMap")
            {
                params.normalMap = ReadParticleMaterialTexturePath(
                    texture.value(), "normalMap");
            }
            else if (slot == "ormMap" || slot == "pbrParamMap")
            {
                params.ormMap = ReadParticleMaterialTexturePath(
                    texture.value(), slot.c_str());
            }
            else if (slot == "emissiveMap" || slot == "emissionMap")
            {
                params.emissiveMap = ReadParticleMaterialTexturePath(
                    texture.value(), slot.c_str());
            }
        }
    }

    const auto parameters = json.find("parameters");
    if (parameters == json.end())
    {
        if (!params.emissiveMap.empty())
        {
            if (!emissiveSpecified)
            {
                params.emissive = Eigen::Vector3f::Ones();
            }
            if (!emissiveStrengthSpecified)
            {
                params.emissiveStrength = 1.0f;
            }
        }
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
        else if (name == "u_tintColor")
        {
            const Eigen::Vector3f color =
                ReadParticleMaterialColor(parameter.value(), "u_tintColor");
            params.baseColor = Eigen::Vector4f(
                color.x(), color.y(), color.z(), 1.0f);
            if (parameter.value().size() == 4)
            {
                params.baseColor.w() = ReadParticleMaterialFloat(
                    parameter.value().at(3), "u_tintColor.a");
            }
        }
        else if (name == "u_emissive" || name == "u_emissiveColor")
        {
            params.emissive = ReadParticleMaterialColor(
                parameter.value(), name.c_str());
            emissiveSpecified = true;
        }
        else if (name == "u_emissiveStrength")
        {
            emissiveStrengthSpecified = true;
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
        else if (name == "u_pbrFactors")
        {
            if (!parameter.value().is_array() ||
                parameter.value().size() != 4)
            {
                throw std::invalid_argument(
                    "Particle material u_pbrFactors requires 4 components");
            }
            params.roughness = std::clamp(
                ReadParticleMaterialFloat(
                    parameter.value().at(0), "u_pbrFactors.x"),
                0.0f,
                1.0f);
            params.metallic = std::clamp(
                ReadParticleMaterialFloat(
                    parameter.value().at(1), "u_pbrFactors.y"),
                0.0f,
                1.0f);
            params.ambientOcclusion = std::clamp(
                ReadParticleMaterialFloat(
                    parameter.value().at(2), "u_pbrFactors.z"),
                0.0f,
                1.0f);
        }
        else if (name == "u_ambientOcclusion")
        {
            params.ambientOcclusion = std::clamp(
                ReadParticleMaterialFloat(
                    parameter.value(), "u_ambientOcclusion"),
                0.0f,
                1.0f);
        }
        else if (name == "u_alphaClipThreshold")
        {
            params.alphaClipThreshold = std::clamp(
                ReadParticleMaterialFloat(
                    parameter.value(), "u_alphaClipThreshold"),
                0.0f,
                1.0f);
        }
        else if (name == "u_normalStrength")
        {
            params.normalStrength = std::clamp(
                ReadParticleMaterialFloat(
                    parameter.value(), "u_normalStrength"),
                0.0f,
                1.0f);
        }
        else if (name == "u_uvScale")
        {
            params.uvScale = ReadParticleMaterialVector2(
                parameter.value(), "u_uvScale");
        }
        else if (name == "u_uvOffset")
        {
            params.uvOffset = ReadParticleMaterialVector2(
                parameter.value(), "u_uvOffset");
        }
    }
    // 贴图自发光使用贴图颜色作为默认 tint；作者显式给 u_emissive 时保留
    // 其颜色，避免 MI 只写 emissionMap + strength 时整段自发光为黑色。
    if (!params.emissiveMap.empty())
    {
        if (!emissiveSpecified)
        {
            params.emissive = Eigen::Vector3f::Ones();
        }
        if (!emissiveStrengthSpecified)
        {
            params.emissiveStrength = 1.0f;
        }
    }
    params.valid = true;
    return params;
}

} // namespace VL
