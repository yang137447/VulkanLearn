#include "engine/diagnosticsSubsystem.h"
#include "engine/engineLoop.h"
#include "engine/runtimeCommand.h"
#include "engine/runtimeConfig.h"
#include "platform/platformApplication.h"

#include <iostream>
#include <string>
#include <utility>

namespace
{

struct LaunchOptions
{
    bool showHelp = false;
    bool runReloadStress = false;
    bool runReloadFailureRollbackTest = false;
    bool runGeneratedMaterialFailureRollbackTest = false;
    bool runGeneratedHighLightReloadStress = false;
    bool runResizeStress = false;
    bool runRenderGraphReloadStress = false;
    bool runFrameSmokeTest = false;
    bool exitAfterTests = false;
    std::string reloadStressScenePath;
    std::string reloadFailureScenePath;
    int reloadStressCount = 20;
    int highLightStressCount = 3;
    int resizeStressCount = 6;
    int graphReloadStressCount = 6;
    int frameSmokeCount = 120;
    std::string errorMessage;
};

bool IsOptionToken(const std::string& value)
{
    return value.size() >= 2 && value[0] == '-' && value[1] == '-';
}

bool TryParsePositiveInt(const std::string& value, int& outNumber)
{
    try
    {
        size_t parsedLength = 0;
        const int parsedValue = std::stoi(value, &parsedLength);
        if (parsedLength != value.size() || parsedValue <= 0)
        {
            return false;
        }

        outNumber = parsedValue;
        return true;
    }
    catch (...)
    {
        return false;
    }
}

LaunchOptions ParseLaunchOptions(int argc, char** argv)
{
    LaunchOptions options;
    for (int i = 1; i < argc; ++i)
    {
        const std::string argument = argv[i];
        if (argument == "--help")
        {
            options.showHelp = true;
            continue;
        }

        if (argument == "--exit-after-tests")
        {
            options.exitAfterTests = true;
            continue;
        }

        if (argument == "--reloadstress")
        {
            if (options.runReloadFailureRollbackTest ||
                options.runGeneratedMaterialFailureRollbackTest ||
                options.runGeneratedHighLightReloadStress ||
                options.runResizeStress ||
                options.runRenderGraphReloadStress ||
                options.runFrameSmokeTest)
            {
                options.errorMessage = "--reloadstress cannot be combined with another runtime test.";
                return options;
            }
            if (i + 1 >= argc || IsOptionToken(argv[i + 1]))
            {
                options.errorMessage = "--reloadstress requires a scene path.";
                return options;
            }

            options.runReloadStress = true;
            options.reloadStressScenePath = argv[++i];
            if (i + 1 < argc && !IsOptionToken(argv[i + 1]))
            {
                const std::string countText = argv[++i];
                if (!TryParsePositiveInt(countText, options.reloadStressCount))
                {
                    options.errorMessage = "--reloadstress count must be a positive integer.";
                    return options;
                }
            }
            continue;
        }

        if (argument == "--reloadfail")
        {
            if (options.runReloadStress ||
                options.runGeneratedMaterialFailureRollbackTest ||
                options.runGeneratedHighLightReloadStress ||
                options.runResizeStress ||
                options.runRenderGraphReloadStress ||
                options.runFrameSmokeTest)
            {
                options.errorMessage = "--reloadfail cannot be combined with another runtime test.";
                return options;
            }
            if (i + 1 >= argc || IsOptionToken(argv[i + 1]))
            {
                options.errorMessage = "--reloadfail requires a scene path that is expected to fail.";
                return options;
            }

            options.runReloadFailureRollbackTest = true;
            options.reloadFailureScenePath = argv[++i];
            continue;
        }

        if (argument == "--reloadfail-material")
        {
            if (options.runReloadStress ||
                options.runReloadFailureRollbackTest ||
                options.runGeneratedHighLightReloadStress ||
                options.runResizeStress ||
                options.runRenderGraphReloadStress ||
                options.runFrameSmokeTest)
            {
                options.errorMessage = "--reloadfail-material cannot be combined with another runtime test.";
                return options;
            }

            options.runGeneratedMaterialFailureRollbackTest = true;
            continue;
        }

        if (argument == "--lightstress")
        {
            if (options.runReloadStress ||
                options.runReloadFailureRollbackTest ||
                options.runGeneratedMaterialFailureRollbackTest ||
                options.runResizeStress ||
                options.runRenderGraphReloadStress ||
                options.runFrameSmokeTest)
            {
                options.errorMessage = "--lightstress cannot be combined with another runtime test.";
                return options;
            }

            options.runGeneratedHighLightReloadStress = true;
            if (i + 1 < argc && !IsOptionToken(argv[i + 1]))
            {
                const std::string countText = argv[++i];
                if (!TryParsePositiveInt(countText, options.highLightStressCount))
                {
                    options.errorMessage = "--lightstress count must be a positive integer.";
                    return options;
                }
            }
            continue;
        }

        if (argument == "--resizestress")
        {
            if (options.runReloadStress ||
                options.runReloadFailureRollbackTest ||
                options.runGeneratedMaterialFailureRollbackTest ||
                options.runGeneratedHighLightReloadStress ||
                options.runRenderGraphReloadStress ||
                options.runFrameSmokeTest)
            {
                options.errorMessage = "--resizestress cannot be combined with another runtime test.";
                return options;
            }

            options.runResizeStress = true;
            if (i + 1 < argc && !IsOptionToken(argv[i + 1]))
            {
                const std::string countText = argv[++i];
                if (!TryParsePositiveInt(countText, options.resizeStressCount))
                {
                    options.errorMessage = "--resizestress count must be a positive integer.";
                    return options;
                }
            }
            continue;
        }

        if (argument == "--graphreloadstress")
        {
            if (options.runReloadStress ||
                options.runReloadFailureRollbackTest ||
                options.runGeneratedMaterialFailureRollbackTest ||
                options.runGeneratedHighLightReloadStress ||
                options.runResizeStress ||
                options.runFrameSmokeTest)
            {
                options.errorMessage = "--graphreloadstress cannot be combined with another runtime test.";
                return options;
            }

            options.runRenderGraphReloadStress = true;
            if (i + 1 < argc && !IsOptionToken(argv[i + 1]))
            {
                const std::string countText = argv[++i];
                if (!TryParsePositiveInt(countText, options.graphReloadStressCount))
                {
                    options.errorMessage = "--graphreloadstress count must be a positive integer.";
                    return options;
                }
            }
            continue;
        }

        if (argument == "--framesmoke")
        {
            if (options.runReloadStress ||
                options.runReloadFailureRollbackTest ||
                options.runGeneratedMaterialFailureRollbackTest ||
                options.runGeneratedHighLightReloadStress ||
                options.runResizeStress ||
                options.runRenderGraphReloadStress)
            {
                options.errorMessage = "--framesmoke cannot be combined with another runtime test.";
                return options;
            }

            options.runFrameSmokeTest = true;
            if (i + 1 < argc && !IsOptionToken(argv[i + 1]))
            {
                const std::string countText = argv[++i];
                if (!TryParsePositiveInt(countText, options.frameSmokeCount))
                {
                    options.errorMessage = "--framesmoke count must be a positive integer.";
                    return options;
                }
            }
            continue;
        }

        options.errorMessage = "Unknown launch option: " + argument;
        return options;
    }

    if (options.exitAfterTests &&
        !options.runReloadStress &&
        !options.runReloadFailureRollbackTest &&
        !options.runGeneratedMaterialFailureRollbackTest &&
        !options.runGeneratedHighLightReloadStress &&
        !options.runResizeStress &&
        !options.runRenderGraphReloadStress &&
        !options.runFrameSmokeTest)
    {
        options.errorMessage = "--exit-after-tests requires --reloadstress, --reloadfail, --reloadfail-material, --lightstress, --resizestress, --graphreloadstress, or --framesmoke.";
    }
    return options;
}

void PrintUsage()
{
    std::cout
        << "Usage: main.exe [--reloadstress <scene-path> [count] --exit-after-tests]\n"
        << "       main.exe [--reloadfail <scene-path> --exit-after-tests]\n"
        << "       main.exe [--reloadfail-material --exit-after-tests]\n"
        << "       main.exe [--lightstress [count] --exit-after-tests]\n"
        << "       main.exe [--resizestress [count] --exit-after-tests]\n"
        << "       main.exe [--graphreloadstress [count] --exit-after-tests]\n"
        << "       main.exe [--framesmoke [count] --exit-after-tests]\n"
        << "  --reloadstress <scene-path> [count]  Queue world reload stress through CommandBus.\n"
        << "  --reloadfail <scene-path>            Verify failed reload preserves active World.\n"
        << "  --reloadfail-material                Generate bad material fixture and verify rollback.\n"
        << "  --lightstress [count]                Generate a high-light scene and reload it to test light SSBO growth.\n"
        << "  --resizestress [count]               Recreate swapchain and graph resources across window sizes.\n"
        << "  --graphreloadstress [count]          Reload frame graph GPU resources through retire queue.\n"
        << "  --framesmoke [count]                 Render fixed frames and report frame-time baseline.\n"
        << "  --exit-after-tests                  Exit with 0 on success or 2 on test failure.\n";
}

void QueueLaunchCommands(VL::EngineLoop& engineLoop, const LaunchOptions& options)
{
    if (options.runReloadFailureRollbackTest)
    {
        VL::RuntimeCommand command;
        command.type = VL::RuntimeCommandType::RunWorldReloadFailureRollbackTest;
        command.stringValue = options.reloadFailureScenePath;
        command.sourceText = "argv: --reloadfail";
        engineLoop.QueueRuntimeCommand(std::move(command));
        engineLoop.SetExitAfterRuntimeTests(options.exitAfterTests);
        return;
    }

    if (options.runGeneratedMaterialFailureRollbackTest)
    {
        VL::RuntimeCommand command;
        command.type = VL::RuntimeCommandType::RunGeneratedMaterialFailureRollbackTest;
        command.sourceText = "argv: --reloadfail-material";
        engineLoop.QueueRuntimeCommand(std::move(command));
        engineLoop.SetExitAfterRuntimeTests(options.exitAfterTests);
        return;
    }

    if (options.runReloadStress)
    {
        VL::RuntimeCommand command;
        command.type = VL::RuntimeCommandType::RunWorldReloadStress;
        command.stringValue = options.reloadStressScenePath;
        command.intValue = options.reloadStressCount;
        command.sourceText = "argv: --reloadstress";
        engineLoop.QueueRuntimeCommand(std::move(command));
        engineLoop.SetExitAfterRuntimeTests(options.exitAfterTests);
        return;
    }

    if (options.runGeneratedHighLightReloadStress)
    {
        VL::RuntimeCommand command;
        command.type = VL::RuntimeCommandType::RunGeneratedHighLightReloadStress;
        command.intValue = options.highLightStressCount;
        command.sourceText = "argv: --lightstress";
        engineLoop.QueueRuntimeCommand(std::move(command));
        engineLoop.SetExitAfterRuntimeTests(options.exitAfterTests);
        return;
    }

    if (options.runResizeStress)
    {
        engineLoop.StartResizeStress(options.resizeStressCount);
        engineLoop.SetExitAfterRuntimeTests(options.exitAfterTests);
        return;
    }

    if (options.runRenderGraphReloadStress)
    {
        engineLoop.StartRenderGraphReloadStress(options.graphReloadStressCount);
        engineLoop.SetExitAfterRuntimeTests(options.exitAfterTests);
        return;
    }

    if (options.runFrameSmokeTest)
    {
        engineLoop.StartFrameSmokeTest(options.frameSmokeCount);
        engineLoop.SetExitAfterRuntimeTests(options.exitAfterTests);
    }
}

} // namespace

int main(int argc, char **argv)
{
    const LaunchOptions launchOptions = ParseLaunchOptions(argc, argv);
    if (launchOptions.showHelp)
    {
        PrintUsage();
        return 0;
    }
    if (!launchOptions.errorMessage.empty())
    {
        std::cerr << launchOptions.errorMessage << std::endl;
        PrintUsage();
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

    QueueLaunchCommands(engineLoop, launchOptions);
    const int exitCode = engineLoop.Run();
    engineLoop.Shutdown();
    platformApplication.Shutdown();
    return exitCode;
}
