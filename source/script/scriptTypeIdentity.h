#pragma once

#include <string>
#include <nlohmann/json.hpp>

namespace VL
{
// 仅匹配当前宿主类目录的身份；类名不能触发任意程序集加载或绕过默认值/实例权限校验。
inline std::string NormalizeScriptTypeIdentity(const std::string& value)
{
    std::string result;
    size_t begin = 0;
    while (true)
    {
        const auto end = value.find(',', begin);
        const auto token = value.substr(begin, end == std::string::npos ? end : end - begin);
        const auto first = token.find_first_not_of(" \t\r\n");
        if (begin != 0) result += ',';
        if (first != std::string::npos)
            result += token.substr(first, token.find_last_not_of(" \t\r\n") - first + 1);
        if (end == std::string::npos) return result;
        begin = end + 1;
    }
}

inline bool ScriptTypeNameMatches(const nlohmann::json& type, const std::string& requested)
{
    const auto identity = NormalizeScriptTypeIdentity(requested);
    const auto name = type.at("name").get<std::string>();
    if (identity == name) return true;
    const auto assembly = type.value("assemblyName", std::string());
    if (!assembly.empty() && identity == name + "," + assembly) return true;
    const auto qualified = type.value("assemblyQualifiedName", std::string());
    return !qualified.empty() && identity == NormalizeScriptTypeIdentity(qualified);
}
} // namespace VL
