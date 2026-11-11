#include "world/loading/worldGameplayPlanBuilder.h"

#include <exception>
#include <stdexcept>

#include <Eigen/Dense>

namespace VL
{

namespace
{

Eigen::Vector3f ReadVec3(const nlohmann::json& value)
{
    if (!value.is_array() || value.size() != 3)
    {
        throw std::runtime_error("Gameplay object requires a vec3");
    }
    return Eigen::Vector3f(
        value[0].get<float>(),
        value[1].get<float>(),
        value[2].get<float>());
}

// 与 CommonFunction::RotationToMatrix 相同的 yaw * pitch * roll 约定；
// 这里本地实现是为了让计划构建器不依赖 Vulkan 头。
Eigen::Matrix4f RotationToMatrix(const Eigen::Vector3f& degrees)
{
    const Eigen::Vector3f radians = degrees * (3.14159265358979323846f / 180.0f);
    const Eigen::Quaternionf quaternion =
        (Eigen::AngleAxisf(radians.y(), Eigen::Vector3f::UnitY()) *
         Eigen::AngleAxisf(radians.x(), Eigen::Vector3f::UnitX()) *
         Eigen::AngleAxisf(radians.z(), Eigen::Vector3f::UnitZ()))
            .normalized();
    Eigen::Matrix4f matrix = Eigen::Matrix4f::Identity();
    matrix.block<3, 3>(0, 0) = quaternion.toRotationMatrix();
    return matrix;
}

Eigen::Matrix4f BuildRuntimeObjectModelMatrix(
    const Eigen::Vector3f& position,
    const Eigen::Vector3f& rotation,
    const Eigen::Vector3f& scale)
{
    Eigen::Matrix4f scaleMatrix = Eigen::Matrix4f::Identity();
    scaleMatrix(0, 0) = scale.x();
    scaleMatrix(1, 1) = scale.y();
    scaleMatrix(2, 2) = scale.z();
    Eigen::Matrix4f translation = Eigen::Matrix4f::Identity();
    translation.block<3, 1>(0, 3) = position;
    return translation * RotationToMatrix(rotation) * scaleMatrix;
}

// 裂纹覆盖层的固定默认 MI；缺失时 cracked 只保留 debug 折线。
constexpr const char* DefaultGlassCrackMaterialPath =
    "Common/Materials/MI_glass_crack.json";

} // namespace

RuntimeResult<WorldGameplayPlans> WorldGameplayPlanBuilder::Build(
    const SceneAssetBuildPlan& sceneAssetPlan,
    const nlohmann::json& resolvedSceneJson,
    std::string_view scenePath) const
{
    try
    {
        if (!resolvedSceneJson.is_object() ||
            !resolvedSceneJson.contains("objects") ||
            !resolvedSceneJson.at("objects").is_array())
        {
            throw std::runtime_error(
                "Gameplay plan build requires a resolved scene objects array");
        }
        const nlohmann::json& objectsJson = resolvedSceneJson.at("objects");

        WorldGameplayPlans plans;
        for (const SceneAssetObjectPlan& objectPlan : sceneAssetPlan.objectPlans)
        {
            const std::string& type = objectPlan.objectType;
            if (type != "balloon" && type != "glassPane")
                continue;
            if (objectPlan.objectIndex >= objectsJson.size())
            {
                throw std::runtime_error(
                    "Gameplay object index is outside the scene objects array: " +
                    objectPlan.objectName);
            }
            const nlohmann::json& objectJson = objectsJson.at(objectPlan.objectIndex);

            if (type == "balloon")
            {
                SoftBodyObjectBuildPlan plan;
                plan.objectName = objectPlan.objectName;
                plan.sceneObjectIdentity = objectPlan.objectName;
                plan.profileKey = objectJson.value("profile", "");
                plan.scenePosition = ReadVec3(objectJson.at("position"));
                plan.sceneRotation = ReadVec3(objectJson.at("rotation"));
                plan.sceneScale = ReadVec3(objectJson.at("scale"));
                plan.model = BuildRuntimeObjectModelMatrix(
                    plan.scenePosition, plan.sceneRotation, plan.sceneScale);
                plan.radius = objectJson.at("radius").get<double>();
                plan.thickness = objectJson.at("thickness").get<double>();
                plan.subdivisions = objectJson.at("subdivisions").get<int>();
                plan.siteCount = objectJson.at("siteCount").get<int>();
                plan.pressure = objectJson.at("pressure").get<double>();
                plan.burstStrain = objectJson.at("burstStrain").get<double>();
                plan.restContraction = objectJson.at("restContraction").get<double>();
                plan.seed = objectJson.at("seed").get<uint64_t>();
                plan.materialInstancePath =
                    objectJson.at("materialInstance").get<std::string>();
                plans.softBodyObjectPlans.push_back(std::move(plan));
            }
            else
            {
                GlassObjectBuildPlan plan;
                plan.objectName = objectPlan.objectName;
                plan.sceneObjectIdentity = objectPlan.objectName;
                plan.profileKey = objectJson.value("profile", "");
                plan.scenePosition = ReadVec3(objectJson.at("position"));
                plan.sceneRotation = ReadVec3(objectJson.at("rotation"));
                plan.sceneScale = ReadVec3(objectJson.at("scale"));
                plan.model = BuildRuntimeObjectModelMatrix(
                    plan.scenePosition, plan.sceneRotation, plan.sceneScale);
                plan.width = objectJson.at("width").get<double>();
                plan.height = objectJson.at("height").get<double>();
                plan.thickness = objectJson.at("thickness").get<double>();
                plan.siteCount = objectJson.at("siteCount").get<int>();
                plan.seed = objectJson.at("seed").get<uint64_t>();
                plan.subfracture = objectJson.value("subfracture", true);
                plan.materialInstancePath =
                    objectJson.at("materialInstance").get<std::string>();
                plan.crackMaterialInstancePath = DefaultGlassCrackMaterialPath;
                plans.glassObjectPlans.push_back(std::move(plan));
            }
        }

        if (resolvedSceneJson.contains("projectile"))
        {
            const nlohmann::json& projectile = resolvedSceneJson.at("projectile");
            ProjectileBuildPlan plan;
            plan.materialInstancePath =
                projectile.at("materialInstance").get<std::string>();
            plan.speedCmPerSecond = projectile.value("speed", 3500.0f);
            plan.radiusCm = projectile.value("radius", 1.5f);
            plan.massKg = projectile.value("mass", 0.08f);
            plan.cooldownSeconds = projectile.value("cooldown", 0.15f);
            plans.projectile = std::move(plan);
        }

        return RuntimeResult<WorldGameplayPlans>::Success(std::move(plans));
    }
    catch (const std::exception& exception)
    {
        // 保持既有加载错误码：场景 JSON 结构问题仍归入 Scene.LoadFailed。
        return RuntimeResult<WorldGameplayPlans>::Failure(MakeRuntimeError(
            "Scene.LoadFailed",
            exception.what(),
            std::string(scenePath)));
    }
}

} // namespace VL
