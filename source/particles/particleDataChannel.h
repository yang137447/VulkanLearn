#pragma once

#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include <nlohmann/json.hpp>

namespace VL
{

// World 级 Particle Data Channel 的类型。值统一存放在四个 float 组件中，
// 未使用的组件保持 0；这样 Console/C#/JSON 三条入口共享同一份校验和
// 提取逻辑，不会出现“类型只存在于某一条桥”的旁路。
enum class ParticleDataChannelType : uint8_t
{
    Float = 0,
    Vec2 = 1,
    Vec3 = 2,
    Vec4 = 3,
    Int = 4,
    Bool = 5,
    Quat = 6
};

constexpr size_t kParticleDataChannelMaxComponents = 4;
constexpr float kParticleDataChannelComponentLimit = 1.0e6f;

inline const char* ParticleDataChannelTypeName(
    ParticleDataChannelType type)
{
    switch (type)
    {
    case ParticleDataChannelType::Float: return "float";
    case ParticleDataChannelType::Vec2: return "vec2";
    case ParticleDataChannelType::Vec3: return "vec3";
    case ParticleDataChannelType::Vec4: return "vec4";
    case ParticleDataChannelType::Int: return "int";
    case ParticleDataChannelType::Bool: return "bool";
    case ParticleDataChannelType::Quat: return "quat";
    }
    return "float";
}

inline size_t ParticleDataChannelComponentCount(
    ParticleDataChannelType type)
{
    switch (type)
    {
    case ParticleDataChannelType::Vec2: return 2;
    case ParticleDataChannelType::Vec3: return 3;
    case ParticleDataChannelType::Vec4: return 4;
    case ParticleDataChannelType::Quat: return 4;
    case ParticleDataChannelType::Float:
    case ParticleDataChannelType::Int:
    case ParticleDataChannelType::Bool:
        return 1;
    }
    return 1;
}

inline bool IsParticleDataChannelScalar(ParticleDataChannelType type)
{
    return type == ParticleDataChannelType::Float ||
        type == ParticleDataChannelType::Int ||
        type == ParticleDataChannelType::Bool;
}

inline bool ParticleDataChannelTypeFromName(
    std::string_view name, ParticleDataChannelType& outType)
{
    if (name == "float") { outType = ParticleDataChannelType::Float; return true; }
    if (name == "vec2") { outType = ParticleDataChannelType::Vec2; return true; }
    if (name == "vec3") { outType = ParticleDataChannelType::Vec3; return true; }
    if (name == "vec4") { outType = ParticleDataChannelType::Vec4; return true; }
    if (name == "int") { outType = ParticleDataChannelType::Int; return true; }
    if (name == "bool") { outType = ParticleDataChannelType::Bool; return true; }
    if (name == "quat" || name == "quaternion")
    {
        outType = ParticleDataChannelType::Quat;
        return true;
    }
    return false;
}

struct ParticleDataChannelValue
{
    ParticleDataChannelType type = ParticleDataChannelType::Float;
    std::array<float, kParticleDataChannelMaxComponents> values{
        0.0f, 0.0f, 0.0f, 0.0f};

    ParticleDataChannelValue() = default;
    // 保留现有 float 路径的隐式转换，老代码仍可把标量直接写进 typed map。
    ParticleDataChannelValue(float value)
        : type(ParticleDataChannelType::Float)
    {
        values[0] = value;
    }

    static ParticleDataChannelValue MakeFloat(float value)
    {
        return ParticleDataChannelValue(value);
    }

    static ParticleDataChannelValue MakeInt(int32_t value)
    {
        ParticleDataChannelValue result;
        result.type = ParticleDataChannelType::Int;
        result.values[0] = static_cast<float>(value);
        return result;
    }

    static ParticleDataChannelValue MakeBool(bool value)
    {
        ParticleDataChannelValue result;
        result.type = ParticleDataChannelType::Bool;
        result.values[0] = value ? 1.0f : 0.0f;
        return result;
    }

    static ParticleDataChannelValue MakeVec2(float x, float y)
    {
        ParticleDataChannelValue result;
        result.type = ParticleDataChannelType::Vec2;
        result.values = {x, y, 0.0f, 0.0f};
        return result;
    }

