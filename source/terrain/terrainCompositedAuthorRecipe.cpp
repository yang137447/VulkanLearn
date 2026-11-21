#include "terrain/terrainCompositedAuthorRecipe.h"

#include <stdexcept>

namespace VL
{
namespace
{
void Fields(const nlohmann::json& value, std::initializer_list<const char*> fields)
{
    if (!value.is_object() || value.size() != fields.size())
        throw std::invalid_argument("Invalid terrain World author recipe fields");
    for (const auto* field : fields)
        if (!value.contains(field))
            throw std::invalid_argument("Missing terrain World author recipe field");
}

TerrainCompositedSurfaceEdge Edge(const nlohmann::json& value)
{
    const auto text = value.get<std::string>();
    if (text == "minU") return TerrainCompositedSurfaceEdge::MinU;
    if (text == "maxU") return TerrainCompositedSurfaceEdge::MaxU;
    if (text == "minV") return TerrainCompositedSurfaceEdge::MinV;
    if (text == "maxV") return TerrainCompositedSurfaceEdge::MaxV;
    throw std::invalid_argument("Unknown terrain World author edge");
}
}

TerrainCompositedAdjacencyGraph ParseTerrainCompositedAuthorRecipe(
    const nlohmann::json& document, const std::set<std::string>& placements)
{
    Fields(document, {"kind", "version", "surfaces", "adjacency"});
    if (document.at("kind") != "terrain-rvt-world-author" ||
        !document.at("version").is_number_integer() || document.at("version") != 1 ||
        placements.empty() || placements.size() > 256)
        throw std::invalid_argument("Invalid terrain World author recipe version or surface count");
    const auto ids = document.at("surfaces").get<std::vector<std::string>>();
    const std::set<std::string> unique(ids.begin(), ids.end());
    if (unique != placements || unique.size() != ids.size())
        throw std::invalid_argument("Terrain World author surface set differs from placements");
    if (!document.at("adjacency").is_array())
        throw std::invalid_argument("Terrain World author adjacency must be an array");
    std::vector<TerrainCompositedAdjacencyLink> links;
    for (const auto& value : document.at("adjacency"))
    {
        Fields(value, {"sourceSurfaceId", "targetSurfaceId", "sourceEdge", "targetEdge",
            "sourceEdgeInterval", "targetEdgeInterval", "sourceUvToTargetUv"});
        TerrainCompositedAdjacencyLink link;
        link.sourceSurfaceId = value.at("sourceSurfaceId").get<std::string>();
        link.targetSurfaceId = value.at("targetSurfaceId").get<std::string>();
        link.sourceEdge = Edge(value.at("sourceEdge"));
        link.targetEdge = Edge(value.at("targetEdge"));
        link.sourceEdgeInterval = value.at("sourceEdgeInterval").get<std::array<double, 2>>();
        link.targetEdgeInterval = value.at("targetEdgeInterval").get<std::array<double, 2>>();
        link.sourceUvToTargetUv = value.at("sourceUvToTargetUv").get<std::array<double, 6>>();
        links.push_back(std::move(link));
    }
    TerrainCompositedAdjacencyGraph result(ids, std::move(links));
    result.Validate(ids);
    return result;
}
}
