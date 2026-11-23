#pragma once

#include "terrain/terrainCompositedAdjacency.h"
#include "terrain/terrainCompositedVT.h"

#include <array>
#include <vector>

namespace VL
{

struct TerrainCompositedEditDirty
{
    TerrainCompositedVTUVRect rect;
    uint64_t revision = 0;
    bool valid = false;

    void Include(const TerrainCompositedVTUVRect& changed, uint64_t changedRevision);
    bool Acknowledge(uint64_t consumedRevision);
};

enum class TerrainCompositedDependencyChange
{
    Paint,
    Sculpt,
    BaseMaterial,
    BaseSource,
    LayerMapping,
    WeightSource,
    Placement,
    Frame,
    Filter,
    Generator,
    HeightBlend,
    TilingHeight,
    LayerDisplacement,
    DynamicDetail,
    DynamicMacro,
    DynamicGlobal,
    DynamicTessellation
};

struct TerrainCompositedDirtyConsumers
{
    bool basePages = false;
    bool dynamicMaterial = false;
    bool tilingHeight = false;
    bool geometry = false;
    bool collision = false;
};

TerrainCompositedDirtyConsumers ClassifyTerrainCompositedDependencyChange(
    TerrainCompositedDependencyChange change,
    bool staticHeightBlendBefore = false, bool staticHeightBlendAfter = false);

struct TerrainCompositedDirtyChange
{
    std::string surfaceId;
    TerrainCompositedDependencyChange kind = TerrainCompositedDependencyChange::Paint;
    TerrainCompositedVTUVRect rect{0, 0, 1, 1};
    std::array<double, 2> inputSupportUv{};
    bool staticHeightBlendBefore = false;
    bool staticHeightBlendAfter = false;
};

struct TerrainCompositedDirtySurface
{
    std::string surfaceId;
    TerrainCompositedDirtyConsumers consumers;
};

struct TerrainCompositedDirtyResult
{
    std::vector<TerrainCompositedVTPageKey> pages;
    std::vector<TerrainCompositedDirtySurface> surfaces;
};

class TerrainCompositedDirtyPropagation
{
public:
    TerrainCompositedDirtyPropagation(
        TerrainCompositedAdjacencyGraph adjacency,
        std::vector<TerrainCompositedVTLayout> layouts);

    TerrainCompositedDirtyResult Collect(
        const std::vector<TerrainCompositedDirtyChange>& changes,
        double filterRadiusTexels = 1) const;

private:
    struct Mapping
    {
        size_t target = 0;
        std::array<double, 6> transform{1, 0, 0, 0, 1, 0};
    };

    TerrainCompositedAdjacencyGraph adjacency;
    std::vector<TerrainCompositedVTLayout> layouts;
    std::vector<std::vector<Mapping>> mappings;
};

}
