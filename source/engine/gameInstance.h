#pragma once

#include "engine/runtimeConfig.h"
#include "engine/subsystemCollection.h"

namespace VL
{

// Cross-world runtime context. GameInstance owns engine subsystems that outlive
// any single World, while RuntimeConfig remains an external startup contract
// supplied by main.
class GameInstance
{
public:
    explicit GameInstance(const RuntimeConfig& runtimeConfig);

    const RuntimeConfig& GetRuntimeConfig() const { return runtimeConfig; }
    SubsystemCollection& GetSubsystems() { return subsystems; }
    const SubsystemCollection& GetSubsystems() const { return subsystems; }

private:
    const RuntimeConfig& runtimeConfig;
    SubsystemCollection subsystems;
};

} // namespace VL
