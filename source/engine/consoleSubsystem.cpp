#include "engine/consoleSubsystem.h"

#include <utility>

#include "debugConsole.h"
#include "engine/runtimeCommand.h"

namespace VL
{

ConsoleSubsystem::ConsoleSubsystem() = default;
ConsoleSubsystem::~ConsoleSubsystem() = default;

RuntimeResult<void> ConsoleSubsystem::Initialize(CommandBus& commandBus)
{
    debugConsole = std::make_unique<DebugConsole>(commandBus);
    debugConsole->Initialize();
    return RuntimeResult<void>::Success();
}

void ConsoleSubsystem::Update()
{
    if (!debugConsole)
    {
        return;
    }

    // 启动脚本只在画面稳定后提交一次。计时按 Update() 调用次数（即渲染帧）推进，
    // 提交顺序与命令行给出的顺序一致，保证同一组参数产生同一份截图。
    if (!pendingLaunchCommands.empty())
    {
        if (elapsedFrames >= launchCommandDelayFrames)
        {
            for (const std::string& line : pendingLaunchCommands)
            {
                debugConsole->SubmitCommandLine(line);
            }
            pendingLaunchCommands.clear();
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