    static ParticleDataChannelValue MakeVec3(float x, float y, float z)
    {
        ParticleDataChannelValue result;
        result.type = ParticleDataChannelType::Vec3;
        result.values = {x, y, z, 0.0f};
        return result;
    }

    static ParticleDataChannelValue MakeVec4(
        float x, float y, float z, float w)
    {
        ParticleDataChannelValue result;
        result.type = ParticleDataChannelType::Vec4;
        result.values = {x, y, z, w};
        return result;
    }

    static ParticleDataChannelValue MakeQuat(
        float x, float y, float z, float w)
    {
        ParticleDataChannelValue result;
        result.type = ParticleDataChannelType::Quat;
        result.values = {x, y, z, w};
        return result;
    }

    size_t ComponentCount() const
    {
        return ParticleDataChannelComponentCount(type);
    }

    bool IsScalar() const
    {
        return IsParticleDataChannelScalar(type);
    }

    bool TryGetFloat(float& outValue) const
    {
        if (!IsScalar()) return false;
        outValue = values[0];
        return true;
    }

    float AsFloat() const
    {
        float value = 0.0f;
        TryGetFloat(value);
        return value;
    }

    // 兼容既有测试与逐帧消费端：只有标量类型可按 float 读取。
    operator float() const { return AsFloat(); }

    bool Validate(std::string& outError) const
    {
        const size_t count = ComponentCount();
        for (size_t index = 0; index < count; ++index)
        {
            const float value = values[index];
            if (!std::isfinite(value) ||
                std::abs(value) > kParticleDataChannelComponentLimit)
            {
                outError =
                    "Particle data channel ";
                outError += ParticleDataChannelTypeName(type);
                outError +=
                    " components must be finite and in [-1e6, 1e6].";
                return false;
            }
        }
        if (type == ParticleDataChannelType::Int &&
            std::trunc(values[0]) != values[0])
        {
            outError = "Particle data channel int value must be integral.";
            return false;
        }
        if (type == ParticleDataChannelType::Bool &&
            values[0] != 0.0f && values[0] != 1.0f)
        {
            outError = "Particle data channel bool value must be 0 or 1.";
            return false;
        }
        outError.clear();
        return true;
    }

    // 紧凑编码：Float/Bool/向量使用自然 JSON 形态，Int/Quat 使用
    // {type,values} 以保留无法从普通 JSON 标量/四元数数组推断的类型。
    nlohmann::json ToJson() const
    {
        switch (type)
        {
        case ParticleDataChannelType::Float:
            return values[0];
        case ParticleDataChannelType::Bool:
            return values[0] != 0.0f;
        case ParticleDataChannelType::Vec2:
        case ParticleDataChannelType::Vec3:
        case ParticleDataChannelType::Vec4:
        {
            nlohmann::json result = nlohmann::json::array();
            for (size_t index = 0; index < ComponentCount(); ++index)
                result.push_back(values[index]);
            return result;
        }
        case ParticleDataChannelType::Int:
            return {
                {"type", ParticleDataChannelTypeName(type)},
                {"values", nlohmann::json::array(
                    {static_cast<long long>(
                        std::llround(values[0]))})}};
        case ParticleDataChannelType::Quat:
            return {
                {"type", ParticleDataChannelTypeName(type)},
                {"values", nlohmann::json::array(
                    {values[0], values[1], values[2], values[3]})}};
        }
        return values[0];
    }

    // 显式编码：C# 提交和调试输出都用 {type,values}，避免歧义。
    nlohmann::json ToTypedJson() const
    {
        nlohmann::json encodedValues = nlohmann::json::array();
        for (size_t index = 0; index < ComponentCount(); ++index)
        {
            if (type == ParticleDataChannelType::Int)
            {
                encodedValues.push_back(static_cast<long long>(
                    std::llround(values[index])));
            }
            else if (type == ParticleDataChannelType::Bool)
            {
                encodedValues.push_back(values[index] != 0.0f);
            }
            else
            {
                encodedValues.push_back(values[index]);
            }
        }
        return {
            {"type", ParticleDataChannelTypeName(type)},
            {"values", std::move(encodedValues)}};
    }

