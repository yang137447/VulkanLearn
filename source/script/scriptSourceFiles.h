#pragma once

#include <algorithm>
#include <filesystem>
#include <stdexcept>
#include <string>
#include <vector>

namespace VL
{
inline bool ScriptSourceDirectoryExcluded(std::string name)
{
    for (char& character : name)
        if (character >= 'A' && character <= 'Z') character += 'a' - 'A';
    return name == "bin" || name == "obj" || name == ".git" || name == "generated";
}

// 用户源码只来自正式内容域；编译清单、摘要和创建冲突检查共用同一集合，不读取 Generated。
inline std::vector<std::filesystem::path> CollectScriptSourceFiles(const std::filesystem::path& resourceRoot)
{
    namespace fs = std::filesystem;
    const auto root = fs::weakly_canonical(fs::absolute(resourceRoot));
    std::vector<fs::path> files;
    for (const char* domain : {"Common", "Maps"})
    {
        const auto directory = root / domain;
        if (!fs::exists(directory)) continue;
        if (fs::is_symlink(fs::symlink_status(directory)))
            throw std::invalid_argument("C# source domain cannot be a symbolic link: " + directory.string());
        for (auto it = fs::recursive_directory_iterator(directory); it != fs::recursive_directory_iterator(); ++it)
        {
            const auto name = it->path().filename().string();
            if (it->is_directory() && ScriptSourceDirectoryExcluded(name))
            { it.disable_recursion_pending(); continue; }
            auto extension = it->path().extension().string();
            for (char& character : extension)
                if (character >= 'A' && character <= 'Z') character += 'a' - 'A';
            if (fs::is_symlink(it->symlink_status()))
            {
                if (it->is_directory() || extension == ".cs")
                    throw std::invalid_argument("C# sources cannot traverse symbolic links: " + it->path().string());
                continue;
            }
            if (it->is_regular_file() && extension == ".cs")
                files.push_back(it->path());
        }
    }
    std::sort(files.begin(), files.end());
    return files;
}

// MSBuild Include 同时解释 XML 和项目表达式；源码绝对路径必须作为字面量进入清单。
inline std::string ScriptSourceItems(const std::filesystem::path& resourceRoot)
{
    std::string text = "<Project><ItemGroup>";
    for (const auto& file : CollectScriptSourceFiles(resourceRoot))
    {
        text += "<Compile Include=\"";
        for (const char character : file.generic_u8string())
        {
            switch (character)
            {
            case '&': text += "&amp;"; break;
            case '<': text += "&lt;"; break;
            case '>': text += "&gt;"; break;
            case '"': text += "&quot;"; break;
            case '%': text += "%25"; break;
            case '$': text += "%24"; break;
            case '@': text += "%40"; break;
            case ';': text += "%3B"; break;
            case '\'': text += "%27"; break;
            case '(': text += "%28"; break;
            case ')': text += "%29"; break;
            case '*': text += "%2A"; break;
            case '?': text += "%3F"; break;
            default: text += character; break;
            }
        }
        text += "\"/>";
    }
    return text + "</ItemGroup></Project>";
}
}
