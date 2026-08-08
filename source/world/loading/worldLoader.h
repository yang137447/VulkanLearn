#pragma once

#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "core/runtimeResult.h"
#include "scene/sceneAssetTypes.h"

namespace VL
{

struct WorldBuildPlan
{
    std::string scenePath;
    nlohmann::json sceneJson;
    SceneAssetBuildPlan sceneAssetPlan;
    std::vector<MeshObjectBuildPlan> meshObjectPlans;
    std::vector<SpeedTreeWindProfile> speedTreeWindProfiles;
};

// IO and validation stage for loading a World. It produces a pure build plan;
// runtime object creation belongs to WorldBuilder or renderer resource loaders.
class WorldLoader
{
public:
    RuntimeResult<WorldBuildPlan> Load(const std::string& scenePath) const;
};

} // namespace VL
