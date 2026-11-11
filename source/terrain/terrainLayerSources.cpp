#include "terrain/terrainLayerSources.h"

#include <algorithm>

namespace VL
{

uint32_t TerrainSourceSetRecord::MinMipLevels() const
{
    return std::min({albedoMipLevels, normalMipLevels, pbrMipLevels});
}

bool TerrainSourceSetRecord::HasTextures() const
{
    return albedo && normal && pbr;
}

bool TerrainSourceSetRecord::TryGetLayerSlice(
    uint32_t globalLayerId,
    uint32_t& slice) const
{
    const auto found = layerSlices.find(globalLayerId);
    if (found == layerSlices.end())
        return false;
    slice = found->second;
    return true;
}

const TerrainSourceSetRecord* FindTerrainSourceSet(
    const TerrainLayerSourceSets& sources,
    uint32_t sourceSet)
{
    const auto found = sources.find(sourceSet);
    return found == sources.end() ? nullptr : &found->second;
}

uint32_t TerrainSourceMinMipLevels(const TerrainLayerSourceSets& sources)
{
    uint32_t minimum = 0;
    bool first = true;
    for (const auto& entry : sources)
    {
        if (!entry.second.HasTextures())
            continue;
        const uint32_t mipLevels = entry.second.MinMipLevels();
        minimum = first ? mipLevels : std::min(minimum, mipLevels);
        first = false;
    }
    return first ? 0 : minimum;
}

uint32_t TerrainSourceMipLevelsForLayer(
    const TerrainLayerSourceSets& sources,
    uint32_t sourceSet)
{
    const TerrainSourceSetRecord* record =
        FindTerrainSourceSet(sources, sourceSet);
    return record == nullptr ? 0 : record->MinMipLevels();
}

} // namespace VL
