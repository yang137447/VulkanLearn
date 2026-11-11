#include "particles/particleAssetInternal.h"

#include <cmath>
#include <limits>
#include <stdexcept>
#include <string>

namespace VL
{
namespace ParticleAssetDetail
{
Eigen::Vector3f ReadVec3(
    const nlohmann::json& object,
    const char* field,
    const Eigen::Vector3f& fallback)
{
    if (!object.contains(field))
    {
        return fallback;
    }
    const nlohmann::json& value = object.at(field);
    if (!value.is_array() || value.size() != 3)
    {
        throw std::invalid_argument(
            std::string("Particle vector requires three components: ") +
            field);
    }
    Eigen::Vector3f result;
    for (int axis = 0; axis < 3; ++axis)
    {
        const nlohmann::json& component = value.at(axis);
        if (!component.is_number() ||
            !std::isfinite(component.get<float>()))
        {
            throw std::invalid_argument(
                std::string("Particle vector is not finite: ") + field);
        }
        result[axis] = component.get<float>();
    }
    return result;
}

Eigen::Vector2f ReadVec2(
    const nlohmann::json& object,
    const char* field,
    const Eigen::Vector2f& fallback)
{
    if (!object.contains(field))
    {
        return fallback;
    }
    const nlohmann::json& value = object.at(field);
    if (!value.is_array() || value.size() != 2)
    {
        throw std::invalid_argument(
            std::string("Particle vector requires two components: ") +
            field);
    }
    Eigen::Vector2f result;
    for (int axis = 0; axis < 2; ++axis)
    {
        const nlohmann::json& component = value.at(axis);
        if (!component.is_number() ||
            !std::isfinite(component.get<float>()))
        {
            throw std::invalid_argument(
                std::string("Particle vector is not finite: ") + field);
        }
        result[axis] = component.get<float>();
    }
    return result;
}

Eigen::Vector2f ReadVec2Tangent(
    const nlohmann::json& object,
    const char* field)
{
    if (!object.contains(field))
    {
        return Eigen::Vector2f::Zero();
    }
    const nlohmann::json& value = object.at(field);
    if (!value.is_array() || value.size() != 2)
    {
        throw std::invalid_argument(
            std::string("Particle curve tangent requires two "
                "components: ") + field);
    }
    Eigen::Vector2f result = Eigen::Vector2f::Zero();
    for (int axis = 0; axis < 2; ++axis)
    {
        const nlohmann::json& component = value.at(axis);
        if (!component.is_number() ||
            !std::isfinite(component.get<float>()))
        {
            throw std::invalid_argument(
                std::string("Particle curve tangent is not finite: ") +
                field);
        }
        result[axis] = component.get<float>();
    }
    return result;
}

Eigen::Vector4f ReadVec4(
    const nlohmann::json& object,
    const char* field,
    const Eigen::Vector4f& fallback)
{
    if (!object.contains(field))
    {
        return fallback;
    }
    const nlohmann::json& value = object.at(field);
    if (!value.is_array() || value.size() != 4)
    {
        throw std::invalid_argument(
            std::string("Particle color requires four components: ") +
            field);
    }
    Eigen::Vector4f result;
    for (int channel = 0; channel < 4; ++channel)
    {
        const nlohmann::json& component = value.at(channel);
        if (!component.is_number() ||
            !std::isfinite(component.get<float>()))
        {
            throw std::invalid_argument(
                std::string("Particle color is not finite: ") + field);
        }
        result[channel] = component.get<float>();
    }
    return result;
}

// 颜色切线允许省略 alpha（默认 0，而不是颜色 key 的 1），因为 alpha 的
// 导数在缺省时应当是“不变”而不是“不透明”。
Eigen::Vector4f ReadVec4Tangent(
    const nlohmann::json& object,
    const char* field)
{
    if (!object.contains(field))
    {
        return Eigen::Vector4f::Zero();
    }
    const nlohmann::json& value = object.at(field);
    if (!value.is_array() ||
        (value.size() != 3 && value.size() != 4))
    {
        throw std::invalid_argument(
            std::string("Particle curve tangent requires three or four "
                "components: ") + field);
    }
    Eigen::Vector4f result = Eigen::Vector4f::Zero();
    for (size_t channel = 0; channel < value.size(); ++channel)
    {
        const nlohmann::json& component = value.at(channel);
        if (!component.is_number() ||
            !std::isfinite(component.get<float>()))
        {
            throw std::invalid_argument(
                std::string("Particle curve tangent is not finite: ") +
                field);
        }
        result[channel] = component.get<float>();
    }
    return result;
}

float ReadFiniteFloat(
    const nlohmann::json& object,
    const char* field,
    float fallback)
{
    if (!object.contains(field))
    {
        return fallback;
    }
    const nlohmann::json& value = object.at(field);
    if (!value.is_number() ||
        !std::isfinite(value.get<float>()))
    {
        throw std::invalid_argument(
            std::string("Particle field is not finite: ") + field);
    }
    return value.get<float>();
}

float ReadFiniteNumber(
    const nlohmann::json& value,
    const char* field)
{
    if (!value.is_number())
    {
        throw std::invalid_argument(
            std::string("Particle field must be a number: ") + field);
    }
    const double number = value.get<double>();
    if (!std::isfinite(number))
    {
        throw std::invalid_argument(
            std::string("Particle field is not finite: ") + field);
    }
    return static_cast<float>(number);
}

// 计数字段必须是整数：直接 get<uint32_t> 会把 512.9 静默截断成 512。
uint32_t ReadCountField(
    const nlohmann::json& object,
    const char* field,
    uint32_t fallback)
{
    if (!object.contains(field))
    {
        return fallback;
    }
    const nlohmann::json& value = object.at(field);
    if (!value.is_number())
    {
        throw std::invalid_argument(
            std::string("Particle count must be a number: ") + field);
    }
    const double number = value.get<double>();
    if (!std::isfinite(number) || number < 0.0 ||
        number > static_cast<double>(
            std::numeric_limits<uint32_t>::max()) ||
        std::floor(number) != number)
    {
        throw std::invalid_argument(
            std::string(
                "Particle count must be a non-negative integer: ") +
            field);
    }
    return static_cast<uint32_t>(number);
}

// 排序层是整数：editor 协议可能把 JSON Number 以 double 送达，必须显式
// 拒绝小数，不能靠 get<int> 静默截断；上下界在解析期就固定，避免运行时
// 出现无法解释的超大排序键。
int ReadIntField(
    const nlohmann::json& object,
    const char* field,
    int fallback,
    int minimum,
    int maximum)
{
    if (!object.contains(field))
    {
        return fallback;
    }
    const nlohmann::json& value = object.at(field);
    if (!value.is_number())
    {
        throw std::invalid_argument(
            std::string("Particle field must be a number: ") + field);
    }
    const double number = value.get<double>();
    if (!std::isfinite(number) || std::floor(number) != number)
    {
        throw std::invalid_argument(
            std::string("Particle field must be an integer: ") + field);
    }
    if (number < static_cast<double>(minimum) ||
        number > static_cast<double>(maximum))
    {
        throw std::invalid_argument(
            std::string("Particle field must be in [") +
            std::to_string(minimum) + ", " +
            std::to_string(maximum) + "]: " + field);
    }
    return static_cast<int>(number);
}

// flipbook 帧号必须是整数；JSON Number 在编辑器里可能以 double 到达，
// 因此先检查有限性、整数性和非负范围，避免直接 get<uint32_t> 静默截断。
uint32_t ReadFrameIndex(
    const nlohmann::json& object,
    const char* field,
    uint32_t fallback)
{
    if (!object.contains(field))
    {
        return fallback;
    }
    const nlohmann::json& value = object.at(field);
    if (!value.is_number())
    {
        throw std::invalid_argument(
            std::string("Particle flipbook frame must be a number: ") +
            field);
    }
    const double number = value.get<double>();
    if (!std::isfinite(number) || number < 0.0 ||
        number > static_cast<double>(
            std::numeric_limits<uint32_t>::max()) ||
        std::floor(number) != number)
    {
        throw std::invalid_argument(
            std::string("Particle flipbook frame must be a non-negative integer: ") +
            field);
    }
    return static_cast<uint32_t>(number);
}

Eigen::Vector3f NormalizeParticleCollisionPlaneNormal(
    const Eigen::Vector3f& normal,
    const char* field)
{
    const float length = normal.norm();
    if (!normal.allFinite() || !std::isfinite(length) ||
        length <= 1.0e-6f)
    {
        throw std::invalid_argument(
            std::string(
                "Particle collision plane normal must be finite and non-zero: ") +
            field);
    }
    return normal / length;
}

// v2 用户参数绑定：任何以 '$' 开头的字符串在展平后的 emitter JSON 中
// 替换为同名数值参数。这样模块字段（spawnRate、gravity、radius 等）
// 与普通数值走同一条解析/校验路径，不存在“只解析不生效”的旁路。
void SubstituteParticleUserParameters(
    nlohmann::json& value,
    const std::unordered_map<std::string, float>& parameters,
    const std::string& fieldPath,
    std::unordered_set<std::string>* references)
{
    if (value.is_object())
    {
        for (auto child = value.begin(); child != value.end(); ++child)
        {
            SubstituteParticleUserParameters(
                child.value(),
                parameters,
                fieldPath.empty()
                    ? child.key()
                    : fieldPath + "." + child.key(),
                references);
        }
        return;
    }
    if (value.is_array())
    {
        for (size_t index = 0; index < value.size(); ++index)
        {
            SubstituteParticleUserParameters(
                value.at(index),
                parameters,
                fieldPath + "[" + std::to_string(index) + "]",
                references);
        }
        return;
    }
    if (!value.is_string())
    {
        return;
    }
    const std::string reference = value.get<std::string>();
    if (reference.empty() || reference.front() != '$')
    {
        return;
    }
    const std::string name = reference.substr(1);
    if (references != nullptr)
    {
        references->insert(name);
    }
    const auto parameter = parameters.find(name);
    if (parameter == parameters.end())
    {
        throw std::invalid_argument(
            "Particle user parameter reference is unknown: " + name +
            " at " + fieldPath);
    }
    value = parameter->second;
}

} // namespace ParticleAssetDetail
} // namespace VL
