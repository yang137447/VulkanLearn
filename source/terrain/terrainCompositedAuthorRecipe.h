#pragma once

#include "terrain/terrainCompositedAdjacency.h"

#include <nlohmann/json.hpp>
#include <set>

namespace VL
{
// 仅解析 Editor/Cook 作者配方；返回 P1 的邻接类型，不定义第二份运行时发布协议。
TerrainCompositedAdjacencyGraph ParseTerrainCompositedAuthorRecipe(
    const nlohmann::json& document, const std::set<std::string>& placements);
}
