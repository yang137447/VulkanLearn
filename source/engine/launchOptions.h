#pragma once

#include <optional>
#include <string>

namespace VL
{

class EngineLoop;

enum class DeveloperUiLaunchMode
{
    UseConfig,
    Enabled,
    Disabled
};

// Owns command-line launch parsing and translation into startup runtime
// commands. It does not initialize platform, config, renderer, or world state.
struct LaunchOptions
{
    bool showHelp = false;
    bool runShaderReloadTest = false;
    bool runShaderComputeReloadTest = false;
    bool runWorldGraphTransactionTest = false;
    bool forceShaderRebuild = false;
    bool exitAfterTests = false;
    DeveloperUiLaunchMode developerUiMode = DeveloperUiLaunchMode::UseConfig;
    std::optional<int> workerThreadCountOverride;
    std::string initialSceneOverride;
    std::string errorMessage;
};

LaunchOptions ParseLaunchOptions(int argc, char** argv);
void PrintLaunchUsage();
void QueueLaunchCommands(EngineLoop& engineLoop, const LaunchOptions& options);

} // namespace VL
