#include "engine/consoleSubsystem.h"

#include <utility>

#include "debugConsole.h"
#include "engine/runtimeCommand.h"

namespace VL
{

ConsoleSubsystem::ConsoleSubsystem() = default;
ConsoleSubsystem::~ConsoleSubsystem() = default;

RuntimeResult<void> ConsoleSubsystem::Initialize(CommandBus& commandBus,
    Editor::MaterialInstanceEditorRuntime* editorRuntime)
{
    debugConsole = std::make_unique<DebugConsole>(commandBus, editorRuntime);
    debugConsole->Initialize();
    return RuntimeResult<void>::Success();
}

void ConsoleSubsystem::Update()
{
    if (!debugConsole)
    {
        return;
    }

    // 启动脚本在画面稳定后每帧提交一条。命令之间需要跨帧生效的场景
    //（例如 particlespawn 后立刻 particlestats）因此能按参数顺序观察到
    // 上一帧结果；提交顺序仍与命令行完全一致。
    if (!pendingLaunchCommands.empty())
    {
        if (elapsedFrames >= launchCommandDelayFrames)
        {
            debugConsole->SubmitCommandLine(
                pendingLaunchCommands.front());
            pendingLaunchCommands.erase(
                pendingLaunchCommands.begin());
        }
        ++elapsedFrames;
    }

    debugConsole->Update();
}

void ConsoleSubsystem::QueueLaunchCommands(std::vector<std::string> lines, int delayFrames)
{
    if (!debugConsole)
    {
        return;
    }

    pendingLaunchCommands = std::move(lines);
    launchCommandDelayFrames = delayFrames < 0 ? 0 : delayFrames;
    elapsedFrames = 0;
}

} // namespace VL
