#include "world/loading/terrainCollisionInstances.h"

#include <cmath>
#include <unordered_set>

namespace VL
{
namespace
{

bool ReadVec3(const nlohmann::json& value, Eigen::Vector3f& out)
{
    if (!value.is_array() || value.size() != 3)
        return false;
    for (size_t index = 0; index < 3; ++index)
    {
        if (!value[index].is_number())
            return false;
        const double number = value[index].get<double>();
        if (!std::isfinite(number) || !std::isfinite(static_cast<float>(number)))
            return false;
        out[static_cast<Eigen::Index>(index)] = static_cast<float>(number);
    }
    return true;
}

RuntimeResult<std::vector<TerrainCollisionInstance>> Failure(
    std::string code,
    std::string message,
    const SceneAssetBuildPlan& sceneAssetPlan)
{
    return RuntimeResult<std::vector<TerrainCollisionInstance>>::Failure(
        MakeRuntimeError(std::move(code), std::move(message), sceneAssetPlan.scenePath));
}

} // namespace

RuntimeResult<std::vector<TerrainCollisionInstance>> BuildTerrainCollisionInstances(
    const SceneAssetBuildPlan& sceneAssetPlan,
    const nlohmann::json& sceneJson)
{
    const nlohmann::json* objects = nullptr;
    if (sceneJson.is_object() && sceneJson.contains("objects") &&
        sceneJson.at("objects").is_array())
    {
        objects = &sceneJson.at("objects");
    }

    std::vector<TerrainCollisionInstance> instances;
    std::unordered_set<std::string> placementIds;
    for (const SceneAssetObjectPlan& objectPlan : sceneAssetPlan.objectPlans)
    {
        if (objectPlan.objectType != "terrain")
            continue;
        if (!objectPlan.terrainBuildPlan.has_value())
        {
            return Failure(
                "TerrainCollision.MissingPlan",
                "Terrain object has no prepared terrain build plan: " + objectPlan.objectName,
                sceneAssetPlan);
        }
        if (objects == nullptr || objectPlan.objectIndex >= objects->size() ||
            !(*objects)[objectPlan.objectIndex].is_object())
        {
            return Failure(
                "TerrainCollision.MissingSceneObject",
                "Terrain object is not present in the validated scene JSON: " + objectPlan.objectName,
                sceneAssetPlan);
        }

        const nlohmann::json& objectJson = (*objects)[objectPlan.objectIndex];
        TerrainCollisionInstance instance;
        instance.plan = *objectPlan.terrainBuildPlan;
        instance.placementId = TerrainPlacementIdentity(instance.plan);
        if (instance.placementId.empty())
        {
            return Failure(
                "TerrainCollision.MissingPlacementId",
                "Terrain object has no placement identity: " + objectPlan.objectName,
                sceneAssetPlan);
        }

        if (!objectJson.contains("position") ||
            !ReadVec3(objectJson.at("position"), instance.placement.position) ||
            !objectJson.contains("rotation") ||
            !ReadVec3(objectJson.at("rotation"), instance.placement.rotation) ||
            !objectJson.contains("scale") ||
            !ReadVec3(objectJson.at("scale"), instance.placement.scale))
        {
            return Failure(
                "TerrainCollision.InvalidTransform",
                "Terrain object requires finite vec3 position/rotation/scale: " +
                    objectPlan.objectName,
                sceneAssetPlan);
        }
        // 物理 heightfield 只支持正向缩放；镜像摆放留给后续独立的反射处理，
        // 现在明确拒绝，避免 Play 时才在 body 创建阶段失败。
        if (instance.placement.scale.x() <= 0.0f ||
            instance.placement.scale.y() <= 0.0f ||
            instance.placement.scale.z() <= 0.0f)
        {
            return Failure(
                "TerrainCollision.NonPositiveScale",
                "Terrain collision requires positive scale: " + objectPlan.objectName,
                sceneAssetPlan);
        }
        if (!placementIds.insert(instance.placementId).second)
        {
            return Failure(
                "TerrainCollision.DuplicatePlacement",
                "Terrain placement identity is duplicated: " + instance.placementId,
                sceneAssetPlan);
        }
        instances.push_back(std::move(instance));
    }
    return RuntimeResult<std::vector<TerrainCollisionInstance>>::Success(
        std::move(instances));
}

} // namespace VL
