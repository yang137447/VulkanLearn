#include "engine/runtimeCommand.h"

#include <utility>

namespace VL
{

void CommandBus::Queue(RuntimeCommand command)
{
    pendingCommands.push_back(std::move(command));
}

void CommandBus::Queue(UiAction action)
{
    pendingUiActions.push_back(std::move(action));
}

std::vector<RuntimeCommand> CommandBus::Drain()
{
    std::vector<RuntimeCommand> commands;
    commands.swap(pendingCommands);
    return commands;
}

std::vector<UiAction> CommandBus::DrainUiActions()
{
    std::vector<UiAction> actions;
    actions.swap(pendingUiActions);
    return actions;
}

} // namespace VL
