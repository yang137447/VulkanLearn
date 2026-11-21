#pragma once

#include "render/frame/terrainCompositedReadiness.h"
#include "script/scriptTypes.h"

namespace VL
{
class World;

using TerrainQualityReadyGetter = bool (*)(uint64_t, const Eigen::Matrix4f&);

inline bool TerrainLoadingRequired(
    bool offlineMode,
    bool terrainView,
    uint64_t worldGeneration,
    const Eigen::Matrix4f& viewProjection,
    TerrainQualityReadyGetter getter = TerrainCompositedQualityReady)
{
    return !offlineMode && terrainView && !getter(worldGeneration, viewProjection);
}

bool IsTerrainWorldLoading(const World* world, bool offlineMode);

inline void ApplyTerrainLoadingHud(ScriptRuntimeView& view, bool loading)
{
    if (!loading)
        return;
    view.available = true;
    view.state = ScriptPlayState::Paused;
    view.hudLines = {{"", "", "terrain-loading", "Loading terrain..."}};
}
}
