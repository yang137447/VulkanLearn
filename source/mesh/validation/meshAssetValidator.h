#pragma once

#include <string_view>
#include <vector>
#include <nlohmann/json.hpp>
#include "../meshAssetTypes.h"

// Converts effective mesh asset JSON into load plans and validates section/material-slot mapping.
// Input comes from MeshAssetResolver and IModelImporter output; RendererMeshLoader
// consumes the result when it creates renderable sections
// and loads material instances. It does not read files, import model geometry,
// create Vulkan resources, fuzzy-match slot names, or repair asset naming.
class MeshAssetValidator
{
public:
    static MeshAssetBuildPlan BuildLoadPlan(
        std::string_view meshAssetPath,
        const nlohmann::json& effectiveMeshAssetJson);

    static std::vector<MeshSectionLoadPlan> BuildSectionLoadPlans(
        const MeshAssetBuildPlan& buildPlan,
        const ModelResource& modelResource);
};
