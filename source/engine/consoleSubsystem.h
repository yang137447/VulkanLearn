#pragma once

#include <memory>
#include <string>
#include <vector>

#include "core/runtimeResult.h"

class DebugConsole;

namespace VL
{

class CommandBus;

// Owns the interactive debug console as an engine subsystem. The console only
// publishes commands; command execution is drained by EngineLoop.
class ConsoleSubsystem
{
public:
    ConsoleSubsystem();
    ~ConsoleSubsystem();

    RuntimeResult<void> Initialize(CommandBus& commandBus);
    void Update();

    // 启动参数携带的控制台脚本。首帧时环境贴图、CLUT 与 swapchain 内容都还没稳定，
    // 立即执行 screenshot 只会采到无效画面，因此延迟 delayFrames 帧后再按原顺序
    // 一次性提交。提交走 DebugConsole::SubmitCommandLine，与手输命令同一条解析路径。
    void QueueLaunchCommands(std::vector<std::string> lines, int delayFrames);

private:
    std::unique_ptr<DebugConsole> debugConsole;
    std::vector<std::string> pendingLaunchCommands;
    int launchCommandDelayFrames = 0;
    int elapsedFrames = 0;
};

} // namespace VL
