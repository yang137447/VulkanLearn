#pragma once

#include <cstddef>
#include <optional>
#include <string>
#include <vector>
#include "../mesh/loader/common/meshAssetLoader.h"

// One validated scene object entry used by SceneLoader runtime dispatch.
// Mesh objects carry an already built mesh load request so SceneLoader does not rebuild it.
struct SceneObjectBuildPlan
{
    size_t objectIndex = 0;
    std::string objectName;
    std::string objectType;
    std::optional<MeshAssetLoadRequest> meshLoadRequest;
};

// Scene-level validated loading plan consumed by SceneLoader and preflight checks.
struct SceneAssetBuildPlan
{
    std::string scenePath;
    std::vector<SceneObjectBuildPlan> objectPlans;
};