    static bool ParseJson(const nlohmann::json& json,
        ParticleDataChannelValue& outValue, std::string& outError)
    {
        outValue = ParticleDataChannelValue();
        if (json.is_number())
        {
            if (!JsonNumberToFloat(json, outValue.values[0]))
            {
                outError =
                    "Particle data channel number must be finite and in "
                    "[-1e6, 1e6].";
                return false;
            }
            outValue.type = ParticleDataChannelType::Float;
            return ValidateParsed(outValue, outError);
        }
        if (json.is_boolean())
        {
            outValue = MakeBool(json.get<bool>());
            return true;
        }
        if (json.is_array())
        {
            return ParseArray(json, outValue, outError);
        }
        if (!json.is_object())
        {
            outError =
                "Particle data channel value must be a number, bool, "
                "array, or typed object.";
            return false;
        }

        if (json.contains("type"))
        {
            const nlohmann::json& typeJson = json.at("type");
            ParticleDataChannelType type = ParticleDataChannelType::Float;
            if (typeJson.is_string())
            {
                if (!ParticleDataChannelTypeFromName(
                        typeJson.get<std::string>(), type))
                {
                    outError = "Particle data channel type is unknown: " +
                        typeJson.dump();
                    return false;
                }
            }
            else if (typeJson.is_number_integer())
            {
                const int rawType = typeJson.get<int>();
                if (rawType < 0 || rawType > 6)
                {
                    outError =
                        "Particle data channel type id is out of range.";
                    return false;
                }
                type = static_cast<ParticleDataChannelType>(rawType);
            }
            else
            {
                outError =
                    "Particle data channel type must be a string.";
                return false;
            }
            if (json.contains("values"))
                return ParseTyped(type, json.at("values"), outValue, outError);
            if (json.contains("value"))
                return ParseTyped(type, json.at("value"), outValue, outError);
            outError =
                "Particle data channel typed object requires values.";
            return false;
        }

        for (const auto& [key, type] : std::array<std::pair<const char*,
                 ParticleDataChannelType>, 7>{{
                 {"float", ParticleDataChannelType::Float},
                 {"vec2", ParticleDataChannelType::Vec2},
                 {"vec3", ParticleDataChannelType::Vec3},
                 {"vec4", ParticleDataChannelType::Vec4},
                 {"int", ParticleDataChannelType::Int},
                 {"bool", ParticleDataChannelType::Bool},
                 {"quat", ParticleDataChannelType::Quat}}})
        {
            if (json.contains(key))
                return ParseTyped(type, json.at(key), outValue, outError);
        }
        if (json.contains("values"))
            return ParseJson(json.at("values"), outValue, outError);
        if (json.contains("value"))
            return ParseJson(json.at("value"), outValue, outError);
        outError =
            "Particle data channel object requires type or values.";
        return false;
    }

