#include "engine/consoleSubsystem.h"

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
    if (debugConsole)
    {
        debugConsole->Update();
    }
}

} // namespace VL
