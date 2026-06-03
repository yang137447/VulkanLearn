#include "engine/diagnosticsSubsystem.h"
#include "engine/engineLoop.h"
#include "engine/launchOptions.h"
#include "engine/runtimeConfig.h"
#include "platform/platformApplication.h"

#include <iostream>

int main(int argc, char **argv)
{
    const VL::LaunchOptions launchOptions = VL::ParseLaunchOptions(argc, argv);
    if (launchOptions.showHelp)
    {
        VL::PrintLaunchUsage();
        return 0;
    }
    if (!launchOptions.errorMessage.empty())
    {
        std::cerr << launchOptions.errorMessage << std::endl;
        VL::PrintLaunchUsage();
        return 1;
    }

    VL::DiagnosticsSubsystem diagnostics;
    VL::RuntimeConfig runtimeConfig;
    auto configResult = runtimeConfig.Load();
    if (configResult.IsFailure())
    {
        diagnostics.ReportRuntimeError("Runtime config load failed", configResult.Error());
        return 1;
    }

    VL::PlatformApplicationDesc platformDesc;
    platformDesc.windowWidth = static_cast<int>(runtimeConfig.GetWindowSize().x());
    platformDesc.windowHeight = static_cast<int>(runtimeConfig.GetWindowSize().y());

    VL::PlatformApplication platformApplication;
    auto platformResult = platformApplication.Initialize(platformDesc);
    if (platformResult.IsFailure())
    {
        diagnostics.ReportRuntimeError("Platform init failed", platformResult.Error());
        platformApplication.Shutdown();
        return 1;
    }

    VL::EngineLoop engineLoop;
    auto initResult = engineLoop.Init(platformApplication, runtimeConfig);
    if (initResult.IsFailure())
    {
        diagnostics.ReportRuntimeError("EngineLoop init failed", initResult.Error());
        engineLoop.Shutdown();
        platformApplication.Shutdown();
        return 1;
    }

    VL::QueueLaunchCommands(engineLoop, launchOptions);
    const int exitCode = engineLoop.Run();
    engineLoop.Shutdown();
    platformApplication.Shutdown();
    return exitCode;
}