    static bool ParseTyped(ParticleDataChannelType type,
        const nlohmann::json& json, ParticleDataChannelValue& outValue,
        std::string& outError)
    {
        outValue = ParticleDataChannelValue();
        outValue.type = type;
        const size_t required = ParticleDataChannelComponentCount(type);
        if (isScalarJson(json))
        {
            if (required != 1)
            {
                outError = "Particle data channel ";
                outError += ParticleDataChannelTypeName(type);
                outError += " requires ";
                outError += std::to_string(required);
                outError += " numeric components.";
                return false;
            }
            if (type == ParticleDataChannelType::Bool)
            {
                if (json.is_boolean())
                    outValue.values[0] = json.get<bool>() ? 1.0f : 0.0f;
                else if (json.is_number())
                {
                    if (!JsonNumberToFloat(json, outValue.values[0]))
                    {
                        outError =
                            "Particle data channel bool requires 0 or 1.";
                        return false;
                    }
                }
                else
                {
                    outError =
                        "Particle data channel bool requires true/false.";
                    return false;
                }
            }
            else
            {
                if (!json.is_number())
                {
                    outError = "Particle data channel ";
                    outError += ParticleDataChannelTypeName(type);
                    outError += " requires a number.";
                    return false;
                }
                if (!JsonNumberToFloat(json, outValue.values[0]))
                {
                    outError = "Particle data channel ";
                    outError += ParticleDataChannelTypeName(type);
                    outError +=
                        " components must be finite and in [-1e6, 1e6].";
                    return false;
                }
            }
        }
        else if (json.is_array())
        {
            if (json.size() != required)
            {
                outError = "Particle data channel ";
                outError += ParticleDataChannelTypeName(type);
                outError += " requires ";
                outError += std::to_string(required);
                outError += " numeric components.";
                return false;
            }
            for (size_t index = 0; index < required; ++index)
            {
                const nlohmann::json& component = json.at(index);
                if (type == ParticleDataChannelType::Bool &&
                    component.is_boolean())
                {
                    outValue.values[index] =
                        component.get<bool>() ? 1.0f : 0.0f;
                }
                else if (component.is_number())
                {
                    if (!JsonNumberToFloat(
                            component, outValue.values[index]))
                    {
                        outError = "Particle data channel ";
                        outError += ParticleDataChannelTypeName(type);
                        outError +=
                            " components must be finite and in [-1e6, 1e6].";
                        return false;
                    }
                }
                else
                {
                    outError = "Particle data channel ";
                    outError += ParticleDataChannelTypeName(type);
                    outError += " requires numeric components.";
                    return false;
                }
            }
        }
        else
        {
            outError = "Particle data channel ";
            outError += ParticleDataChannelTypeName(type);
            outError += " requires ";
            outError += std::to_string(required);
            outError += " numeric components.";
            return false;
        }

        if (type == ParticleDataChannelType::Int &&
            std::trunc(outValue.values[0]) != outValue.values[0])
        {
            outError = "Particle data channel int value must be integral.";
            return false;
        }
        return ValidateParsed(outValue, outError);
    }

    static bool ParseArray(const nlohmann::json& json,
        ParticleDataChannelValue& outValue, std::string& outError)
    {
        ParticleDataChannelType type = ParticleDataChannelType::Float;
        switch (json.size())
        {
        case 2: type = ParticleDataChannelType::Vec2; break;
        case 3: type = ParticleDataChannelType::Vec3; break;
        case 4: type = ParticleDataChannelType::Vec4; break;
        default:
            outError =
                "Particle data channel array requires 2, 3, or 4 components.";
            return false;
        }
        return ParseTyped(type, json, outValue, outError);
    }

    static bool isScalarJson(const nlohmann::json& json)
    {
        return json.is_number() || json.is_boolean();
    }

    static bool JsonNumberToFloat(
        const nlohmann::json& json, float& outValue)
    {
        if (!json.is_number()) return false;
        const double value = json.get<double>();
        if (!std::isfinite(value) ||
            std::abs(value) > static_cast<double>(
                kParticleDataChannelComponentLimit))
        {
            return false;
        }
        outValue = static_cast<float>(value);
        return true;
    }

