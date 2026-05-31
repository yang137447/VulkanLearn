#include "engine/runtimeCommand.h"

#include <utility>

namespace VL
{

void CommandBus::Queue(RuntimeCommand command)
{
    pendingCommands.push_back(std::move(command));
}

std::vector<RuntimeCommand> CommandBus::Drain()
{
    std::vector<RuntimeCommand> commands;
    commands.swap(pendingCommands);
    return commands;
}

} // namespace VL
