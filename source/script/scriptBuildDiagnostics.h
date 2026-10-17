#pragma once

#include <regex>
#include <sstream>
#include "core/logStore.h"

namespace VL
{
struct ScriptBuildLogLine
{
    LogSeverity severity = LogSeverity::Info;
    std::string message;
    LogLocation location;
};

inline std::vector<ScriptBuildLogLine> ParseScriptBuildDiagnostics(const std::string& output)
{
    // 保留编译器原文；只解析位置与级别，工程名后缀不改变文件路径。
    static const std::regex diagnostic(R"(^(.*)\(([0-9]+),([0-9]+)\): (warning|error) .*$)");
    std::vector<ScriptBuildLogLine> result;
    std::istringstream lines(output);
    std::string line;
    while (std::getline(lines, line))
    {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (line.empty()) continue;
        ScriptBuildLogLine entry;
        entry.message = line;
        std::smatch match;
        if (std::regex_match(line, match, diagnostic))
        {
            entry.location.file = match[1].str();
            // 编译器行列是正整数；过长数字仍保留原文，不让日志解析打断构建结果发布。
            try
            {
                const auto row = std::stoull(match[2].str()), column = std::stoull(match[3].str());
                if (row <= UINT32_MAX && column <= UINT32_MAX)
                { entry.location.line = uint32_t(row); entry.location.column = uint32_t(column); }
            }
            catch (const std::exception&) {}
            entry.severity = match[4] == "error" ? LogSeverity::Error : LogSeverity::Warning;
        }
        else if (line.find("error ") != std::string::npos || line.find("Build FAILED") != std::string::npos)
            entry.severity = LogSeverity::Error;
        else if (line.find("warning ") != std::string::npos)
            entry.severity = LogSeverity::Warning;
        result.push_back(std::move(entry));
    }
    return result;
}
} // namespace VL
