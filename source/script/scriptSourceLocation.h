#pragma once

#include <filesystem>
#include <nlohmann/json.hpp>
#include "script/scriptSourceFiles.h"
#include "script/scriptTypeIdentity.h"

namespace VL
{
inline std::string ScriptSourceEditorUrl(const std::filesystem::path& resourceRoot,
    const nlohmann::json& classes, const std::string& className)
{
    const auto root = std::filesystem::weakly_canonical(resourceRoot);
    for (const auto& type : classes)
    {
        if (!ScriptTypeNameMatches(type, className)) continue;
        for (const auto& file : type.value("sourceFiles", nlohmann::json::array()))
        {
            const auto input = std::filesystem::u8path(file.get<std::string>());
            const auto path = std::filesystem::weakly_canonical(input.is_absolute() ? input : root / input);
            const auto relative = path.lexically_relative(root);
            if (relative.empty() || relative.is_absolute() || *relative.begin() == "..") continue;
            const auto domain = relative.begin()->string();
            if (domain != "Common" && domain != "Maps") continue;
            bool excluded = false;
            for (const auto& part : relative.parent_path())
                excluded = excluded || ScriptSourceDirectoryExcluded(part.string());
            auto extension = path.extension().string();
            for (char& value : extension)
                if (value >= 'A' && value <= 'Z') value += 'a' - 'A';
            if (excluded || !std::filesystem::is_regular_file(path) || extension != ".cs") continue;
            // URI 只承载已注册类型的资源源码；不执行 shell，也不接受 UI 直接指定文件。
            std::string url = "vscode://file/";
            constexpr char Hex[] = "0123456789ABCDEF";
            for (const unsigned char byte : path.generic_u8string())
            {
                if ((byte >= 'a' && byte <= 'z') || (byte >= 'A' && byte <= 'Z') ||
                    (byte >= '0' && byte <= '9') || byte == '/' || byte == ':' ||
                    byte == '-' || byte == '_' || byte == '.' || byte == '~')
                    url += static_cast<char>(byte);
                else { url += '%'; url += Hex[byte >> 4]; url += Hex[byte & 15]; }
            }
            return url;
        }
        throw std::runtime_error("No available resource source location for C# class: " + className);
    }
    throw std::runtime_error("The C# class is no longer registered: " + className);
}
}
