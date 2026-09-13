#pragma once
#include <string>

namespace VL
{
class CommandBus;
}

class DebugConsole
{
public:
    explicit DebugConsole(VL::CommandBus& commandBus);

    void Initialize();
    void Update();

    // 启动期脚本化入口。交互路径靠 _kbhit/_getch 读取控制台键盘缓冲，无法被
    // 自动化驱动；这里复用同一条 ProcessCommand 通路，保证脚本命令与手输命令
    // 的解析、校验和日志行为完全一致。
    void SubmitCommandLine(const std::string& line);

private:
    void HandleInput(int ch);
    void ProcessCommand(const std::string& line);
    void PrintHelp() const;
    void PrintShaderReloadHelp() const;
    void PrintPrompt() const;

    VL::CommandBus& commandBus;
    std::string currentLine;
};
