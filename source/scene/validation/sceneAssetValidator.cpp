#include "sceneAssetValidator.h"
#include "scene/validation/sceneJsonValidator.h"

#include <stdexcept>
#include <string>

SceneAssetBuildPlan SceneAssetValidator::BuildLoadPlan(
    std::string_view scenePath,
    const nlohmann::json& sceneJson)
{
    try { VL::ValidateSceneAssetJson(sceneJson); }
    catch (const std::exception& error)
    {
        throw std::runtime_error(std::string(error.what()) + ": " + std::string(scenePath));
    }

    SceneAssetBuildPlan sceneBuildPlan;
    sceneBuildPlan.scenePath = std::string(scenePath);
    const auto& objectsJson = sceneJson.at("objects");
    sceneBuildPlan.objectPlans.reserve(objectsJson.size());
    for (size_t objectIndex = 0; objectIndex < objectsJson.size(); ++objectIndex)
    {
        const auto& objectJson = objectsJson[objectIndex];
        SceneAssetObjectPlan objectPlan;
        objectPlan.objectIndex = objectIndex;
        objectPlan.objectName = objectJson.at("name").get<std::string>();
        objectPlan.objectType = objectJson.at("type").get<std::string>();
        // JSON 合同在共享入口收口；运行时计划仍由正式 loader 准备，不为资产工具另建导入路径。
        if (objectPlan.objectType == "mesh")
            objectPlan.meshLoadRequest = MeshAssetLoader::Prepare(objectJson.at("modelPath").get<std::string>());
        sceneBuildPlan.objectPlans.push_back(std::move(objectPlan));
    }
    return sceneBuildPlan;
}

void SceneAssetValidator::Validate(const SceneAssetBuildPlan& sceneBuildPlan)
{
    for (const SceneAssetObjectPlan& objectPlan : sceneBuildPlan.objectPlans)
    {
        if (!objectPlan.meshLoadRequest.has_value()) continue;
        const MeshAssetLoadRequest& meshLoadRequest = *objectPlan.meshLoadRequest;
        try
        {
            MeshAssetLoader::ValidateImportRequest(meshLoadRequest);
            if (meshLoadRequest.buildPlan.assetType == "speedtree")
                MeshAssetLoader::ValidateSectionLoadable(meshLoadRequest);
        }
        catch (const std::exception& exception)
        {
            throw std::runtime_error(
                "Scene mesh object load validation failed: " + sceneBuildPlan.scenePath +
                " object=" + objectPlan.objectName +
                " modelPath=" + meshLoadRequest.buildPlan.meshAssetPath +
                " error=" + exception.what());
        }
    }
}
