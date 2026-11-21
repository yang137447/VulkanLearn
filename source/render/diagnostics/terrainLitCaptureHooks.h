#pragma once
#include <memory>

namespace VL
{
struct PassRuntimeContext;
class TerrainRvtBudgetOwner;
class ITerrainLitCaptureHooks
{
public:
    virtual ~ITerrainLitCaptureHooks() = default;
    virtual void AfterActualPass(PassRuntimeContext& actualContext) = 0;
    virtual void SetReferencePoolOwner(std::shared_ptr<TerrainRvtBudgetOwner> admittedOwner) = 0;
};
void RecordOptionalTerrainLitAfterPass(PassRuntimeContext& actualContext);
}
