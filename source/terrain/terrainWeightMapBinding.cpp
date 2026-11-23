#include "terrain/terrainWeightMapBinding.h"

#include <algorithm>
#include <stdexcept>
#include <utility>

namespace VL
{

void TerrainWeightMapBindingTracker::Record(
    const void* weightsIdentity,
    std::string materialPath,
    std::string placementId,
    BindingPolicy policy)
{
    if (!weightsIdentity || materialPath.empty() || placementId.empty())
        throw std::invalid_argument("Terrain local weight binding requires texture, material and placement identities");
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
        entry.immutablePublicationOnly =
            policy == BindingPolicy::VerifiedImmutablePublication;
        bindings.push_back(std::move(entry));
        return;
    }

    binding->immutablePublicationOnly &=
        policy == BindingPolicy::VerifiedImmutablePublication;
    if (std::find(
            binding->placements.begin(),
            binding->placements.end(),
            placementId) == binding->placements.end())
    {
        binding->placements.push_back(std::move(placementId));
    }
}

void TerrainWeightMapBindingTracker::RecordFrozenOverlay(
    const void* importedWeightsIdentity,
    const void* frozenWeightsIdentity,
    std::string materialPath,
    std::string placementId)
{
    if (!importedWeightsIdentity || !frozenWeightsIdentity ||
        importedWeightsIdentity == frozenWeightsIdentity)
    {
        throw std::invalid_argument(
            "Terrain composited Local weights require the final per-placement "
            "overlay binding, not the immutable imported base: " + materialPath);
    }
    Record(frozenWeightsIdentity, std::move(materialPath), std::move(placementId));
}

std::string TerrainWeightMapBindingTracker::SharedBindingError() const
{
    for (const Binding& binding : bindings)
    {
        if (binding.placements.size() <= 1 || binding.immutablePublicationOnly)
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
