#include "render/resource/rendererResourceLoadCoordinator.h"

#include <iterator>
#include <stdexcept>

#include <nlohmann/json.hpp>

#include "render/resource/rendererEnvironmentLoader.h"
#include "render/resource/rendererMaterialLoader.h"
#include "render/resource/rendererMeshLoader.h"
#include "render/resource/rendererResourceCache.h"
#include "world/loading/worldLoader.h"

namespace VL
{

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

RendererWorldResourceLoadResult RendererResourceLoadCoordinator::LoadRendererResources(
    const WorldBuildPlan& worldBuildPlan,
    uint64_t ownerGeneration)
{
    if (pipelineFactory == nullptr)
    {
        throw std::runtime_error("PipelineFactory is not set in RendererResourceLoadCoordinator");
    }
    if (rendererBackend == nullptr)
    {
        throw std::runtime_error("RendererBackendVulkan is not set in RendererResourceLoadCoordinator");
    }

    RendererResourceCache& resourceCache = RendererResourceCache::GetInstance();
    resourceCache.BeginWorldLocalResourceLoad(ownerGeneration);

    RendererEnvironmentLoader environmentLoader(*pipelineFactory, *rendererBackend);
    RendererMaterialLoader materialLoader(*pipelineFactory, *rendererBackend);
    RendererMeshLoader meshLoader(*pipelineFactory, *rendererBackend);
    RendererWorldResourceLoadResult loadResult;

    environmentLoader.LoadGlobalResources();

    // Load pass materials before scene meshes so descriptor setup can reference
    // render graph pass material instances.
    materialLoader.LoadPassMaterials();

    // Delegate renderer-facing resource creation by scene object type.
    // Gameplay-facing camera/light/environment metadata is owned by WorldBuilder.
    const nlohmann::json& scnJson = worldBuildPlan.sceneJson;
    for (const SceneAssetObjectPlan& objectPlan : worldBuildPlan.sceneAssetPlan.objectPlans)
    {
        const auto& obj = scnJson["objects"][objectPlan.objectIndex];
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
        else if (type == "environment")
        {
            environmentLoader.LoadEnvironmentObject(obj);
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
