#define Recompute RecomputeV2Implementation
#include "render/resource/terrainCompositedQualityData.h"
#undef Recompute

namespace VL
{
namespace TerrainQualityAbi
{
Counts Recompute(const uint32_t* words, size_t bytes)
{
    return RecomputeV2Implementation(words, bytes);
}
}
}
