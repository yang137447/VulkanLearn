#pragma once

#include <cmath>
#include <cstdint>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

#include "render/camera/cameraExposure.h"

// 控制台实时调参命令的纯解析层：不触碰 CommandBus/World，
// 便于用 GoogleTest 覆盖参数边界，命令处理只负责把结果翻译成 RuntimeCommand。
namespace VL::ConsoleCommandParsing
{

inline std::vector<std::string> SplitTokens(std::string_view text)
{
    std::istringstream stream{ std::string(text) };
    std::vector<std::string> tokens;
    std::string token;
    while (stream >> token)
    {
        tokens.push_back(token);
    }
    return tokens;
}

inline bool ParseFiniteFloat(const std::string& token, float& value)
{
    std::istringstream stream(token);
    float parsed = 0.0f;
    char trailing = '\0';
    if (!(stream >> parsed) || (stream >> trailing) || !std::isfinite(parsed))
    {
        return false;
    }
    value = parsed;
    return true;
}

struct SunArgument
{
    bool ok = false;
    float lux = 0.0f;
};

// sun <lux>：只接受一个有限正数；非法输入不提交命令。
inline SunArgument ParseSunIntensity(std::string_view text)
{
    const std::vector<std::string> tokens = SplitTokens(text);
    if (tokens.size() != 1)
    {
        return {};
    }
    SunArgument argument;
    if (!ParseFiniteFloat(tokens[0], argument.lux) || argument.lux <= 0.0f)
    {
        return {};
    }
    argument.ok = true;
    return argument;
}

struct MaterialParameterArgument
{
    bool ok = false;
    std::string materialInstanceKey;
    std::string parameterName;
    uint32_t valueCount = 0;
    float values[4] = { 0.0f, 0.0f, 0.0f, 0.0f };
};

// matparam <instanceKey> <paramName> [v0 [v1 [v2 [v3]]]]
// 1 个分量走 scalar，2 个分量按 vec2 提交（当前材质系统没有 vec2 参数类型，
// 由渲染线程的类型校验给出诊断），3~4 个分量走 vec3/vec4。
inline MaterialParameterArgument ParseMaterialParameter(std::string_view text)
{
    const std::vector<std::string> tokens = SplitTokens(text);
    if (tokens.size() < 3 || tokens.size() > 6)
    {
        return {};
    }

    MaterialParameterArgument argument;
    argument.materialInstanceKey = tokens[0];
    argument.parameterName = tokens[1];
    argument.valueCount = static_cast<uint32_t>(tokens.size() - 2);
    for (uint32_t index = 0; index < argument.valueCount; ++index)
    {
        if (!ParseFiniteFloat(tokens[index + 2], argument.values[index]))
        {
            return {};
        }
    }
    argument.ok = true;
    return argument;
}

} // namespace VL::ConsoleCommandParsing
