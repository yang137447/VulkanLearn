#pragma once

#include "mesh/meshAssetTypes.h"
#include "hair/tressfxDynamics.h"

namespace VL
{
namespace RenderableDetail
{
// CPU reference solver configuration derived from an authored placement
// config. Wind stays zero here; AdvanceTressfxSimulation writes the scene wind
// immediately before StepFrame.
Hair::SimulationConfig MakeCpuSimulationConfig(const TressfxSimulationConfig& config);
} // namespace RenderableDetail
} // namespace VL