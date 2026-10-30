#include "render/resource/rendererResourceLoadCoordinator.h"

#include <iterator>
#include <stdexcept>

#include <nlohmann/json.hpp>

#include "commonFunction.h"
#include "material.h"
#include "materialInstance.h"
#include "render/resource/rendererEnvironmentLoader.h"
#include "render/resource/rendererMaterialLoader.h"
#include "render/resource/rendererMeshLoader.h"
#include "render/resource/rendererResourceCache.h"
#include "render/resource/rendererResourceLoadContext.h"
#include "renderGraph.h"
#include "world/loading/worldLoader.h"

namespace VL
{
namespace
{

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
    return translation * CommonFunction::RotationToMatrix(rotation) * scaleMatrix;
}

} // namespace

RendererResourceLoadCoordinator::RendererResourceLoadCoordinator()
{
}

void RendererResourceLoadCoordinator::SetPipelineFactory(PipelineFactory* pipelineFactory)
{
    this->pipelineFactory = pipelineFactory;
}

void RendererResourceLoadCoordinator::SetRendererBackend(RendererBackendVulkan* rendererBackend)
{
    this->rendererBackend = rendererBackend;
}

RendererWorldResourceLoadResult
RendererResourceLoadCoordinator::LoadRendererResources(
    const WorldBuildPlan& worldBuildPlan,
    RendererResourceLoadContext& loadContext)
{
    if (pipelineFactory == nullptr)
    {
        throw std::runtime_error("PipelineFactory is not set in RendererResourceLoadCoordinator");
    }
    if (rendererBackend == nullptr)
    {
        throw std::runtime_error("RendererBackendVulkan is not set in RendererResourceLoadCoordinator");
    }

    RendererEnvironmentLoader environmentLoader(
        *pipelineFactory,
        *rendererBackend,
        loadContext);
    RendererMaterialLoader materialLoader(
        *pipelineFactory,
        *rendererBackend,
        loadContext);
    RendererMeshLoader meshLoader(
        *pipelineFactory,
        *rendererBackend,
        loadContext);
    RendererWorldResourceLoadResult loadResult;

    environmentLoader.LoadGlobalResources();

    // Load pass materials before scene meshes so descriptor setup can reference
    // render graph pass material instances.
    materialLoader.LoadPassMaterials();

    // Delegate renderer-facing resource creation by scene object type.
    // Gameplay-facing camera/light/environment metadata is owned by WorldBuilder.
    const nlohmann::json& scnJson = worldBuildPlan.sceneJson;
    if (scnJson.contains("projectile"))
    {
        const auto& projectile = scnJson.at("projectile");
        const std::string materialPath =
            projectile.at("materialInstance").get<std::string>();
        const std::shared_ptr<MaterialInstance> materialInstance =
            materialLoader.LoadSceneMaterialInstance(materialPath);
        if (!materialInstance)
            throw std::runtime_error(
                "Projectile material instance was not created: " +
                materialPath);
        loadResult.projectileSettings.materialKey =
            materialInstance->GetBaseMaterial().lock()->GetMaterialKey();
        loadResult.projectileSettings.materialInstanceKey =
            materialInstance->GetName();
        loadResult.projectileSettings.speedCmPerSecond =
            projectile.value("speed", 3500.0f);
        loadResult.projectileSettings.radiusCm =
            projectile.value("radius", 1.5f);
        loadResult.projectileSettings.massKg =
            projectile.value("mass", 0.08f);
        loadResult.projectileSettings.cooldownSeconds =
            projectile.value("cooldown", 0.15f);
    }
    for (const SceneAssetObjectPlan& objectPlan : worldBuildPlan.sceneAssetPlan.objectPlans)
    {
        const auto& obj = objectPlan.componentObject ? *objectPlan.componentObject :
            scnJson["objects"][objectPlan.objectIndex];
        const std::string& type = objectPlan.objectType;

        if (type == "mesh")
        {
            if (!objectPlan.meshLoadRequest.has_value())
            {
                throw std::runtime_error(
                    "Scene mesh object missing mesh load request: " + worldBuildPlan.scenePath +
                    " object=" + objectPlan.objectName);
            }
            RendererMeshLoadResult meshLoadResult =
                meshLoader.LoadMeshObject(obj, *objectPlan.meshLoadRequest);
            loadResult.meshObjectPlans.insert(
                loadResult.meshObjectPlans.end(),
                std::make_move_iterator(meshLoadResult.objectPlans.begin()),
                std::make_move_iterator(meshLoadResult.objectPlans.end()));
            if (meshLoadResult.speedTreeWindProfile.has_value())
            {
                loadResult.speedTreeWindProfiles.push_back(
                    std::move(*meshLoadResult.speedTreeWindProfile));
            }
        }
        else if (type == "balloon")
        {
            if (loadContext.offlineOnly)
                throw std::runtime_error(
                    "Balloon soft body is realtime-only: " +
                    worldBuildPlan.scenePath + " object=" + objectPlan.objectName);
            const std::string materialPath =
                obj.at("materialInstance").get<std::string>();
            const std::shared_ptr<MaterialInstance> materialInstance =
                materialLoader.LoadSceneMaterialInstance(materialPath);
            if (!materialInstance)
                throw std::runtime_error(
                    "Balloon material instance was not created: " +
                    worldBuildPlan.scenePath + " object=" + objectPlan.objectName);

            SoftBodyObjectBuildPlan plan;
            plan.objectName = objectPlan.objectName;
            plan.sceneObjectIdentity = objectPlan.objectName;
            plan.profileKey = obj.value("profile", "");
            plan.scenePosition = JsonParser::ParseValue<Eigen::Vector3f>(obj.at("position"));
            plan.sceneRotation = JsonParser::ParseValue<Eigen::Vector3f>(obj.at("rotation"));
            plan.sceneScale = JsonParser::ParseValue<Eigen::Vector3f>(obj.at("scale"));
            plan.model = BuildRuntimeObjectModelMatrix(
                plan.scenePosition, plan.sceneRotation, plan.sceneScale);
            plan.radius = obj.at("radius").get<double>();
            plan.thickness = obj.at("thickness").get<double>();
            plan.subdivisions = obj.at("subdivisions").get<int>();
            plan.siteCount = obj.at("siteCount").get<int>();
            plan.pressure = obj.at("pressure").get<double>();
            plan.burstStrain = obj.at("burstStrain").get<double>();
            plan.restContraction = obj.at("restContraction").get<double>();
            plan.seed = obj.at("seed").get<uint64_t>();
            plan.materialKey = materialInstance->GetBaseMaterial().lock()->GetMaterialKey();
            plan.materialInstanceKey = materialInstance->GetName();
            loadResult.softBodyObjectPlans.push_back(std::move(plan));
        }
        else if (type == "glassPane")
        {
            if (loadContext.offlineOnly)
                throw std::runtime_error(
                    "Glass pane fracture is realtime-only: " +
                    worldBuildPlan.scenePath + " object=" + objectPlan.objectName);
            const std::string materialPath =
                obj.at("materialInstance").get<std::string>();
            const std::shared_ptr<MaterialInstance> materialInstance =
                materialLoader.LoadSceneMaterialInstance(materialPath);
            if (!materialInstance)
                throw std::runtime_error(
                    "Glass material instance was not created: " +
                    worldBuildPlan.scenePath + " object=" + objectPlan.objectName);

            GlassObjectBuildPlan plan;
            plan.objectName = objectPlan.objectName;
            plan.sceneObjectIdentity = objectPlan.objectName;
            plan.profileKey = obj.value("profile", "");
            plan.scenePosition = JsonParser::ParseValue<Eigen::Vector3f>(obj.at("position"));
            plan.sceneRotation = JsonParser::ParseValue<Eigen::Vector3f>(obj.at("rotation"));
            plan.sceneScale = JsonParser::ParseValue<Eigen::Vector3f>(obj.at("scale"));
            plan.model = BuildRuntimeObjectModelMatrix(
                plan.scenePosition, plan.sceneRotation, plan.sceneScale);
            plan.width = obj.at("width").get<double>();
            plan.height = obj.at("height").get<double>();
            plan.thickness = obj.at("thickness").get<double>();
            plan.siteCount = obj.at("siteCount").get<int>();
            plan.seed = obj.at("seed").get<uint64_t>();
            plan.subfracture = obj.value("subfracture", true);
            plan.materialKey = materialInstance->GetBaseMaterial().lock()->GetMaterialKey();
            plan.materialInstanceKey = materialInstance->GetName();
            // 裂纹覆盖层使用固定深色 MI；缺失时 cracked 仍保留 debug 折线。
            const std::shared_ptr<MaterialInstance> crackMaterial =
                materialLoader.LoadSceneMaterialInstance(
                    "Common/Materials/MI_glass_crack.json");
            if (crackMaterial)
            {
                plan.crackMaterialKey =
                    crackMaterial->GetBaseMaterial().lock()->GetMaterialKey();
                plan.crackMaterialInstanceKey = crackMaterial->GetName();
            }
            loadResult.glassObjectPlans.push_back(std::move(plan));
        }
        else if (type == "environment")
        {
            if (!loadContext.offlineOnly)
                environmentLoader.LoadEnvironmentObject(obj);
        }
        else if (type == "blueprintInstance")
        {
            // Actor 声明归脚本上下文；它的网格已有独立派生计划，不能再次按声明加载。
            continue;
        }
        else if (type == "directionalLight" ||
                 type == "pointLight" ||
                 type == "spotLight" ||
                 type == "camera")
        {
            // WorldBuilder creates gameplay-facing cameras and lights from the
            // validated WorldBuildPlan. Renderer loaders only create resources
            // consumed by the Vulkan draw path.
            continue;
        }
        else if (type == "squareAreaLight")
        {
            if (loadContext.offlineOnly) continue;
            // 当前实时渲染无面光源实现；不能静默丢弃或降级成点光。
            throw std::runtime_error(
                "Realtime path does not support squareAreaLight: " +
                worldBuildPlan.scenePath + " object=" + objectPlan.objectName);
        }
        else
        {
            throw std::runtime_error(
                "Renderer resource load received unsupported validated object type: " +
                type + " object=" + objectPlan.objectName);
        }
    }

    return loadResult;
}

} // namespace VL
