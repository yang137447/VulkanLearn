#include "render/resource/rendererResourceLoadCoordinator.h"

#include <algorithm>
#include <iterator>
#include <stdexcept>

#include <nlohmann/json.hpp>

#include "render/backend/rendererBackendVulkan.h"
#include "render/resource/rendererEnvironmentLoader.h"
#include "render/resource/rendererMaterialLoader.h"
#include "render/resource/rendererMeshLoader.h"
#include "render/resource/rendererResourceCache.h"
#include "render/resource/rendererResourceLoadContext.h"
#include "render/resource/terrainFeedbackGpu.h"
#include "render/resource/rendererTerrainLoader.h"
#include "terrain/terrainPageManager.h"
#include "terrain/terrainLayerTable.h"
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

    // C2b/P3：先注册本 World 的全部 terrain page，再创建共享 page table/atlas；
    // 生产路径不再全量 preload，只建立身份并预取每个 placement 的中心 2×2 页。
    // P0：这些状态必须活到 World 退休，因此挂在 candidate package 的
    // TerrainStreamingState 上，而不是 coordinator 的函数局部变量。
    std::shared_ptr<TerrainStreamingState> terrainStreaming =
        std::make_shared<TerrainStreamingState>();
    TerrainPageManager& terrainPageManager = terrainStreaming->pageManager;
    // page table 与 feedback ring 始终存在：即使 World 没有地形，feedback pass
    // 仍需要一个合法 worldTexture，并清空一张 128×128 RT 产出 0 页统计。
    terrainStreaming->pageResources.pageTable =
        CreateTerrainPageTableGpuResource(*rendererBackend);
    terrainStreaming->feedbackRing =
        std::make_shared<TerrainFeedbackGpuRing>(*rendererBackend);
    std::vector<TerrainLayerDesc> worldTerrainLayers;
    bool hasTerrainLayers = false;
    for (const SceneAssetObjectPlan& objectPlan : worldBuildPlan.sceneAssetPlan.objectPlans)
    {
        if (objectPlan.objectType != "terrain")
            continue;
        if (!objectPlan.terrainBuildPlan.has_value())
            throw std::runtime_error(
                "Scene terrain object missing terrain build plan before page registration: " +
                worldBuildPlan.scenePath + " object=" + objectPlan.objectName);
        const TerrainBuildPlan& terrainPlan = *objectPlan.terrainBuildPlan;
        // M_pbr/M_unlit 等地形不必有 layer table；只有带 layer 的地形参与共享 VT。
        if (!TerrainUsesVirtualPages(terrainPlan))
            continue;
        if (!hasTerrainLayers)
        {
            worldTerrainLayers = terrainPlan.desc.layers;
            hasTerrainLayers = true;
        }
        else if (!TerrainLayerTable::AreLayersIdentical(worldTerrainLayers, terrainPlan.desc.layers))
        {
            throw std::runtime_error(
                "All layer-table terrains in a World must share identical layer definitions: " +
                terrainPlan.desc.assetPath);
        }
        // P3：不再全量 preload。注册只建立虚拟页/槽位身份，驻留由初始集合
        // 与 feedback 驱动；否则 32×32 地形会在加载期烘焙所有页并互相淘汰。
        terrainPageManager.RegisterTerrain(terrainPlan, false);
    }
    if (!terrainPageManager.Registrations().empty())
    {
        terrainPageManager.SetResidentCapacity(
            TerrainPageManager::DefaultResidentCapacity);
        CreateTerrainPageAtlasGpuResources(
            *rendererBackend,
            terrainStreaming->pageResources);
        terrainStreaming->uploadRing =
            std::make_shared<TerrainUploadGpuRing>(
                *rendererBackend,
                TerrainPageManager::MaxUploadPagesPerFrame);
        for (const TerrainPageManager::Registration& registration :
             terrainPageManager.Registrations())
        {
            terrainPageManager.RequestInitialTerrainPages(registration);
        }
        // 初始 2×2 页会写脏页表和 atlas；这一整批就是 streaming state 的初始版本。
        terrainStreaming->NotifyContentChanged();
        ApplyTerrainStreamingUploads(*rendererBackend, *terrainStreaming);
    }
    if (hasTerrainLayers)
    {
        terrainStreaming->layerDefinitions = worldTerrainLayers;
        // 层页虚拟编号依赖各层源分辨率，必须在创建页表/请求页之前配置。
        terrainStreaming->layerPageManager.ConfigureLayers(worldTerrainLayers);
        terrainStreaming->layerPageManager.SetResidentCapacity(
            TerrainLayerPageManager::DefaultResidentCapacity);
        terrainStreaming->layerResources =
            CreateTerrainLayerGpuResources(
                *rendererBackend,
                worldTerrainLayers,
                terrainStreaming->layerPageManager);
    }
    // candidate 拥有新 state；提交时随 package 一起发布，失败时随 candidate 析构。
    loadContext.resourceCache.BindTerrainStreaming(terrainStreaming);
    // feedback pass 的 worldTexture 输入；与 M_terrain 使用同一张 page table。
    loadContext.resourceCache.BindWorldTexture(
        "terrainPageTable",
        terrainStreaming->pageResources.pageTable);

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
    RendererTerrainLoader terrainLoader(
        *pipelineFactory,
        *rendererBackend,
        loadContext,
        *terrainStreaming);
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
        else if (type == "terrain")
        {
            if (!objectPlan.terrainBuildPlan.has_value())
            {
                throw std::runtime_error(
                    "Scene terrain object missing terrain build plan: " + worldBuildPlan.scenePath +
                    " object=" + objectPlan.objectName);
            }
            RendererTerrainLoadResult terrainLoadResult =
                terrainLoader.LoadTerrainObject(obj, *objectPlan.terrainBuildPlan);
            loadResult.meshObjectPlans.insert(
                loadResult.meshObjectPlans.end(),
                std::make_move_iterator(terrainLoadResult.objectPlans.begin()),
                std::make_move_iterator(terrainLoadResult.objectPlans.end()));
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

    if (hasTerrainLayers && terrainStreaming->layerSources.albedo)
    {
        // L3：atlas 依赖真实 MI 的层数组，已在首个地形 MI 载入时创建；这里
        // 只做兜底。mode 0 上传每层尾页的映射验证集合；mode 1 也只请求
        // 这个小集合，其余层页由覆盖页 feedback 逐帧驱动（L4a）。
        if (!terrainStreaming->layerResources.atlasesCreated)
        {
            CreateTerrainLayerAtlasGpuResources(
                *rendererBackend,
                *terrainStreaming);
        }
        if (terrainStreaming->layerVTRequested &&
            !terrainStreaming->layerUploadRing)
        {
            // 逐帧层页上传 ring：槽数 = frame fence 数，只打包页表 texel；
            // atlas 像素走 GPU→GPU copy，不占 staging。
            terrainStreaming->layerUploadRing =
                std::make_shared<TerrainLayerUploadGpuRing>(
                    *rendererBackend,
                    terrainStreaming->layerPageManager,
                    terrainStreaming->maxLayerPageUploadsPerFrame);
        }
        RequestInitialTerrainLayerPages(
            *terrainStreaming,
            0,
            rendererBackend->GetFrameFenceCount());
        terrainStreaming->NotifyContentChanged();
        ApplyTerrainLayerStreamingUploads(*rendererBackend, *terrainStreaming);
    }

    return loadResult;
}

} // namespace VL
