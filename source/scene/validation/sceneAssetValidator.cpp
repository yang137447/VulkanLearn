#include "sceneAssetValidator.h"
#include "scene/validation/sceneJsonValidator.h"
#include "terrain/terrainAssetLoader.h"

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
        {
            objectPlan.meshLoadRequest = MeshAssetLoader::Prepare(objectJson.at("modelPath").get<std::string>());
            MeshAssetLoader::PrepareTressfxPlacement(*objectPlan.meshLoadRequest, objectJson);
        }
        else if (objectPlan.objectType == "terrain")
        {
            objectPlan.terrainBuildPlan = VL::TerrainAssetLoader::Prepare(
                objectJson.at("terrainPath").get<std::string>());
            VL::TerrainBuildPlan& terrainPlan = *objectPlan.terrainBuildPlan;
            // 场景对象身份（type|name）就是摆放身份：同一 TR 可以有多个不同名字的对象，
            // 它们共享 assetPath，但页表注册与 chunk 资源必须彼此独立。
            terrainPlan.placementId = objectPlan.objectType + "|" + objectPlan.objectName;
            // 可选覆盖让同一 TR 的多处摆放占用不同虚拟页区域；未提供时保留 TR 原值。
            if (objectJson.contains("pageOrigin"))
                terrainPlan.desc.pageOrigin = Eigen::Vector2i(
                    objectJson.at("pageOrigin")[0].get<int>(),
                    objectJson.at("pageOrigin")[1].get<int>());
            if (objectJson.contains("pageTilesPerSide"))
                terrainPlan.desc.pageTilesPerSide =
                    objectJson.at("pageTilesPerSide").get<uint32_t>();
        }
        sceneBuildPlan.objectPlans.push_back(std::move(objectPlan));
        if (objectJson.at("type") == "blueprintInstance" && objectJson.contains("components"))
        for (size_t component = 0; component < objectJson.at("components").size(); ++component)
        {
            const auto& componentJson = objectJson.at("components")[component];
            const std::string componentType = componentJson.at("type").get<std::string>();
            if (!VL::IsScriptMeshComponent(componentType)) continue;
            SceneAssetObjectPlan mesh;
            mesh.objectIndex = objectIndex;
            mesh.objectType = "mesh";
            mesh.componentObject = VL::ScriptBlueprintMeshObject(objectJson, component);
            mesh.objectName = mesh.componentObject->at("name").get<std::string>();
            mesh.meshLoadRequest = MeshAssetLoader::Prepare(mesh.componentObject->at("modelPath").get<std::string>());
            MeshAssetLoader::PrepareTressfxPlacement(*mesh.meshLoadRequest, *mesh.componentObject);
            // tressfx 组件：毛发描述符必须是 tressfx，父 mesh 必须是带骨架的角色。
            // 骨架检查放在加载计划里（需要读描述符），组件树本身的拓扑已在
            // ValidateScriptComponents 收口。
            if (componentType == "tressfx")
            {
                const std::string componentId = componentJson.value("id", "root");
                if (mesh.meshLoadRequest->buildPlan.assetType != "tressfx")
                    throw std::runtime_error(
                        "TressFX component must reference a tressfx descriptor: actor=" +
                        objectJson.at("id").get<std::string>() + " component=" + componentId +
                        " modelPath=" + mesh.componentObject->at("modelPath").get<std::string>());
                const auto& parentPath = mesh.componentObject->at("__tressfxParentModelPath");
                const auto parentRequest = MeshAssetLoader::Prepare(parentPath.get<std::string>());
                if (!parentRequest.buildPlan.IsSkeletal())
                    throw std::runtime_error(
                        "TressFX component parent mesh must be skeletal: actor=" +
                        objectJson.at("id").get<std::string>() + " component=" + componentId +
                        " parentModelPath=" + parentPath.get<std::string>());
            }
            sceneBuildPlan.objectPlans.push_back(std::move(mesh));
        }
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
