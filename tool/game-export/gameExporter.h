#pragma once

#include <filesystem>
#include <string>
#include <vector>
#include <nlohmann/json.hpp>

namespace VL
{
struct GameExportOptions
{
    std::filesystem::path projectRoot;
    std::filesystem::path binaryDirectory;
    std::filesystem::path outputDirectory;
    std::filesystem::path dotnetRoot;
    std::string initialScene;
    bool csharpEnabled = true;
    // 动态拼出的资源路径无法静态发现，由作者显式声明资源根目录相对路径。
    std::vector<std::string> dynamicAssets;
};

struct GameExportResult
{
    size_t fileCount = 0;
    uintmax_t totalBytes = 0;
    nlohmann::json manifest;
};

GameExportResult ExportGame(const GameExportOptions& options);

// 仅解析 PE 导入表，不把待打包 DLL 加载进导出器进程。
std::vector<std::filesystem::path> CollectNativeGameDependencies(
    const std::filesystem::path& executable);
} // namespace VL
