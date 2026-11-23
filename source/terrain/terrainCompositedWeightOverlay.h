#pragma once

#include "terrain/terrainAssetTypes.h"

namespace VL
{
HostImage BuildTerrainCompositedWeightOverlay(
    const TerrainBuildPlan& plan,
    const HostImage& importedWeights,
    const std::vector<uint8_t>& localToGlobal);
}
