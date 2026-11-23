#include "terrain/terrainCompositedDirty.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <map>
#include <queue>
#include <set>
#include <stdexcept>
#include <utility>

namespace VL
{
namespace
{
using Transform = std::array<double, 6>;
using Rect = TerrainCompositedVTUVRect;

void Require(bool condition, const char* diagnostic)
{
    if (!condition)
        throw std::runtime_error(std::string("Terrain composited dirty: ") + diagnostic);
}

Transform Compose(const Transform& outer, const Transform& inner)
{
    return {outer[0] * inner[0] + outer[1] * inner[3],
        outer[0] * inner[1] + outer[1] * inner[4],
        outer[0] * inner[2] + outer[1] * inner[5] + outer[2],
        outer[3] * inner[0] + outer[4] * inner[3],
        outer[3] * inner[1] + outer[4] * inner[4],
        outer[3] * inner[2] + outer[4] * inner[5] + outer[5]};
}

std::array<double, 2> Apply(const Transform& transform, double horizontal, double vertical)
{
    return {transform[0] * horizontal + transform[1] * vertical + transform[2],
        transform[3] * horizontal + transform[4] * vertical + transform[5]};
}

Rect Map(const Transform& transform, const Rect& rect)
{
    const auto first = Apply(transform, rect.minU, rect.minV);
    const auto last = Apply(transform, rect.maxU, rect.maxV);
    Require(std::isfinite(first[0]) && std::isfinite(first[1]) &&
        std::isfinite(last[0]) && std::isfinite(last[1]), "mapped rectangle overflow");
    return {std::min(first[0], last[0]), std::min(first[1], last[1]),
        std::max(first[0], last[0]), std::max(first[1], last[1])};
}

bool HasExternalBoundary(const TerrainCompositedAdjacencyGraph& graph,
    const std::string& surface, TerrainCompositedSurfaceEdge edge, double begin, double end)
{
    std::vector<std::array<double, 2>> intervals;
    for (const auto& link : graph.Links())
        if (link.sourceSurfaceId == surface && link.sourceEdge == edge)
            intervals.push_back(link.sourceEdgeInterval);
    std::sort(intervals.begin(), intervals.end());
    double covered = begin;
    for (const auto& interval : intervals)
    {
        if (interval[1] <= covered)
            continue;
        if (interval[0] > covered)
            return true;
        covered = interval[1];
        if (covered >= end)
            return false;
    }
    return covered < end;
}

void Extend(Rect& rect, double horizontal, double vertical)
{
    const double infinity = std::numeric_limits<double>::infinity();
    if (horizontal < 0)
        rect.minU = -infinity;
    if (horizontal > 0)
        rect.maxU = infinity;
    if (vertical < 0)
        rect.minV = -infinity;
    if (vertical > 0)
        rect.maxV = infinity;
}

void Merge(TerrainCompositedDirtyConsumers& target, const TerrainCompositedDirtyConsumers& source)
{
    target.basePages |= source.basePages;
    target.dynamicMaterial |= source.dynamicMaterial;
    target.tilingHeight |= source.tilingHeight;
    target.geometry |= source.geometry;
    target.collision |= source.collision;
}
}

void TerrainCompositedEditDirty::Include(const TerrainCompositedVTUVRect& changed, uint64_t changedRevision)
{
    Require(std::isfinite(changed.minU) && std::isfinite(changed.minV) &&
        std::isfinite(changed.maxU) && std::isfinite(changed.maxV) &&
        changed.minU <= changed.maxU && changed.minV <= changed.maxV,
        "invalid edit rectangle");
    Require(changedRevision >= revision, "edit revision moved backwards");
    if (changed.minU == changed.maxU || changed.minV == changed.maxV)
        return;
    if (!valid)
        rect = changed;
    else
        rect = {std::min(rect.minU, changed.minU), std::min(rect.minV, changed.minV),
            std::max(rect.maxU, changed.maxU), std::max(rect.maxV, changed.maxV)};
    revision = changedRevision;
    valid = true;
}

bool TerrainCompositedEditDirty::Acknowledge(uint64_t consumedRevision)
{
    if (!valid || consumedRevision != revision)
        return false;
    rect = {};
    valid = false;
    return true;
}

TerrainCompositedDirtyConsumers ClassifyTerrainCompositedDependencyChange(
    TerrainCompositedDependencyChange change, bool before, bool after)
{
    using Change = TerrainCompositedDependencyChange;
    switch (change)
    {
    case Change::Paint:
    case Change::WeightSource:
    case Change::LayerMapping:
        return {true, true, false, false, false};
    case Change::Sculpt:
        return {true, true, false, true, true};
    case Change::Placement:
        return {true, true, false, true, true};
    case Change::BaseMaterial:
    case Change::BaseSource:
    case Change::Frame:
    case Change::Filter:
    case Change::Generator:
    case Change::HeightBlend:
        return {true, true, false, false, false};
    case Change::TilingHeight:
        return {before || after, true, true, false, false};
    case Change::LayerDisplacement:
        return {before || after, true, false, false, false};
    case Change::DynamicDetail:
    case Change::DynamicMacro:
    case Change::DynamicGlobal:
    case Change::DynamicTessellation:
        return {false, true, false, false, false};
    }
    throw std::runtime_error("Terrain composited dirty: unknown dependency change");
}

TerrainCompositedDirtyPropagation::TerrainCompositedDirtyPropagation(
    TerrainCompositedAdjacencyGraph graph, std::vector<TerrainCompositedVTLayout> surfaceLayouts)
    : adjacency(std::move(graph)), layouts(std::move(surfaceLayouts))
{
    std::sort(layouts.begin(), layouts.end(), [](const auto& first, const auto& second) {
        return first.SurfaceId() < second.SurfaceId();
    });
    std::vector<std::string> ids;
    for (const auto& layout : layouts)
        ids.push_back(layout.SurfaceId());
    adjacency.Validate(ids);
    mappings.resize(layouts.size());
    for (size_t source = 0; source < layouts.size(); ++source)
    {
        std::vector<bool> visited(layouts.size(), false);
        std::queue<Mapping> pending;
        pending.push({source, {1, 0, 0, 0, 1, 0}});
        visited[source] = true;
        while (!pending.empty())
        {
            const auto current = pending.front();
            pending.pop();
            mappings[source].push_back(current);
            for (const auto& link : adjacency.Links())
            {
                if (link.sourceSurfaceId != ids[current.target])
                    continue;
                const size_t target = std::lower_bound(ids.begin(), ids.end(), link.targetSurfaceId) - ids.begin();
                Require(layouts[current.target].AddressMode() == TerrainCompositedVTAddressMode::Clamp &&
                    layouts[target].AddressMode() == TerrainCompositedVTAddressMode::Clamp,
                    "repeat layout cannot have explicit adjacency");
                if (visited[target])
                    continue;
                visited[target] = true;
                pending.push({target, Compose(link.sourceUvToTargetUv, current.transform)});
            }
        }
    }
}

TerrainCompositedDirtyResult TerrainCompositedDirtyPropagation::Collect(
    const std::vector<TerrainCompositedDirtyChange>& changes, double filterRadiusTexels) const
{
    Require(std::isfinite(filterRadiusTexels) && filterRadiusTexels >= 0 &&
        filterRadiusTexels <= TerrainCompositedVTLayout::MaxDimension, "invalid filter radius");
    std::set<TerrainCompositedVTPageKey> pages;
    std::map<std::string, TerrainCompositedDirtyConsumers> consumers;
    for (const auto& change : changes)
    {
        const auto found = std::lower_bound(layouts.begin(), layouts.end(), change.surfaceId,
            [](const auto& layout, const std::string& id) { return layout.SurfaceId() < id; });
        Require(found != layouts.end() && found->SurfaceId() == change.surfaceId, "unknown placement surface");
        const auto& input = change.rect;
        Require(std::isfinite(input.minU) && std::isfinite(input.minV) &&
            std::isfinite(input.maxU) && std::isfinite(input.maxV) &&
            input.minU <= input.maxU && input.minV <= input.maxV,
            "invalid dirty rectangle");
        for (const auto support : change.inputSupportUv)
            Require(std::isfinite(support) && support >= 0 && support <= 1, "invalid input support");
        const auto effect = ClassifyTerrainCompositedDependencyChange(
            change.kind, change.staticHeightBlendBefore, change.staticHeightBlendAfter);
        if (input.minU == input.maxU || input.minV == input.maxV)
            continue;
        Rect region{input.minU - change.inputSupportUv[0], input.minV - change.inputSupportUv[1],
            input.maxU + change.inputSupportUv[0], input.maxV + change.inputSupportUv[1]};
        if (found->AddressMode() == TerrainCompositedVTAddressMode::Repeat)
        {
            Merge(consumers[change.surfaceId], effect);
            if (effect.basePages)
            {
                const auto changed = found->DirtyPages(region,
                    std::min(filterRadiusTexels + 0.5, double(TerrainCompositedVTLayout::MaxDimension)));
                pages.insert(changed.begin(), changed.end());
            }
            continue;
        }
        region = {std::max(0., region.minU), std::max(0., region.minV),
            std::min(1., region.maxU), std::min(1., region.maxV)};
        if (region.minU >= region.maxU || region.minV >= region.maxV)
            continue;
        Merge(consumers[change.surfaceId], effect);
        if (!effect.basePages)
            continue;
        const auto external = [&](TerrainCompositedSurfaceEdge edge, double begin, double end) {
            return HasExternalBoundary(adjacency, change.surfaceId, edge, begin, end);
        };
        const std::array<bool, 4> extend{
            region.minU == 0 && external(TerrainCompositedSurfaceEdge::MinU, region.minV, region.maxV),
            region.maxU == 1 && external(TerrainCompositedSurfaceEdge::MaxU, region.minV, region.maxV),
            region.minV == 0 && external(TerrainCompositedSurfaceEdge::MinV, region.minU, region.maxU),
            region.maxV == 1 && external(TerrainCompositedSurfaceEdge::MaxV, region.minU, region.maxU)};
        for (const auto& mapping : mappings[size_t(found - layouts.begin())])
        {
            const auto& layout = layouts[mapping.target];
            auto mapped = Map(mapping.transform, region);
            if (extend[0])
                Extend(mapped, -mapping.transform[0], -mapping.transform[3]);
            if (extend[1])
                Extend(mapped, mapping.transform[0], mapping.transform[3]);
            if (extend[2])
                Extend(mapped, -mapping.transform[1], -mapping.transform[4]);
            if (extend[3])
                Extend(mapped, mapping.transform[1], mapping.transform[4]);
            bool affected = false;
            for (uint32_t mipIndex = 0; mipIndex < layout.MipCount(); ++mipIndex)
            {
                const auto& mip = layout.Mip(mipIndex);
                const double radius = TerrainCompositedVTLayout::Border + filterRadiusTexels +
                    (mipIndex == 0 ? 0 : 0.5);
                const double beginU = std::max(0., mapped.minU - radius / mip.width);
                const double beginV = std::max(0., mapped.minV - radius / mip.height);
                const double endU = std::min(1., mapped.maxU + radius / mip.width);
                const double endV = std::min(1., mapped.maxV + radius / mip.height);
                if (beginU >= endU || beginV >= endV)
                    continue;
                const auto beginX = uint32_t(std::floor(beginU * mip.width / layout.InnerExtent));
                const auto beginY = uint32_t(std::floor(beginV * mip.height / layout.InnerExtent));
                const auto endX = std::min(mip.pagesX,
                    uint32_t(std::ceil(endU * mip.width / layout.InnerExtent)));
                const auto endY = std::min(mip.pagesY,
                    uint32_t(std::ceil(endV * mip.height / layout.InnerExtent)));
                for (uint32_t pageY = beginY; pageY < endY; ++pageY)
                    for (uint32_t pageX = beginX; pageX < endX; ++pageX)
                    {
                        TerrainCompositedVTPageKey key{layout.SurfaceId(), mipIndex, pageX, pageY};
                        pages.insert(key);
                        while (key.mip + 1 < layout.MipCount())
                        {
                            key = layout.Parent(key);
                            pages.insert(key);
                        }
                        affected = true;
                    }
            }
            if (affected)
                consumers[layout.SurfaceId()].basePages = true;
        }
    }
    TerrainCompositedDirtyResult result;
    result.pages.assign(pages.begin(), pages.end());
    for (const auto& consumer : consumers)
        result.surfaces.push_back({consumer.first, consumer.second});
    return result;
}

}
