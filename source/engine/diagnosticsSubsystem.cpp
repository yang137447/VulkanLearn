#include "engine/diagnosticsSubsystem.h"

#include <iostream>

namespace VL
{
namespace
{

const char* ToString(DiagnosticSeverity severity)
{
    switch (severity)
    {
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

void DiagnosticsSubsystem::ReportInfo(const std::string& message) const
{
    Report(DiagnosticSeverity::Info, message);
}

void DiagnosticsSubsystem::ReportWarning(const std::string& message) const
{
    Report(DiagnosticSeverity::Warning, message);
}

void DiagnosticsSubsystem::ReportError(const std::string& message) const
{
    Report(DiagnosticSeverity::Error, message);
}

void DiagnosticsSubsystem::ReportRuntimeError(
    const std::string& context,
    const RuntimeError& error) const
{
    ReportError(context + ": " + FormatRuntimeError(error));
}

void DiagnosticsSubsystem::Report(DiagnosticSeverity severity, const std::string& message) const
{
    std::ostream& output = severity == DiagnosticSeverity::Error ? std::cerr : std::cout;
    output << "[Diagnostics][" << ToString(severity) << "] " << message << std::endl;
}

} // namespace VL
