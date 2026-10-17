#pragma once

#include "engine/consoleSubsystem.h"
#include "engine/diagnosticsSubsystem.h"
#include "engine/runtimeCommand.h"
#include "engine/runtimeClock.h"
#include "engine/runtimeTestHooks.h"
#include "fpsTool.h"
#include "input/inputSubsystem.h"
#include "world/worldManager.h"
#include "script/scriptRuntime.h"

namespace VL
{

// Explicit owner for cross-world engine subsystems. This is intentionally a
// typed collection, not a generic service locator, so dependencies stay visible
// while EngineLoop sheds long-lived object ownership.
class SubsystemCollection
{
public:
    RuntimeClock& GetRuntimeClock() { return runtimeClock; }
    FpsTool& GetFpsTool() { return fpsTool; }
    CommandBus& GetCommandBus() { return commandBus; }
    ConsoleSubsystem& GetConsoleSubsystem() { return consoleSubsystem; }
    DiagnosticsSubsystem& GetDiagnosticsSubsystem() { return diagnosticsSubsystem; }
    RuntimeTestHooks& GetRuntimeTestHooks() { return runtimeTestHooks; }
    InputSubsystem& GetInputSubsystem() { return inputSubsystem; }
    WorldManager& GetWorldManager() { return worldManager; }
    ScriptRuntimeSubsystem& GetScriptRuntime() { return scriptRuntime; }

    const RuntimeClock& GetRuntimeClock() const { return runtimeClock; }
    const FpsTool& GetFpsTool() const { return fpsTool; }
    const CommandBus& GetCommandBus() const { return commandBus; }
    const ConsoleSubsystem& GetConsoleSubsystem() const { return consoleSubsystem; }
    const DiagnosticsSubsystem& GetDiagnosticsSubsystem() const { return diagnosticsSubsystem; }
    const RuntimeTestHooks& GetRuntimeTestHooks() const { return runtimeTestHooks; }
    const InputSubsystem& GetInputSubsystem() const { return inputSubsystem; }
    const WorldManager& GetWorldManager() const { return worldManager; }
    const ScriptRuntimeSubsystem& GetScriptRuntime() const { return scriptRuntime; }

private:
    RuntimeClock runtimeClock;
    FpsTool fpsTool;
    CommandBus commandBus;
    ConsoleSubsystem consoleSubsystem;
    DiagnosticsSubsystem diagnosticsSubsystem;
    RuntimeTestHooks runtimeTestHooks;
    InputSubsystem inputSubsystem;
    WorldManager worldManager;
    ScriptRuntimeSubsystem scriptRuntime;
};

} // namespace VL
