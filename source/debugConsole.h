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

private:
    void HandleInput(int ch);
    void ProcessCommand(const std::string& line);
    void PrintHelp() const;
    void PrintShaderReloadHelp() const;
    void PrintPrompt() const;

    VL::CommandBus& commandBus;
    std::string currentLine;
};
