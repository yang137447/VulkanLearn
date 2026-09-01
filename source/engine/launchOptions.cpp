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
    ShaderReloadTest,
    ShaderComputeReloadTest,
    WorldGraphTransactionTest
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
    case RuntimeTestOption::ShaderReloadTest:
        return "--shader-reload-test";
    case RuntimeTestOption::ShaderComputeReloadTest:
        return "--shader-compute-reload-test";
    case RuntimeTestOption::WorldGraphTransactionTest:
        return "--world-graph-transaction-test";
    case RuntimeTestOption::None:
        break;
    }

    return {};
}

RuntimeTestOption FindRuntimeTest(const LaunchOptions& options, RuntimeTestOption excludedOption)
{
    if (excludedOption != RuntimeTestOption::ShaderReloadTest &&
        options.runShaderReloadTest)
    {
        return RuntimeTestOption::ShaderReloadTest;
    }
    if (excludedOption != RuntimeTestOption::ShaderComputeReloadTest &&
        options.runShaderComputeReloadTest)
    {
        return RuntimeTestOption::ShaderComputeReloadTest;
    }
    if (excludedOption != RuntimeTestOption::WorldGraphTransactionTest &&
        options.runWorldGraphTransactionTest)
    {
        return RuntimeTestOption::WorldGraphTransactionTest;
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

        options.errorMessage = "Unknown launch option: " + argument;
        return options;
    }

    if (options.exitAfterTests &&
        !HasRuntimeTest(options, RuntimeTestOption::None))
    {
        options.errorMessage = "--exit-after-tests requires --shader-reload-test, --shader-compute-reload-test, or --world-graph-transaction-test.";
    }
    return options;
}

void PrintLaunchUsage()
{
    std::cout
        << "       main.exe [--shader-reload-test --exit-after-tests]\n"
        << "       main.exe [--shader-compute-reload-test --exit-after-tests]\n"
        << "       main.exe [--world-graph-transaction-test --exit-after-tests]\n"
        << "       main.exe [--shader-force-rebuild]\n"
        << "       main.exe [--initial-scene <scene-path>]\n"
        << "       main.exe [--worker-thread-count <1|2>]\n"
        << "  --shader-force-rebuild              Recompile and republish every startup shader artifact.\n"
        << "  --shader-reload-test                Run the graphics shader hot-reload rollback matrix.\n"
        << "  --shader-compute-reload-test        Run the compute pipeline reload participant matrix.\n"
        << "  --world-graph-transaction-test      Run deterministic World/RenderGraph transaction rollback faults.\n"
        << "  --initial-scene <scene-path>        Override config initScene for this process.\n"
        << "  --worker-thread-count <1|2>         Override config worker mode for this process.\n"
        << "  --dev-ui                            Enable Dear ImGui developer tools for this launch.\n"
        << "  --no-dev-ui                         Disable Dear ImGui developer tools for this launch.\n"
        << "  --exit-after-tests                  Exit with 0 on success or 2 on test failure.\n";
}

void QueueLaunchCommands(EngineLoop& engineLoop, const LaunchOptions& options)
{
    if (options.runShaderReloadTest)
    {
        RuntimeCommand command;
        command.type = RuntimeCommandType::RunShaderReloadTest;
        command.sourceText = "argv: --shader-reload-test";
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

}
} // namespace VL
