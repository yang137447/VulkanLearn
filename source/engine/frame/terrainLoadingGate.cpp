#include "engine/frame/terrainLoadingGate.h"

#include "sceneNode.h"
#include "world/world.h"

#include <algorithm>

namespace VL
{
bool IsTerrainWorldLoading(const World* world, bool offlineMode)
{
    if (!world || offlineMode)
        return false;
    const auto& objects = world->GetMeshObjects();
    const bool terrainView = std::any_of(objects.begin(), objects.end(),
        [](const auto& entry)
        {
            return entry.second.isTerrain && entry.second.visible;
        });
    if (!terrainView)
        return false;
    const auto& camera = world->GetRenderCamera();
    if (!camera)
        return true;
    const Eigen::Matrix4f viewProjection =
        camera->GetProjectionMatrix() * camera->GetViewMatrix();
    return TerrainLoadingRequired(false, true, world->GetGeneration(), viewProjection);
}
}
