#include "engine/application.h"

#include "engine/diagnosticsSubsystem.h"
#include "engine/editorHost.h"
#include "engine/engineLoop.h"
#include "engine/launchOptions.h"
#include "engine/runtimeConfig.h"
#include "platform/platformApplication.h"

#include <filesystem>
#include <iostream>
#include <stdexcept>
#include <utility>
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

namespace VL
{

int RunApplication(
    int argc,
    char** argv,
    std::function<std::unique_ptr<IEditorHost>()> createEditorHost)
{
    try { PrepareExportedGameDirectory(); }
    catch (const std::exception& error)
    {
        std::cerr << "Game deployment root failed: " << error.what() << std::endl;
        return 1;
    }
    const LaunchOptions launchOptions = ParseLaunchOptions(argc, argv);
    if (launchOptions.showHelp)
    {
        PrintLaunchUsage();
        return 0;
    }
    if (!launchOptions.errorMessage.empty())
    {
        std::cerr << launchOptions.errorMessage << std::endl;
        PrintLaunchUsage();
        return 1;
    }

    DiagnosticsSubsystem diagnostics;
    RuntimeConfig runtimeConfig;
    auto configResult = runtimeConfig.Load();
    if (configResult.IsFailure())
    {
        diagnostics.ReportRuntimeError("Runtime config load failed", configResult.Error());
        return 1;
    }

    PlatformApplicationDesc platformDesc;
    platformDesc.windowWidth = static_cast<int>(runtimeConfig.GetWindowSize().x());
    platformDesc.windowHeight = static_cast<int>(runtimeConfig.GetWindowSize().y());

    PlatformApplication platformApplication;
    auto platformResult = platformApplication.Initialize(platformDesc);
    if (platformResult.IsFailure())
    {
        diagnostics.ReportRuntimeError("Platform init failed", platformResult.Error());
        platformApplication.Shutdown();
        return 1;
    }

    EngineLoop engineLoop;
    std::unique_ptr<IEditorHost> editorHost = createEditorHost ? createEditorHost() : nullptr;
    auto initResult = engineLoop.Init(
        platformApplication,
        runtimeConfig,
        launchOptions,
        std::move(editorHost));
    if (initResult.IsFailure())
    {
        diagnostics.ReportRuntimeError("EngineLoop init failed", initResult.Error());
        engineLoop.Shutdown();
        platformApplication.Shutdown();
        return 1;
    }

    QueueLaunchCommands(engineLoop, launchOptions);
    (void)engineLoop.Run();
    engineLoop.Shutdown();
    platformApplication.Shutdown();
    return engineLoop.GetExitCode();
}

} // namespace VL