    static bool ValidateParsed(const ParticleDataChannelValue& value,
        std::string& outError)
    {
        return value.Validate(outError);
    }
};

// 现有 World/快照消费端需要 std::unordered_map<string,float> 的隐式视图；
// 仅 Float/Int/Bool 参与转换，向量与四元数必须显式按类型读取。
class ParticleDataChannelMap
    : public std::unordered_map<std::string, ParticleDataChannelValue>
{
public:
    using Base =
        std::unordered_map<std::string, ParticleDataChannelValue>;
    using Base::Base;

    ParticleDataChannelMap() = default;
    ParticleDataChannelMap(const Base& values) : Base(values) {}
    ParticleDataChannelMap(Base&& values) : Base(std::move(values)) {}

    bool TryGetFloat(const std::string& name, float& outValue) const
    {
        const auto found = find(name);
        if (found == end()) return false;
        return found->second.TryGetFloat(outValue);
    }

    operator std::unordered_map<std::string, float>() const
    {
        std::unordered_map<std::string, float> scalars;
        scalars.reserve(size());
        for (const auto& [name, value] : *this)
        {
            float scalar = 0.0f;
            if (value.TryGetFloat(scalar))
                scalars.emplace(name, scalar);
        }
        return scalars;
    }
};

inline bool ParseParticleDataChannelFloatToken(
    const std::string& token, float& outValue)
{
    try
    {
        size_t consumed = 0;
        const float value = std::stof(token, &consumed);
        if (consumed != token.size()) return false;
        outValue = value;
        return true;
    }
    catch (...)
    {
        return false;
    }
}

inline bool ParseParticleDataChannelIntToken(
    const std::string& token, int32_t& outValue)
{
    try
    {
        size_t consumed = 0;
        const long long value = std::stoll(token, &consumed);
        if (consumed != token.size()) return false;
        if (value < -1000000LL || value > 1000000LL) return false;
        outValue = static_cast<int32_t>(value);
        return true;
    }
    catch (...)
    {
        return false;
    }
}

// Console `particledata <name> ...` 的类型化值语法。tokens 是 name 之后的
// 全部 token；错误文本直接回显给 Console，避免只打印泛化的 usage。
inline bool ParseParticleDataChannelText(
    const std::vector<std::string>& tokens,
    ParticleDataChannelValue& outValue, std::string& outError)
{
    outValue = ParticleDataChannelValue();
    if (tokens.empty())
    {
        outError = "Particle data channel value is required.";
        return false;
    }

    const std::string& kind = tokens[0];
    const auto parseComponents = [&](ParticleDataChannelType type,
        size_t count) -> bool
    {
        if (tokens.size() != count + 1)
        {
            outError = "Particle data channel ";
            outError += ParticleDataChannelTypeName(type);
            outError += " requires ";
            outError += std::to_string(count);
            outError += " numeric components.";
            return false;
        }
        outValue.type = type;
        for (size_t index = 0; index < count; ++index)
        {
            if (!ParseParticleDataChannelFloatToken(
                    tokens[index + 1], outValue.values[index]))
            {
                outError = "Particle data channel ";
                outError += ParticleDataChannelTypeName(type);
                outError += " requires numeric components.";
                return false;
            }
        }
        return outValue.Validate(outError);
    };

    if (kind == "vec2") return parseComponents(
        ParticleDataChannelType::Vec2, 2);
    if (kind == "vec3") return parseComponents(
        ParticleDataChannelType::Vec3, 3);
    if (kind == "vec4") return parseComponents(
        ParticleDataChannelType::Vec4, 4);
    if (kind == "quat" || kind == "quaternion")
        return parseComponents(ParticleDataChannelType::Quat, 4);
    if (kind == "int")
    {
        int32_t value = 0;
        if (tokens.size() != 2 ||
            !ParseParticleDataChannelIntToken(tokens[1], value))
        {
            outError =
                "Particle data channel int requires one integer in "
                "[-1000000, 1000000].";
            return false;
        }
        outValue = ParticleDataChannelValue::MakeInt(value);
        return true;
    }
    if (kind == "bool")
    {
        if (tokens.size() != 2)
        {
            outError =
                "Particle data channel bool requires true or false.";
            return false;
        }
        if (tokens[1] == "true" || tokens[1] == "1")
            outValue = ParticleDataChannelValue::MakeBool(true);
        else if (tokens[1] == "false" || tokens[1] == "0")
            outValue = ParticleDataChannelValue::MakeBool(false);
        else
        {
            outError =
                "Particle data channel bool requires true or false.";
            return false;
        }
        return true;
    }
    if (kind == "float")
    {
        if (tokens.size() != 2 ||
            !ParseParticleDataChannelFloatToken(tokens[1], outValue.values[0]))
        {
            outError = "Particle data channel float requires one number.";
            return false;
        }
        outValue.type = ParticleDataChannelType::Float;
        return outValue.Validate(outError);
    }

    if (tokens.size() != 1 ||
        !ParseParticleDataChannelFloatToken(tokens[0], outValue.values[0]))
    {
        outError =
            "Particle data channel value must be a number or one of "
            "vec2/vec3/vec4/quat/bool/int.";
        return false;
    }
    outValue.type = ParticleDataChannelType::Float;
    return outValue.Validate(outError);
}

} // namespace VL
