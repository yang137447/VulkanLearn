#pragma once

#include <string_view>
#include <nlohmann/json.hpp>
#include "../sceneAssetTypes.h"

// Validates scene JSON structure and object-level required fields.
// Mesh object preflight is delegated to MeshAssetLoader so SceneLoader can stay focused on runtime creation.
class SceneAssetValidator
{
public:
    static SceneAssetBuildPlan BuildLoadPlan(
        std::string_view scenePath,
        const nlohmann::json& sceneJson);

    static void Validate(const SceneAssetBuildPlan& sceneBuildPlan);
};
