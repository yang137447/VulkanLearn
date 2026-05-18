#pragma once
#include <string>

class RenderSystem;

class DebugConsole
{
public:
    explicit DebugConsole(RenderSystem& renderSystem);

    void Initialize();
    void Update();

private:
    void HandleInput(int ch);
    void ProcessCommand(const std::string& line);
    void PrintHelp() const;
    void PrintPrompt() const;

    RenderSystem& renderSystem;
    std::string currentLine;
};
