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
    FrameSmoke,
    EnvironmentUpdateStress,
    ShaderReloadTest,
    ShaderAutoReloadTest,
    ShaderComputeReloadTest,
    ShaderDefinitionReloadTest,
    WorldGraphTransactionTest,
    ShaderUiReloadTest,
    ShaderShutdownInflightTest
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
    case RuntimeTestOption::EnvironmentUpdateStress:
        return "--environmentstress";
    case RuntimeTestOption::ShaderReloadTest:
        return "--shader-reload-test";
    case RuntimeTestOption::ShaderAutoReloadTest:
        return "--shader-auto-reload-test";
    case RuntimeTestOption::ShaderComputeReloadTest:
        return "--shader-compute-reload-test";
    case RuntimeTestOption::ShaderDefinitionReloadTest:
        return "--shader-definition-reload-test";
    case RuntimeTestOption::WorldGraphTransactionTest:
        return "--world-graph-transaction-test";
    case RuntimeTestOption::ShaderUiReloadTest:
        return "--shader-ui-reload-test";
    case RuntimeTestOption::ShaderShutdownInflightTest:
        return "--shader-shutdown-inflight-test";
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
    if (excludedOption != RuntimeTestOption::EnvironmentUpdateStress &&
        options.runEnvironmentUpdateStress)
    {
        return RuntimeTestOption::EnvironmentUpdateStress;
    }
    if (excludedOption != RuntimeTestOption::ShaderReloadTest &&
        options.runShaderReloadTest)
    {
        return RuntimeTestOption::ShaderReloadTest;
    }
    if (excludedOption != RuntimeTestOption::ShaderAutoReloadTest &&
        options.runShaderAutoReloadTest)
    {
        return RuntimeTestOption::ShaderAutoReloadTest;
    }
    if (excludedOption != RuntimeTestOption::ShaderComputeReloadTest &&
        options.runShaderComputeReloadTest)
    {
        return RuntimeTestOption::ShaderComputeReloadTest;
    }
    if (excludedOption != RuntimeTestOption::ShaderDefinitionReloadTest &&
        options.runShaderDefinitionReloadTest)
    {
        return RuntimeTestOption::ShaderDefinitionReloadTest;
    }
    if (excludedOption != RuntimeTestOption::WorldGraphTransactionTest &&
        options.runWorldGraphTransactionTest)
    {
        return RuntimeTestOption::WorldGraphTransactionTest;
    }
    if (excludedOption != RuntimeTestOption::ShaderUiReloadTest &&
        options.runShaderUiReloadTest)
    {
        return RuntimeTestOption::ShaderUiReloadTest;
    }
    if (excludedOption !=
            RuntimeTestOption::ShaderShutdownInflightTest &&
        options.runShaderShutdownInflightTest)
    {
        return RuntimeTestOption::ShaderShutdownInflightTest;
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

        if (argument == "--shader-force-rebuild")
        {
            options.forceShaderRebuild = true;
            continue;
        }

        if (argument == "--worker-thread-count")
        {
            if (i + 1 >= argc || IsOptionToken(argv[i + 1]))
            {
                options.errorMessage =
                    "--worker-thread-count requires 1 or 2.";
                return options;
            }

            int workerThreadCount = 0;
            const std::string countText = argv[++i];
            if (!TryParsePositiveInt(
                    countText,
                    workerThreadCount) ||
                (workerThreadCount != 1 &&
                 workerThreadCount != 2))
            {
                options.errorMessage =
                    "--worker-thread-count must be 1 or 2.";
                return options;
            }
            options.workerThreadCountOverride =
                workerThreadCount;
            continue;
        }

        if (argument == "--initial-scene")
        {
            if (i + 1 >= argc || IsOptionToken(argv[i + 1]))
            {
                options.errorMessage =
                    "--initial-scene requires a scene path.";
                return options;
            }
            options.initialSceneOverride = argv[++i];
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

        if (argument == "--environmentstress")
        {
            if (RejectRuntimeTestConflict(
                options,
                "--environmentstress",
                RuntimeTestOption::EnvironmentUpdateStress))
            {
                return options;
            }

            options.runEnvironmentUpdateStress = true;
            if (!TryConsumeOptionalPositiveInt(
                argc,
                argv,
                i,
                options.environmentUpdateStressCount,
                "--environmentstress",
                options))
            {
                return options;
            }
            continue;
        }

        if (argument == "--shader-reload-test")
        {
            if (RejectRuntimeTestConflict(
                options,
                "--shader-reload-test",
                RuntimeTestOption::ShaderReloadTest))
            {
                return options;
            }

            options.runShaderReloadTest = true;
            continue;
        }

        if (argument == "--shader-auto-reload-test")
        {
            if (RejectRuntimeTestConflict(
                options,
                "--shader-auto-reload-test",
                RuntimeTestOption::ShaderAutoReloadTest))
            {
                return options;
            }

            options.runShaderAutoReloadTest = true;
            continue;
        }

        if (argument == "--shader-compute-reload-test")
        {
            if (RejectRuntimeTestConflict(
                options,
                "--shader-compute-reload-test",
                RuntimeTestOption::ShaderComputeReloadTest))
            {
                return options;
            }

            options.runShaderComputeReloadTest = true;
            continue;
        }

        if (argument == "--shader-definition-reload-test")
        {
            if (RejectRuntimeTestConflict(
                options,
                "--shader-definition-reload-test",
                RuntimeTestOption::ShaderDefinitionReloadTest))
            {
                return options;
            }

            options.runShaderDefinitionReloadTest = true;
            continue;
        }

        if (argument == "--world-graph-transaction-test")
        {
            if (RejectRuntimeTestConflict(
                options,
                "--world-graph-transaction-test",
                RuntimeTestOption::WorldGraphTransactionTest))
            {
                return options;
            }

            options.runWorldGraphTransactionTest = true;
            continue;
        }

        if (argument == "--shader-ui-reload-test")
        {
            if (RejectRuntimeTestConflict(
                options,
                "--shader-ui-reload-test",
                RuntimeTestOption::ShaderUiReloadTest))
            {
                return options;
            }

            options.runShaderUiReloadTest = true;
            continue;
        }

        if (argument == "--shader-shutdown-inflight-test")
        {
            if (RejectRuntimeTestConflict(
                options,
                "--shader-shutdown-inflight-test",
                RuntimeTestOption::ShaderShutdownInflightTest))
            {
                return options;
            }

            options.runShaderShutdownInflightTest = true;
            continue;
        }

        options.errorMessage = "Unknown launch option: " + argument;
        return options;
    }

    if (options.exitAfterTests &&
        !HasRuntimeTest(options, RuntimeTestOption::None))
    {
        options.errorMessage = "--exit-after-tests requires --reloadstress, --reloadfail, --reloadfail-material, --reloadfail-mesh, --reloadfail-texture, --lightstress, --resizestress, --graphreloadstress, --framesmoke, --environmentstress, --shader-reload-test, --shader-auto-reload-test, --shader-compute-reload-test, --shader-definition-reload-test, --world-graph-transaction-test, --shader-ui-reload-test, or --shader-shutdown-inflight-test.";
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
        << "       main.exe [--environmentstress [count] --exit-after-tests]\n"
        << "       main.exe [--shader-reload-test --exit-after-tests]\n"
        << "       main.exe [--shader-auto-reload-test --exit-after-tests]\n"
        << "       main.exe [--shader-compute-reload-test --exit-after-tests]\n"
        << "       main.exe [--shader-definition-reload-test --exit-after-tests]\n"
        << "       main.exe [--world-graph-transaction-test --exit-after-tests]\n"
        << "       main.exe [--shader-ui-reload-test --exit-after-tests]\n"
        << "       main.exe [--shader-shutdown-inflight-test --exit-after-tests]\n"
        << "       main.exe [--shader-force-rebuild]\n"
        << "       main.exe [--initial-scene <scene-path>]\n"
        << "       main.exe [--worker-thread-count <1|2>]\n"
        << "  --reloadstress <scene-path> [count]  Queue world reload stress through CommandBus.\n"
        << "  --reloadfail <scene-path>            Verify failed reload preserves active World.\n"
        << "  --reloadfail-material                Generate bad material fixture and verify rollback.\n"
        << "  --reloadfail-mesh                    Generate bad mesh fixture and verify rollback.\n"
        << "  --reloadfail-texture                 Generate bad texture fixture and verify rollback.\n"
        << "  --lightstress [count]                Generate a high-light scene and reload it to test light SSBO growth.\n"
        << "  --resizestress [count]               Recreate swapchain and graph resources across window sizes.\n"
        << "  --graphreloadstress [count]          Reload frame graph GPU resources through retire queue.\n"
        << "  --framesmoke [count]                 Render fixed frames and report frame-time baseline.\n"
        << "  --environmentstress [count]          Change procedural sky inputs and verify incremental IBL generations.\n"
        << "  --shader-reload-test                Run the graphics shader hot-reload rollback matrix.\n"
        << "  --shader-auto-reload-test           Run the FileMonitor + compile-worker auto reload matrix.\n"
        << "  --shader-compute-reload-test        Run the compute pipeline reload participant matrix.\n"
        << "  --shader-definition-reload-test     Run the M_*.json schema rebuild/rollback matrix.\n"
        << "  --world-graph-transaction-test      Run deterministic World/RenderGraph transaction rollback faults.\n"
        << "  --shader-ui-reload-test             Run the UI Overlay pipeline pair reload matrix.\n"
        << "  --shader-shutdown-inflight-test     Stop after a complete worker candidate and verify shutdown discards it.\n"
        << "  --shader-force-rebuild              Recompile and republish every startup shader artifact.\n"
        << "  --initial-scene <scene-path>        Override config initScene for this process.\n"
        << "  --worker-thread-count <1|2>         Override config worker mode for this process.\n"
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
        return;
    }

    if (options.runEnvironmentUpdateStress)
    {
        RuntimeCommand command;
        command.type = RuntimeCommandType::RunEnvironmentUpdateStress;
        command.intValue = options.environmentUpdateStressCount;
        command.sourceText = "argv: --environmentstress";
        engineLoop.QueueRuntimeCommand(std::move(command));
        engineLoop.SetExitAfterRuntimeTests(options.exitAfterTests);
        return;
    }

    if (options.runShaderReloadTest)
    {
        RuntimeCommand command;
        command.type = RuntimeCommandType::RunShaderReloadTest;
        command.sourceText = "argv: --shader-reload-test";
        engineLoop.QueueRuntimeCommand(std::move(command));
        engineLoop.SetExitAfterRuntimeTests(options.exitAfterTests);
        return;
    }

    if (options.runShaderAutoReloadTest)
    {
        RuntimeCommand command;
        command.type = RuntimeCommandType::RunShaderAutoReloadTest;
        command.sourceText = "argv: --shader-auto-reload-test";
        engineLoop.QueueRuntimeCommand(std::move(command));
        engineLoop.SetExitAfterRuntimeTests(options.exitAfterTests);
        return;
    }

    if (options.runShaderComputeReloadTest)
    {
        RuntimeCommand command;
        command.type = RuntimeCommandType::RunShaderComputeReloadTest;
        command.sourceText = "argv: --shader-compute-reload-test";
        engineLoop.QueueRuntimeCommand(std::move(command));
        engineLoop.SetExitAfterRuntimeTests(options.exitAfterTests);
        return;
    }

    if (options.runShaderDefinitionReloadTest)
    {
        RuntimeCommand command;
        command.type = RuntimeCommandType::RunShaderDefinitionReloadTest;
        command.sourceText = "argv: --shader-definition-reload-test";
        engineLoop.QueueRuntimeCommand(std::move(command));
        engineLoop.SetExitAfterRuntimeTests(options.exitAfterTests);
        return;
    }

    if (options.runWorldGraphTransactionTest)
    {
        RuntimeCommand command;
        command.type =
            RuntimeCommandType::RunWorldGraphTransactionTest;
        command.sourceText =
            "argv: --world-graph-transaction-test";
        engineLoop.QueueRuntimeCommand(std::move(command));
        engineLoop.SetExitAfterRuntimeTests(
            options.exitAfterTests);
        return;
    }

    if (options.runShaderUiReloadTest)
    {
        RuntimeCommand command;
        command.type = RuntimeCommandType::RunShaderUiReloadTest;
        command.sourceText = "argv: --shader-ui-reload-test";
        engineLoop.QueueRuntimeCommand(std::move(command));
        engineLoop.SetExitAfterRuntimeTests(options.exitAfterTests);
        return;
    }

    if (options.runShaderShutdownInflightTest)
    {
        RuntimeCommand command;
        command.type =
            RuntimeCommandType::RunShaderShutdownInflightTest;
        command.sourceText =
            "argv: --shader-shutdown-inflight-test";
        engineLoop.QueueRuntimeCommand(std::move(command));
        engineLoop.SetExitAfterRuntimeTests(
            options.exitAfterTests);
    }
}

} // namespace VL
