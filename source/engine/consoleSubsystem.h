#pragma once

#include <memory>

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

private:
    std::unique_ptr<DebugConsole> debugConsole;
};

} // namespace VL
