#include "render/diagnostics/terrainLitCaptureHooks.h"
#include "render/diagnostics/terrainCaptureHooks.h"

namespace VL
{
void RecordOptionalTerrainLitAfterPass(PassRuntimeContext& actualContext)
{
    const auto hooks = AcquireTerrainCaptureHooks();
    if (auto* lit = dynamic_cast<ITerrainLitCaptureHooks*>(hooks.get()))
        lit->AfterActualPass(actualContext);
}
}
