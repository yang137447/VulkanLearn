#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace VL
{

enum class TerrainCompositedSurfaceEdge { MinU, MaxU, MinV, MaxV };

struct TerrainCompositedAdjacencyLink
{
    std::string sourceSurfaceId;
    std::string targetSurfaceId;
    TerrainCompositedSurfaceEdge sourceEdge = TerrainCompositedSurfaceEdge::MaxU;
    TerrainCompositedSurfaceEdge targetEdge = TerrainCompositedSurfaceEdge::MinU;
    std::array<double, 2> sourceEdgeInterval{0, 1};
    std::array<double, 2> targetEdgeInterval{0, 1};
    std::array<double, 6> sourceUvToTargetUv{1, 0, 0, 0, 1, 0};
};

struct TerrainCompositedAdjacencyAddress
{
    std::string surfaceId;
    std::array<double, 2> uv{};
    std::array<double, 4> originalUvToResolvedUvJacobian{};
    uint32_t hops = 0;
    bool clampedAtWorldBoundary = false;
};

class TerrainCompositedAdjacencyGraph
{
public:
    struct StaticChart
    {
        size_t component = 0;
        std::array<double, 6> toComponent{1, 0, 0, 0, 1, 0};
    };
    TerrainCompositedAdjacencyGraph() = default;
    TerrainCompositedAdjacencyGraph(
        std::vector<std::string> surfaceIds,
        std::vector<TerrainCompositedAdjacencyLink> links);

    const std::vector<std::string>& SurfaceIds() const { return surfaceIds; }
    const std::vector<TerrainCompositedAdjacencyLink>& Links() const { return links; }
    const std::vector<StaticChart>& StaticCharts() const { return charts; }
    void Validate(const std::vector<std::string>& expectedSurfaceIds) const;
    TerrainCompositedAdjacencyAddress Resolve(
        const std::string& surfaceId, const std::array<double, 2>& uv) const;

private:
    void Build();
    std::vector<std::string> surfaceIds;
    std::vector<TerrainCompositedAdjacencyLink> links;
    std::vector<StaticChart> charts;
};

}
