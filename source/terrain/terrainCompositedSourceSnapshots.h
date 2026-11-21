#pragma once

#include "terrain/terrainAssetTypes.h"
#include "terrain/terrainRvtBudget.h"

#include <cstddef>
#include <memory>

namespace VL
{
class TerrainCompositedSourceSnapshots
{
public:
    explicit TerrainCompositedSourceSnapshots(
        std::shared_ptr<TerrainRvtBudgetOwner> geometryOwner);
    TerrainCompositedSourceSnapshots(const TerrainCompositedSourceSnapshots&) = delete;
    TerrainCompositedSourceSnapshots& operator=(const TerrainCompositedSourceSnapshots&) = delete;

    TerrainBuildPlan Retain(const TerrainBuildPlan& plan,
        TerrainRvtResourcePhase phase = TerrainRvtResourcePhase::Candidate);
    void Promote(const TerrainBuildPlan& plan, TerrainRvtResourcePhase phase);
    std::vector<TerrainRvtBudgetPhaseChange> PreparePromote(
        const TerrainBuildPlan& plan, TerrainRvtResourcePhase phase) const;
    size_t LiveSources() const;

private:
    struct State;
    std::shared_ptr<TerrainRvtBudgetOwner> owner;
    std::shared_ptr<State> state;
};
}
