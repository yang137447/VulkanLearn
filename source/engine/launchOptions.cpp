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
    GeneratedMeshFailureRollback,
    GeneratedTextureFailureRollback,
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

std::string GetRuntimeTestOptionName(RuntimeTestOption option)
{
    switch (option)
    {
    case RuntimeTestOption::ReloadStress:
        return "--reloadstress";
    case RuntimeTestOption::ReloadFailureRollback:
        return "--reloadfail";
    case RuntimeTestOption::GeneratedMaterialFailureRollback:
        return "--reloadfail-material";
    case RuntimeTestOption::GeneratedMeshFailureRollback:
        return "--reloadfail-mesh";
    case RuntimeTestOption::GeneratedTextureFailureRollback:
        return "--reloadfail-texture";
    case RuntimeTestOption::GeneratedHighLightReloadStress:
        return "--lightstress";
    case RuntimeTestOption::ResizeStress:
        return "--resizestress";
    case RuntimeTestOption::RenderGraphReloadStress:
        return "--graphreloadstress";
    case RuntimeTestOption::FrameSmoke:
        return "--framesmoke";
    case RuntimeTestOption::None:
        break;
    }

    return {};
}

RuntimeTestOption FindRuntimeTest(const LaunchOptions& options, RuntimeTestOption excludedOption)
{
    if (excludedOption != RuntimeTestOption::ReloadStress && options.runReloadStress)
    {
        return RuntimeTestOption::ReloadStress;
    }
    if (excludedOption != RuntimeTestOption::ReloadFailureRollback &&
        options.runReloadFailureRollbackTest)
    {
        return RuntimeTestOption::ReloadFailureRollback;
    }
    if (excludedOption != RuntimeTestOption::GeneratedMaterialFailureRollback &&
        options.runGeneratedMaterialFailureRollbackTest)
    {
        return RuntimeTestOption::GeneratedMaterialFailureRollback;
    }
    if (excludedOption != RuntimeTestOption::GeneratedMeshFailureRollback &&
        options.runGeneratedMeshFailureRollbackTest)
    {
        return RuntimeTestOption::GeneratedMeshFailureRollback;
    }
    if (excludedOption != RuntimeTestOption::GeneratedTextureFailureRollback &&
        options.runGeneratedTextureFailureRollbackTest)
    {
        return RuntimeTestOption::GeneratedTextureFailureRollback;
    }
    if (excludedOption != RuntimeTestOption::GeneratedHighLightReloadStress &&
        options.runGeneratedHighLightReloadStress)
    {
        return RuntimeTestOption::GeneratedHighLightReloadStress;
    }
    if (excludedOption != RuntimeTestOption::ResizeStress && options.runResizeStress)
    {
        return RuntimeTestOption::ResizeStress;
    }
    if (excludedOption != RuntimeTestOption::RenderGraphReloadStress &&
        options.runRenderGraphReloadStress)
    {
        return RuntimeTestOption::RenderGraphReloadStress;
    }
    if (excludedOption != RuntimeTestOption::FrameSmoke && options.runFrameSmokeTest)
    {
        return RuntimeTestOption::FrameSmoke;
    }

    return RuntimeTestOption::None;
}

bool HasRuntimeTest(const LaunchOptions& options, RuntimeTestOption excludedOption)
{
    return FindRuntimeTest(options, excludedOption) != RuntimeTestOption::None;
}

