#pragma once

#include <string_view>
#include <unordered_map>
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
        const nlohmann::json& sceneJson,
        // 粒子预览的未保存草稿：资源根相对路径 → 作者文档。命中时跳过磁盘读取。
        const std::unordered_map<std::string, nlohmann::json>&
            particleDocuments = {});

    static void Validate(const SceneAssetBuildPlan& sceneBuildPlan);
};
