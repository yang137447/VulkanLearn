#include "engine/diagnosticsSubsystem.h"

#include <iostream>
#include "core/logStore.h"

namespace VL
{
namespace
{

const char* ToString(DiagnosticSeverity severity)
{
    switch (severity)
    {
    case DiagnosticSeverity::Debug:
        return "Debug";
    case DiagnosticSeverity::Info:
        return "Info";
    case DiagnosticSeverity::Warning:
        return "Warning";
    case DiagnosticSeverity::Error:
        return "Error";
    }

    return "Unknown";
}

} // namespace

void DiagnosticsSubsystem::ReportDebug(const std::string& message, const std::string& source,
    LogLocation location) const
{
    Report(DiagnosticSeverity::Debug, message, source, std::move(location));
}

void DiagnosticsSubsystem::ReportInfo(const std::string& message, const std::string& source,
    LogLocation location) const
{
    Report(DiagnosticSeverity::Info, message, source, std::move(location));
}

void DiagnosticsSubsystem::ReportWarning(const std::string& message, const std::string& source,
    LogLocation location) const
{
    Report(DiagnosticSeverity::Warning, message, source, std::move(location));
}

void DiagnosticsSubsystem::ReportError(const std::string& message, const std::string& source,
    LogLocation location) const
{
    Report(DiagnosticSeverity::Error, message, source, std::move(location));
}

void DiagnosticsSubsystem::ReportRuntimeError(
    const std::string& context,
    const RuntimeError& error) const
{
    ReportError(context + ": " + FormatRuntimeError(error));
}

void DiagnosticsSubsystem::Report(DiagnosticSeverity severity, const std::string& message,
    const std::string& source, LogLocation location) const
{
    const auto level = severity == DiagnosticSeverity::Error ? LogSeverity::Error :
        severity == DiagnosticSeverity::Warning ? LogSeverity::Warning :
        severity == DiagnosticSeverity::Debug ? LogSeverity::Debug : LogSeverity::Info;
    ProcessLog().Append(level, source, message, std::move(location));
    std::ostream& output = severity == DiagnosticSeverity::Error ? std::cerr : std::cout;
    output << "[Diagnostics][" << ToString(severity) << "] " << message << std::endl;
}

} // namespace VL
