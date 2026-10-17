#pragma once

#include <string>

#include "core/runtimeResult.h"
#include "core/logStore.h"

namespace VL
{

enum class DiagnosticSeverity
{
    Debug,
    Info,
    Warning,
    Error
};

// 控制台保留原有输出；结构化日志另存入有界 owner，UI 不访问 producer。
class DiagnosticsSubsystem
{
public:
    void ReportDebug(const std::string& message, const std::string& source = "Engine", LogLocation location = {}) const;
    void ReportInfo(const std::string& message, const std::string& source = "Engine", LogLocation location = {}) const;
    void ReportWarning(const std::string& message, const std::string& source = "Engine", LogLocation location = {}) const;
    void ReportError(const std::string& message, const std::string& source = "Engine", LogLocation location = {}) const;
    void ReportRuntimeError(const std::string& context, const RuntimeError& error) const;

private:
    void Report(DiagnosticSeverity severity, const std::string& message, const std::string& source,
        LogLocation location) const;
};

} // namespace VL
