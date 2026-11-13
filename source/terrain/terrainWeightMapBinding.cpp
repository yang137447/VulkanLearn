#include "terrain/terrainWeightMapBinding.h"

#include <algorithm>
#include <utility>

namespace VL
{

void TerrainWeightMapBindingTracker::Record(
    const void* weightsIdentity,
    std::string materialPath,
    std::string placementId)
{
    auto binding = std::find_if(
        bindings.begin(),
        bindings.end(),
        [weightsIdentity](const Binding& entry)
        {
            return entry.weightsIdentity == weightsIdentity;
        });
    if (binding == bindings.end())
    {
        Binding entry;
        entry.weightsIdentity = weightsIdentity;
        entry.materialPath = std::move(materialPath);
        entry.placements.push_back(std::move(placementId));
        bindings.push_back(std::move(entry));
        return;
    }

    if (std::find(
            binding->placements.begin(),
            binding->placements.end(),
            placementId) == binding->placements.end())
    {
        binding->placements.push_back(std::move(placementId));
    }
}

std::string TerrainWeightMapBindingTracker::SharedBindingError() const
{
    for (const Binding& binding : bindings)
    {
        if (binding.placements.size() <= 1)
            continue;
        std::string placements;
        for (const std::string& placement : binding.placements)
        {
            if (!placements.empty())
                placements += ", ";
            placements += placement;
        }
        return "Terrain u_terrainWeightMapMode=1 requires one material "
            "instance per terrain placement; layerWeightMaps of '" +
            binding.materialPath + "' is shared by " +
            std::to_string(binding.placements.size()) + " placements (" +
            placements +
            "). Use the world page-table weights (set "
            "u_terrainWeightMapMode=0) or give every placement its own "
            "layerWeightMaps texture.";
    }
    return std::string();
}

void TerrainWeightMapBindingTracker::Clear()
{
    bindings.clear();
}

size_t TerrainWeightMapBindingTracker::PlacementCount() const
{
    size_t count = 0;
    for (const Binding& binding : bindings)
        count += binding.placements.size();
    return count;
}

} // namespace VL
