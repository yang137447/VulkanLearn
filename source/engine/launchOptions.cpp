#include "engine/launchOptions.h"

#include <iostream>
#include <utility>

#include "engine/engineLoop.h"
#include "engine/runtimeCommand.h"

namespace VL
{

namespace
{

enum class RuntimeTestOption
{
    None,
    ReloadStress,
    ReloadFailureRollback,
    GeneratedMaterialFailureRollback,
    GeneratedHighLightReloadStress,
    ResizeStress,
    RenderGraphReloadStress,
    FrameSmoke
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

bool HasRuntimeTest(const LaunchOptions& options, RuntimeTestOption excludedOption)
{
    if (excludedOption != RuntimeTestOption::ReloadStress && options.runReloadStress)
    {
        return true;
    }
    if (excludedOption != RuntimeTestOption::ReloadFailureRollback &&
        options.runReloadFailureRollbackTest)
    {
        return true;
    }
    if (excludedOption != RuntimeTestOption::GeneratedMaterialFailureRollback &&
        options.runGeneratedMaterialFailureRollbackTest)
    {
        return true;
    }
    if (excludedOption != RuntimeTestOption::GeneratedHighLightReloadStress &&
        options.runGeneratedHighLightReloadStress)
    {
        return true;
    }
    if (excludedOption != RuntimeTestOption::ResizeStress && options.runResizeStress)
    {
        return true;
    }
    if (excludedOption != RuntimeTestOption::RenderGraphReloadStress &&
        options.runRenderGraphReloadStress)
    {
        return true;
    }
    if (excludedOption != RuntimeTestOption::FrameSmoke && options.runFrameSmokeTest)
    {
        return true;
    }

    return false;
}

bool RejectRuntimeTestConflict(
    LaunchOptions& options,
    const std::string& optionName,
    RuntimeTestOption currentOption)
{
    if (!HasRuntimeTest(options, currentOption))
    {
        return false;
    }

    options.errorMessage = optionName + " cannot be combined with another runtime test.";
    return true;
}

bool TryConsumeOptionalPositiveInt(
    int argc,
    char** argv,
    int& index,
    int& value,
    const std::string& optionName,
    LaunchOptions& options)
{
    if (index + 1 >= argc || IsOptionToken(argv[index + 1]))
    {
        return true;
    }

    const std::string countText = argv[++index];
    if (TryParsePositiveInt(countText, value))
    {
        return true;
    }

    options.errorMessage = optionName + " count must be a positive integer.";
    return false;
}

} // namespace

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
            if (RejectRuntimeTestConflict(
                options,
                "--reloadstress",
                RuntimeTestOption::ReloadStress))
            {
                return options;
            }
            if (i + 1 >= argc || IsOptionToken(argv[i + 1]))
            {
                options.errorMessage = "--reloadstress requires a scene path.";
                return options;
            }

            options.runReloadStress = true;
            options.reloadStressScenePath = argv[++i];
            if (!TryConsumeOptionalPositiveInt(
                argc,
                argv,
                i,
                options.reloadStressCount,
                "--reloadstress",
                options))
            {
                return options;
            }
            continue;
        }

        if (argument == "--reloadfail")
        {
            if (RejectRuntimeTestConflict(
                options,
                "--reloadfail",
                RuntimeTestOption::ReloadFailureRollback))
            {
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
            if (RejectRuntimeTestConflict(
                options,
                "--reloadfail-material",
                RuntimeTestOption::GeneratedMaterialFailureRollback))
            {
                return options;
            }

            options.runGeneratedMaterialFailureRollbackTest = true;
            continue;
        }

        if (argument == "--lightstress")
        {
            if (RejectRuntimeTestConflict(
                options,
                "--lightstress",
                RuntimeTestOption::GeneratedHighLightReloadStress))
            {
                return options;
            }

            options.runGeneratedHighLightReloadStress = true;
            if (!TryConsumeOptionalPositiveInt(
                argc,
                argv,
                i,
                options.highLightStressCount,
                "--lightstress",
                options))
            {
                return options;
            }
            continue;
        }

        if (argument == "--resizestress")
        {
            if (RejectRuntimeTestConflict(
                options,
                "--resizestress",
                RuntimeTestOption::ResizeStress))
            {
                return options;
            }

            options.runResizeStress = true;
            if (!TryConsumeOptionalPositiveInt(
                argc,
                argv,
                i,
                options.resizeStressCount,
                "--resizestress",
                options))
            {
                return options;
            }
            continue;
        }

        if (argument == "--graphreloadstress")
        {
            if (RejectRuntimeTestConflict(
                options,
                "--graphreloadstress",
                RuntimeTestOption::RenderGraphReloadStress))
            {
                return options;
            }

            options.runRenderGraphReloadStress = true;
            if (!TryConsumeOptionalPositiveInt(
                argc,
                argv,
                i,
                options.graphReloadStressCount,
                "--graphreloadstress",
                options))
            {
                return options;
            }
            continue;
        }

        if (argument == "--framesmoke")
        {
            if (RejectRuntimeTestConflict(
                options,
                "--framesmoke",
                RuntimeTestOption::FrameSmoke))
            {
                return options;
            }

            options.runFrameSmokeTest = true;
            if (!TryConsumeOptionalPositiveInt(
                argc,
                argv,
                i,
                options.frameSmokeCount,
                "--framesmoke",
                options))
            {
                return options;
            }
            continue;
        }

        options.errorMessage = "Unknown launch option: " + argument;
        return options;
    }

    if (options.exitAfterTests &&
        !HasRuntimeTest(options, RuntimeTestOption::None))
    {
        options.errorMessage = "--exit-after-tests requires --reloadstress, --reloadfail, --reloadfail-material, --lightstress, --resizestress, --graphreloadstress, or --framesmoke.";
    }
    return options;
}

void PrintLaunchUsage()
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

void QueueLaunchCommands(EngineLoop& engineLoop, const LaunchOptions& options)
{
    if (options.runReloadFailureRollbackTest)
    {
        RuntimeCommand command;
        command.type = RuntimeCommandType::RunWorldReloadFailureRollbackTest;
        command.stringValue = options.reloadFailureScenePath;
        command.sourceText = "argv: --reloadfail";
        engineLoop.QueueRuntimeCommand(std::move(command));
        engineLoop.SetExitAfterRuntimeTests(options.exitAfterTests);
        return;
    }

    if (options.runGeneratedMaterialFailureRollbackTest)
    {
        RuntimeCommand command;
        command.type = RuntimeCommandType::RunGeneratedMaterialFailureRollbackTest;
        command.sourceText = "argv: --reloadfail-material";
        engineLoop.QueueRuntimeCommand(std::move(command));
        engineLoop.SetExitAfterRuntimeTests(options.exitAfterTests);
        return;
    }

    if (options.runReloadStress)
    {
        RuntimeCommand command;
        command.type = RuntimeCommandType::RunWorldReloadStress;
        command.stringValue = options.reloadStressScenePath;
        command.intValue = options.reloadStressCount;
        command.sourceText = "argv: --reloadstress";
        engineLoop.QueueRuntimeCommand(std::move(command));
        engineLoop.SetExitAfterRuntimeTests(options.exitAfterTests);
        return;
    }

    if (options.runGeneratedHighLightReloadStress)
    {
        RuntimeCommand command;
        command.type = RuntimeCommandType::RunGeneratedHighLightReloadStress;
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

} // namespace VL
