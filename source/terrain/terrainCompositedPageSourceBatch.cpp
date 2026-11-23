#include "terrain/terrainCompositedPageSourceBatch.h"
#include "terrain/terrainPageManager.h"
#include "terrain/terrainWeightMap.h"
#include <set>
#include <stdexcept>

namespace VL
{
TerrainCompositedPageSourceBatch::TerrainCompositedPageSourceBatch(
    TerrainPageManager& value, const std::vector<TerrainBuildPlan>& plans)
    : manager(value), expectedPlans(value.plans.size()), expectedPages(value.pages.size())
{
    sources.reserve(plans.size());
    std::set<size_t> unique;
    for (const auto& plan : plans)
    {
        const auto index = manager.FindRegistrationIndex(TerrainPlacementIdentity(plan));
        if (index == static_cast<size_t>(-1) || !unique.insert(index).second ||
            !plan.heightmap || index >= manager.plans.size())
            throw std::invalid_argument("Prepared page sources need distinct registered plans");
        const auto& old = manager.plans[index];
        if (old.heightmap && (old.heightmap->width != plan.heightmap->width ||
            old.heightmap->height != plan.heightmap->height))
            throw std::invalid_argument("Prepared page sources cannot change geometry layout");
        if (plan.layerPaint && (plan.layerPaint->Empty() ||
            plan.layerPaint->layerCount != old.desc.layers.size()))
            throw std::invalid_argument("Prepared page paint layout differs");
        sources.push_back({index, old.heightmap, old.layerPaint, plan.heightmap, plan.layerPaint});
        for (size_t pageIndex = 0; pageIndex < manager.pages.size(); ++pageIndex)
        {
            const auto& page = manager.pages[pageIndex];
            if (page.key.placementId != TerrainPlacementIdentity(plan))
                continue;
            auto layers = TerrainWeightMapBuilder::BuildPageCandidates(plan,
                manager.registrations[index].tilesPerSide, TerrainPageManager::PageResolution,
                page.key.tileX, page.key.tileZ).layerIds;
            pages.push_back({pageIndex, std::move(layers)});
        }
    }
}

bool TerrainCompositedPageSourceBatch::CanCommitNoThrow() const noexcept
{
    if (committed || manager.plans.size() != expectedPlans || manager.pages.size() != expectedPages)
        return false;
    for (const auto& source : sources)
        if (manager.plans[source.index].heightmap != source.expectedHeight ||
            manager.plans[source.index].layerPaint != source.expectedPaint)
            return false;
    return true;
}

void TerrainCompositedPageSourceBatch::CommitNoThrow() noexcept
{
    if (!CanCommitNoThrow())
        return;
    for (auto& source : sources)
    {
        manager.plans[source.index].heightmap.swap(source.height);
        manager.plans[source.index].layerPaint.swap(source.paint);
    }
    for (auto& page : pages)
        manager.pages[page.index].candidateLayers.swap(page.layers);
    committed = true;
}

uint64_t TerrainCompositedPageSourceBatch::MetadataBytes() const noexcept
{
    uint64_t bytes = sizeof(*this) + sources.capacity() * sizeof(Source) +
        pages.capacity() * sizeof(Page);
    for (const auto& page : pages)
        bytes += page.layers.capacity() + 128;
    return bytes;
}
}
