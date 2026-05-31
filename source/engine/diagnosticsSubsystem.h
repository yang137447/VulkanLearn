#pragma once

#include <string>

#include "core/runtimeResult.h"

namespace VL
{

enum class DiagnosticSeverity
{
    Info,
    Warning,
    Error
};

// Central report point for runtime diagnostics. It deliberately starts small:
// console output today, profiler overlays or test hooks later without changing
// every engine subsystem that wants to report a runtime event.
class DiagnosticsSubsystem
{
public:
    void ReportInfo(const std::string& message) const;
    void ReportWarning(const std::string& message) const;
    void ReportError(const std::string& message) const;
    void ReportRuntimeError(const std::string& context, const RuntimeError& error) const;

private:
    void Report(DiagnosticSeverity severity, const std::string& message) const;
};

} // namespace VL
