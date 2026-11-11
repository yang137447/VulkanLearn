#include "engine/consoleSubsystem.h"

#include <algorithm>
#include <cctype>
#include <utility>

#include "debugConsole.h"
#include "engine/runtimeCommand.h"

namespace VL
{

ConsoleSubsystem::ConsoleSubsystem() = default;
ConsoleSubsystem::~ConsoleSubsystem() = default;

RuntimeResult<void> ConsoleSubsystem::Initialize(CommandBus& commandBus,
    IEditorConsoleSink* editorConsoleSink)
{
    debugConsole = std::make_unique<DebugConsole>(commandBus, editorConsoleSink);
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
        const auto ready = std::find_if(
            pendingLaunchCommands.begin(), pendingLaunchCommands.end(),
            [this](const ScheduledLaunchCommand& command)
            {
                return elapsedFrames >= command.frame;
            });
        if (ready != pendingLaunchCommands.end())
        {
            debugConsole->SubmitCommandLine(ready->line);
            pendingLaunchCommands.erase(ready);
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

    pendingLaunchCommands.clear();
    pendingLaunchCommands.reserve(lines.size());
    for (const std::string& line : lines)
    {
        ScheduledLaunchCommand command;
        command.frame = delayFrames < 0 ? 0 : delayFrames;
        command.line = line;
        // "@120 screenshot a.bmp"：该条命令在第 120 帧提交，其余命令沿用统一延迟。
        if (!line.empty() && line.front() == '@')
        {
            size_t offset = 1;
            int frame = 0;
            while (offset < line.size() &&
                   std::isdigit(static_cast<unsigned char>(line[offset])) != 0)
            {
                frame = frame * 10 + (line[offset] - '0');
                ++offset;
            }
            if (offset > 1)
            {
                command.frame = frame;
                while (offset < line.size() && line[offset] == ' ')
                {
                    ++offset;
                }
                command.line = line.substr(offset);
            }
        }
        pendingLaunchCommands.push_back(std::move(command));
    }
    launchCommandDelayFrames = delayFrames < 0 ? 0 : delayFrames;
    elapsedFrames = 0;
}

} // namespace VL
