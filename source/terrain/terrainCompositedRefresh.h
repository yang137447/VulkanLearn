#pragma once

#include "terrain/terrainAssetTypes.h"
#include "terrain/terrainCompositedDirty.h"
#include "terrain/terrainCompositedAuthorDependency.h"
#include "shader/build/contentHash.h"

#include <cmath>
#include <memory>
#include <stdexcept>
#include <utility>
#include <vector>

namespace VL
{
class TerrainCompositedVTStore;
class TerrainCompositedVerifiedInputs;

struct TerrainCompositedRefreshCommand
{
    std::shared_ptr<const TerrainBuildPlan> inputs;
    std::vector<TerrainCompositedDirtyChange> changes;
    uint64_t worldGeneration = 0;
    uint64_t expectedRevision = 0;
    uint64_t editSeq = 0;
    uint64_t sourceRevision = 0;

    static TerrainCompositedRefreshCommand Freeze(
        const TerrainBuildPlan& plan, TerrainCompositedDirtyChange change,
        uint64_t generation, uint64_t sequence, uint64_t sourceRevision,
        uint64_t expectedRevision = 0)
    {
        const auto& rect = change.rect;
        if (TerrainPlacementIdentity(plan) != change.surfaceId || change.surfaceId.empty() ||
            sequence == 0 || !std::isfinite(rect.minU) || !std::isfinite(rect.minV) ||
            !std::isfinite(rect.maxU) || !std::isfinite(rect.maxV) ||
            rect.minU < 0 || rect.minV < 0 || rect.maxU > 1 || rect.maxV > 1 ||
            rect.minU >= rect.maxU || rect.minV >= rect.maxV)
            throw std::invalid_argument("Terrain refresh requires matching placement, dirty rect and edit sequence");
        ClassifyTerrainCompositedDependencyChange(
            change.kind, change.staticHeightBlendBefore, change.staticHeightBlendAfter);
        return {std::make_shared<const TerrainBuildPlan>(plan), {std::move(change)},
            generation, expectedRevision, sequence, sourceRevision};
    }

    bool IsCurrent(uint64_t generation, uint64_t sequence) const noexcept
    {
        return inputs && worldGeneration == generation && editSeq == sequence;
    }
};

enum class TerrainCompositedRefreshState
{
    Queued,
    Prepared,
    Submitted,
    Published,
    Failed,
    Stale
};

struct TerrainCompositedRefreshTicket
{
    uint64_t id = 0;
    uint64_t worldGeneration = 0;
    uint64_t editSeq = 0;
    std::string surfaceId;
};

struct TerrainCompositedRefreshStatus
{
    TerrainCompositedRefreshTicket ticket;
    TerrainCompositedRefreshState state = TerrainCompositedRefreshState::Queued;
    uint64_t publishedRevision = 0;
    std::string diagnostic;

    bool Terminal() const noexcept
    {
        return state == TerrainCompositedRefreshState::Published ||
            state == TerrainCompositedRefreshState::Failed ||
            state == TerrainCompositedRefreshState::Stale;
    }

    bool Acknowledges(const TerrainCompositedRefreshTicket& expected) const noexcept
    {
        return state == TerrainCompositedRefreshState::Published && ticket.id != 0 &&
            ticket.id == expected.id && ticket.worldGeneration == expected.worldGeneration &&
            ticket.editSeq == expected.editSeq && ticket.surfaceId == expected.surfaceId;
    }
};

struct TerrainCompositedAuthorBaseline
{
    uint64_t worldGeneration = 0;
    std::string surfaceId;
    ContentDigest productionIdentity;
    std::shared_ptr<const TerrainCompositedVTStore> store;
    std::vector<TerrainCompositedAuthorDependency> dependencies;
    std::shared_ptr<TerrainCompositedVerifiedInputs> verificationOwner;
};
}