bool RejectRuntimeTestConflict(
    LaunchOptions& options,
    const std::string& optionName,
    RuntimeTestOption currentOption)
{
    const RuntimeTestOption existingOption = FindRuntimeTest(options, currentOption);
    if (existingOption == RuntimeTestOption::None)
    {
        return false;
    }

    options.errorMessage =
        optionName +
        " cannot be combined with " +
        GetRuntimeTestOptionName(existingOption) +
        ". Runtime tests must run one at a time because shader/SPIR-V outputs are shared.";
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

        if (argument == "--dev-ui")
        {
            options.developerUiMode = DeveloperUiLaunchMode::Enabled;
            continue;
        }

        if (argument == "--no-dev-ui")
        {
            options.developerUiMode = DeveloperUiLaunchMode::Disabled;
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

        if (argument == "--reloadfail-mesh")
        {
            if (RejectRuntimeTestConflict(
                options,
                "--reloadfail-mesh",
                RuntimeTestOption::GeneratedMeshFailureRollback))
            {
                return options;
            }

            options.runGeneratedMeshFailureRollbackTest = true;
            continue;
        }

        if (argument == "--reloadfail-texture")
        {
            if (RejectRuntimeTestConflict(
                options,
                "--reloadfail-texture",
                RuntimeTestOption::GeneratedTextureFailureRollback))
            {
                return options;
            }

            options.runGeneratedTextureFailureRollbackTest = true;
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
        options.errorMessage = "--exit-after-tests requires --reloadstress, --reloadfail, --reloadfail-material, --reloadfail-mesh, --reloadfail-texture, --lightstress, --resizestress, --graphreloadstress, or --framesmoke.";
    }
    return options;
}

void PrintLaunchUsage()
{
    std::cout
        << "Usage: main.exe [--reloadstress <scene-path> [count] --exit-after-tests]\n"
        << "       main.exe [--reloadfail <scene-path> --exit-after-tests]\n"
        << "       main.exe [--reloadfail-material --exit-after-tests]\n"
        << "       main.exe [--reloadfail-mesh --exit-after-tests]\n"
        << "       main.exe [--reloadfail-texture --exit-after-tests]\n"
        << "       main.exe [--lightstress [count] --exit-after-tests]\n"
        << "       main.exe [--resizestress [count] --exit-after-tests]\n"
        << "       main.exe [--graphreloadstress [count] --exit-after-tests]\n"
        << "       main.exe [--framesmoke [count] --exit-after-tests]\n"
        << "  --reloadstress <scene-path> [count]  Queue world reload stress through CommandBus.\n"
        << "  --reloadfail <scene-path>            Verify failed reload preserves active World.\n"
        << "  --reloadfail-material                Generate bad material fixture and verify rollback.\n"
        << "  --reloadfail-mesh                    Generate bad mesh fixture and verify rollback.\n"
        << "  --reloadfail-texture                 Generate bad texture fixture and verify rollback.\n"
        << "  --lightstress [count]                Generate a high-light scene and reload it to test light SSBO growth.\n"
        << "  --resizestress [count]               Recreate swapchain and graph resources across window sizes.\n"
        << "  --graphreloadstress [count]          Reload frame graph GPU resources through retire queue.\n"
        << "  --framesmoke [count]                 Render fixed frames and report frame-time baseline.\n"
        << "  --dev-ui                            Enable Dear ImGui developer tools for this launch.\n"
        << "  --no-dev-ui                         Disable Dear ImGui developer tools for this launch.\n"
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

    if (options.runGeneratedMeshFailureRollbackTest)
    {
        RuntimeCommand command;
        command.type = RuntimeCommandType::RunGeneratedMeshFailureRollbackTest;
        command.sourceText = "argv: --reloadfail-mesh";
        engineLoop.QueueRuntimeCommand(std::move(command));
        engineLoop.SetExitAfterRuntimeTests(options.exitAfterTests);
        return;
    }

    if (options.runGeneratedTextureFailureRollbackTest)
    {
        RuntimeCommand command;
        command.type = RuntimeCommandType::RunGeneratedTextureFailureRollbackTest;
        command.sourceText = "argv: --reloadfail-texture";
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
        RuntimeCommand command;
        command.type = RuntimeCommandType::RunResizeStress;
        command.intValue = options.resizeStressCount;
        command.sourceText = "argv: --resizestress";
        engineLoop.QueueRuntimeCommand(std::move(command));
        engineLoop.SetExitAfterRuntimeTests(options.exitAfterTests);
        return;
    }

    if (options.runRenderGraphReloadStress)
    {
        RuntimeCommand command;
        command.type = RuntimeCommandType::RunRenderGraphReloadStress;
        command.intValue = options.graphReloadStressCount;
        command.sourceText = "argv: --graphreloadstress";
        engineLoop.QueueRuntimeCommand(std::move(command));
        engineLoop.SetExitAfterRuntimeTests(options.exitAfterTests);
        return;
    }

    if (options.runFrameSmokeTest)
    {
        RuntimeCommand command;
        command.type = RuntimeCommandType::RunFrameSmokeTest;
        command.intValue = options.frameSmokeCount;
        command.sourceText = "argv: --framesmoke";
        engineLoop.QueueRuntimeCommand(std::move(command));
        engineLoop.SetExitAfterRuntimeTests(options.exitAfterTests);
    }
}

} // namespace VL
