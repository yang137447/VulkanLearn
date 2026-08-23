#include "render/resource/rendererResourceLoadCoordinator.h"

#include <iterator>
#include <stdexcept>

#include <nlohmann/json.hpp>

#include "render/resource/rendererEnvironmentLoader.h"
#include "render/eye/eyeResourceLoader.h"
#include "render/resource/rendererMaterialLoader.h"
#include "render/resource/rendererMeshLoader.h"
#include "render/resource/rendererResourceCache.h"
#include "render/resource/rendererResourceLoadContext.h"
#include "render/subsurface/subsurfaceResourceLoader.h"
#include "render/hair/hairLutBaker.h"
#include "renderGraph.h"
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

void RendererResourceLoadCoordinator::SetEyeComputeReloadParticipant(
    EyeComputeReloadParticipant* participant)
{
    eyeComputeReloadParticipant = participant;
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
    SubsurfaceResourceLoader subsurfaceResourceLoader(
        *pipelineFactory,
        *rendererBackend,
        loadContext);
    HairResourceLoader hairResourceLoader(
        *pipelineFactory,
        *rendererBackend,
        loadContext);
    EyeResourceLoader eyeResourceLoader(
        *pipelineFactory,
        *rendererBackend,
        loadContext,
        eyeComputeReloadParticipant);
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
    // lookup texture 和 path->stable ID 必须先进入 candidate cache，材质加载才能只消费同一 World generation。
    subsurfaceResourceLoader.Load();
    // Hair pass 的外部 LUT descriptor 必须在材质和 candidate graph 初始化前就绪.
    hairResourceLoader.Load();
    // Eye profile 与 caustic LUT 必须在所有 Eye MI 解析前进入 candidate cache。
    eyeResourceLoader.Load();

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
