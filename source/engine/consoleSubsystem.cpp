#include "engine/consoleSubsystem.h"

#include <cctype>
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

    // 启动脚本只在画面稳定后提交一次。计时按 Update() 调用次数（即渲染帧）推进，
    // 提交顺序与命令行给出的顺序一致，保证同一组参数产生同一份截图。
    if (!pendingLaunchCommands.empty())
    {
        for (auto iterator = pendingLaunchCommands.begin();
             iterator != pendingLaunchCommands.end();)
        {
            if (elapsedFrames >= iterator->frame)
            {
                debugConsole->SubmitCommandLine(iterator->line);
                iterator = pendingLaunchCommands.erase(iterator);
            }
            else
            {
                ++iterator;
            }
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
