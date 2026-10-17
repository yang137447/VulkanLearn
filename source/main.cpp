#include "engine/diagnosticsSubsystem.h"
#include "engine/engineLoop.h"
#include "engine/launchOptions.h"
#include "engine/runtimeConfig.h"
#include "platform/platformApplication.h"

#include <iostream>
#include <filesystem>
#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#endif

namespace
{
void PrepareExportedGameDirectory()
{
#ifdef _WIN32
    wchar_t executable[32768];
    const auto size = GetModuleFileNameW(nullptr, executable, 32768);
    if (!size || size >= 32768) throw std::runtime_error("Cannot locate game executable");
    const auto root = std::filesystem::path(std::wstring(executable, size)).parent_path();
    // 导出包自带清单才采用可执行文件目录；开发启动仍由原有工作目录决定项目根。
    if (std::filesystem::is_regular_file(root / "game-export-manifest.json"))
        std::filesystem::current_path(root);
#endif
}
}

int main(int argc, char **argv)
{
    try { PrepareExportedGameDirectory(); }
    catch (const std::exception& error)
    {
        std::cerr << "Game deployment root failed: " << error.what() << std::endl;
        return 1;
    }
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
    auto initResult = engineLoop.Init(platformApplication, runtimeConfig, launchOptions);
    if (initResult.IsFailure())
    {
        diagnostics.ReportRuntimeError("EngineLoop init failed", initResult.Error());
        engineLoop.Shutdown();
        platformApplication.Shutdown();
        return 1;
    }

    VL::QueueLaunchCommands(engineLoop, launchOptions);
    (void)engineLoop.Run();
    engineLoop.Shutdown();
    platformApplication.Shutdown();
    return engineLoop.GetExitCode();
}
