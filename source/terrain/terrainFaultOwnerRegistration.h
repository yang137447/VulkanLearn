#pragma once

#include "terrain/terrainFaultInjection.h"

namespace VL
{
struct TerrainFaultOwnerRegistration
{
    uint64_t faultStatusHook = 0;
    uint64_t faultSubmitHook = 0;
    uint64_t faultSlotHook = 0;
    uint64_t faultStaleHook = 0;
    uint64_t faultBudgetHook = 0;

    bool Activate(TerrainFaultInjectionController& controller,
        uint64_t worldGeneration, TerrainFaultSlotHooks hooks);
    void Deactivate(TerrainFaultInjectionController& controller) noexcept;
    bool Complete() const noexcept;
};
}
