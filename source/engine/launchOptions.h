#pragma once

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
    bool runReloadStress = false;
    bool runReloadFailureRollbackTest = false;
    bool runGeneratedMaterialFailureRollbackTest = false;
    bool runGeneratedMeshFailureRollbackTest = false;
    bool runGeneratedTextureFailureRollbackTest = false;
    bool runGeneratedHighLightReloadStress = false;
    bool runResizeStress = false;
    bool runRenderGraphReloadStress = false;
    bool runFrameSmokeTest = false;
    bool runEnvironmentUpdateStress = false;
    bool exitAfterTests = false;
    DeveloperUiLaunchMode developerUiMode = DeveloperUiLaunchMode::UseConfig;
    std::string reloadStressScenePath;
    std::string reloadFailureScenePath;
    int reloadStressCount = 20;
    int highLightStressCount = 3;
    int resizeStressCount = 6;
    int graphReloadStressCount = 6;
    int frameSmokeCount = 120;
    int environmentUpdateStressCount = 3;
    std::string errorMessage;
};

LaunchOptions ParseLaunchOptions(int argc, char** argv);
void PrintLaunchUsage();
void QueueLaunchCommands(EngineLoop& engineLoop, const LaunchOptions& options);

} // namespace VL
