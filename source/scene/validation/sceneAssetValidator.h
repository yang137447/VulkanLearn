#pragma once

#include <string_view>
#include <nlohmann/json.hpp>
#include "../sceneAssetTypes.h"

// Validates scene JSON structure and object-level required fields.
// Mesh object preflight is delegated to MeshAssetLoader; runtime creation is
// split between WorldBuilder and renderer resource loaders.
class SceneAssetValidator
{
public:
    static SceneAssetBuildPlan BuildLoadPlan(
        std::string_view scenePath,
        const nlohmann::json& sceneJson);

    static void Validate(const SceneAssetBuildPlan& sceneBuildPlan);
};
