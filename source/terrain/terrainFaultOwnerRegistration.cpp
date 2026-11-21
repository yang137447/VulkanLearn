#include "terrain/terrainFaultOwnerRegistration.h"
#include "core/runtimeEvidence.h"

#include <exception>
#include <utility>

namespace VL
{
bool TerrainFaultOwnerRegistration::Complete() const noexcept
{
    return faultStatusHook && faultSubmitHook && faultSlotHook &&
        faultStaleHook && faultBudgetHook;
}

bool TerrainFaultOwnerRegistration::Activate(TerrainFaultInjectionController& controller,
    uint64_t worldGeneration, TerrainFaultSlotHooks hooks)
{
    if (Complete())
        return true;
    Deactivate(controller);
    if (!worldGeneration)
        return false;
    try
    {
        faultStatusHook = controller.RegisterStatusHook(worldGeneration);
        faultSubmitHook = controller.RegisterSubmitHook(worldGeneration);
        faultSlotHook = controller.RegisterSlotHook(worldGeneration, std::move(hooks));
        faultStaleHook = controller.RegisterActionHook(worldGeneration, TerrainFaultKind::StaleRevision);
        faultBudgetHook = controller.RegisterActionHook(worldGeneration, TerrainFaultKind::BudgetReject);
        if (Complete())
            return true;
    }
    catch (...)
    {
        Deactivate(controller);
        throw;
    }
    Deactivate(controller);
    return false;
}

void TerrainFaultOwnerRegistration::Deactivate(TerrainFaultInjectionController& controller) noexcept
{
    if (faultSlotHook)
    {
        try { controller.UnregisterSlotHook(faultSlotHook); }
        catch (...)
        {
            if (!RuntimeEvidence::HasIncompleteSession())
                std::terminate();
        }
    }
    if (faultStaleHook)
        controller.UnregisterActionHook(faultStaleHook);
    if (faultBudgetHook)
        controller.UnregisterActionHook(faultBudgetHook);
    if (faultSubmitHook)
        controller.UnregisterSubmitHook(faultSubmitHook);
    if (faultStatusHook)
        controller.UnregisterStatusHook(faultStatusHook);
    faultStatusHook = faultSubmitHook = faultSlotHook = faultStaleHook = faultBudgetHook = 0;
}
}
