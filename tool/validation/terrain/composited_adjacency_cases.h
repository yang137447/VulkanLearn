#pragma once

#include "render/resource/terrainCompositedBakeAdjacency.h"
#include <limits>

namespace TerrainBakeRouteCases
{
using namespace VL;

inline std::vector<TerrainCompositedAdjacencyGraph> Graphs()
{
    std::vector<std::string> ids;
    std::vector<TerrainCompositedAdjacencyLink> links;
    const auto horizontal = [&](const std::string& first, const std::string& second) {
        links.push_back({first, second, TerrainCompositedSurfaceEdge::MaxU,
            TerrainCompositedSurfaceEdge::MinU, {0, 1}, {0, 1}, {1, 0, -1, 0, 1, 0}});
        links.push_back({second, first, TerrainCompositedSurfaceEdge::MinU,
            TerrainCompositedSurfaceEdge::MaxU, {0, 1}, {0, 1}, {1, 0, 1, 0, 1, 0}});
    };
    const auto vertical = [&](const std::string& first, const std::string& second) {
        links.push_back({first, second, TerrainCompositedSurfaceEdge::MaxV,
            TerrainCompositedSurfaceEdge::MinV, {0, 1}, {0, 1}, {1, 0, 0, 0, 1, -1}});
        links.push_back({second, first, TerrainCompositedSurfaceEdge::MinV,
            TerrainCompositedSurfaceEdge::MaxV, {0, 1}, {0, 1}, {1, 0, 0, 0, 1, 1}});
    };
    for (uint32_t index = 0; index < 16; ++index)
        ids.push_back("tile" + std::to_string(index));
    for (uint32_t row = 0; row < 4; ++row)
        for (uint32_t column = 0; column < 4; ++column)
        {
            if (column < 3)
                horizontal(ids[row * 4 + column], ids[row * 4 + column + 1]);
            if (row < 3)
                vertical(ids[row * 4 + column], ids[(row + 1) * 4 + column]);
        }
    std::vector<TerrainCompositedAdjacencyGraph> result;
    ids.push_back("disconnected");
    result.emplace_back(ids, links);
    result.emplace_back(std::vector<std::string>{"a", "b", "c"},
        std::vector<TerrainCompositedAdjacencyLink>{
            {"a", "b", TerrainCompositedSurfaceEdge::MaxU, TerrainCompositedSurfaceEdge::MinU,
                {0, .5}, {0, 1}, {1, 0, -1, 0, 2, 0}},
            {"b", "a", TerrainCompositedSurfaceEdge::MinU, TerrainCompositedSurfaceEdge::MaxU,
                {0, 1}, {0, .5}, {1, 0, 1, 0, .5, 0}},
            {"a", "c", TerrainCompositedSurfaceEdge::MaxU, TerrainCompositedSurfaceEdge::MinU,
                {.5, 1}, {0, 1}, {1, 0, -1, 0, 2, -1}},
            {"c", "a", TerrainCompositedSurfaceEdge::MinU, TerrainCompositedSurfaceEdge::MaxU,
                {0, 1}, {.5, 1}, {1, 0, 1, 0, .5, .5}},
            {"b", "c", TerrainCompositedSurfaceEdge::MaxV, TerrainCompositedSurfaceEdge::MinV,
                {0, 1}, {0, 1}, {1, 0, 0, 0, 1, -1}},
            {"c", "b", TerrainCompositedSurfaceEdge::MinV, TerrainCompositedSurfaceEdge::MaxV,
                {0, 1}, {0, 1}, {1, 0, 0, 0, 1, 1}}});
    result.emplace_back(std::vector<std::string>{"a", "b"},
        std::vector<TerrainCompositedAdjacencyLink>{
            {"a", "b", TerrainCompositedSurfaceEdge::MaxU, TerrainCompositedSurfaceEdge::MinV,
                {0, 1}, {0, 1}, {0, 1, 0, 1, 0, -1}},
            {"b", "a", TerrainCompositedSurfaceEdge::MinV, TerrainCompositedSurfaceEdge::MaxU,
                {0, 1}, {0, 1}, {0, 1, 1, 1, 0, 0}}});
    result.emplace_back(std::vector<std::string>{"a", "b"},
        std::vector<TerrainCompositedAdjacencyLink>{
            {"a", "b", TerrainCompositedSurfaceEdge::MaxU, TerrainCompositedSurfaceEdge::MinU,
                {0, 1}, {0, 1}, {1, 0, -1, 0, -1, 1}},
            {"b", "a", TerrainCompositedSurfaceEdge::MinU, TerrainCompositedSurfaceEdge::MaxU,
                {0, 1}, {0, 1}, {1, 0, 1, 0, -1, 1}}});
    return result;
}

inline std::vector<std::array<float, 2>> Queries()
{
    std::vector<std::array<float, 2>> result;
    const float below = std::nextafter(1.f, 0.f);
    const float above = std::nextafter(1.f, 2.f);
    for (float vertical : {-4.f, -1.f, -0.f, 0.f, .5f, below, 1.f, above, 2.f, 4.f, 132.f})
        for (float horizontal : {-4.f, -1.f, -0.f, 0.f, .5f, below, 1.f, above, 2.f, 4.f, 132.f})
            result.push_back({horizontal, vertical});
    for (uint32_t extent : {1u, 2u, 7u, 128u, 8192u})
        for (uint32_t vertical = 0; vertical < 136; ++vertical)
            for (uint32_t horizontal = 0; horizontal < 136; ++horizontal)
                result.push_back({(float(horizontal) - 3.875f) / extent,
                    (float(vertical) - 3.875f) / extent});
    return result;
}
}
