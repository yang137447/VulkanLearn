#include "render/resource/rendererMeshLoader.h"

#include <iostream>
#include <memory>
#include <string>

#include "commonFunction.h"
#include "materialInstance.h"
#include "mesh/loader/common/meshAssetLoader.h"
#include "pipeline/pipelineFactory.h"
#include "render/backend/rendererBackendVulkan.h"
#include "render/resource/rendererMaterialLoader.h"
#include "render/resource/rendererResourceCache.h"
#include "renderGraph.h"
#include "renderableObject.h"
#include "sceneObject.h"

namespace VL
{

RendererMeshLoader::RendererMeshLoader(
    PipelineFactory& pipelineFactory,
    RendererBackendVulkan& rendererBackend)
    : pipelineFactory(pipelineFactory)
    , rendererBackend(rendererBackend)
{
}

void RendererMeshLoader::LoadMeshObject(
    const nlohmann::basic_json<>& node,
    const MeshAssetLoadRequest& meshLoadRequest) const
{
    MeshAssetImportResult importResult = MeshAssetLoader::ImportSections(meshLoadRequest);
    const MeshAssetBuildPlan& buildPlan = importResult.buildPlan;
    const ModelResource& modelResource = importResult.modelResource;
    const std::vector<MeshSectionLoadPlan>& sectionPlans = importResult.sectionPlans;

    auto& renderGraph = RenderGraph::GetInstance();
    RendererResourceCache& resourceCache = RendererResourceCache::GetInstance();
    RendererMaterialLoader materialLoader(pipelineFactory, rendererBackend);

    Eigen::Vector3f position = JsonParser::ParseValue<Eigen::Vector3f>(node["position"]);
    Eigen::Vector3f rotation = JsonParser::ParseValue<Eigen::Vector3f>(node["rotation"]);
    Eigen::Vector3f scale = JsonParser::ParseValue<Eigen::Vector3f>(node["scale"]);
    const std::string sceneObjectBaseName = node["name"];
    const auto& geometryPass = renderGraph.GetRenderpasses().at("geometry");

    for (size_t sectionIndex = 0; sectionIndex < modelResource.sections.size(); ++sectionIndex)
    {
        const MeshSection& section = modelResource.sections[sectionIndex];
        const MeshSectionLoadPlan& sectionPlan = sectionPlans[sectionIndex];
        std::string renderableObjectKey =
            buildPlan.modelCacheKey + "|" + std::to_string(sectionIndex) + "|" + section.sectionName;

        std::shared_ptr<RenderableObject> renderableObject;
        const std::shared_ptr<RenderableObject>* cachedRenderableObject =
            resourceCache.GetRenderableObject(renderableObjectKey);
        if(cachedRenderableObject != nullptr && *cachedRenderableObject != nullptr)
        {
            renderableObject = *cachedRenderableObject;
        }
        else
        {
            renderableObject = std::make_shared<RenderableObject>(
                section.vertices,
                section.indices,
                rendererBackend,
                renderableObjectKey);
            resourceCache.BindRenderableObject(renderableObjectKey, renderableObject);
        }

        if (sectionPlan.bUsesUnsafeFallbackMaterial)
        {
            std::cout << "Unsafe mesh material slot fallback: "
                      << sceneObjectBaseName << "::" << section.sectionName
                      << " reason=" << sectionPlan.unsafeFallbackReason << std::endl;
        }
        std::shared_ptr<MaterialInstance> materialInstance =
            materialLoader.LoadMaterialInstance(sectionPlan.materialInstancePath, geometryPass.sampleCount);

        // Keep one SceneObject per imported section for the current transform
        // export path until mesh scene objects move to a pure World model.
        std::string sceneObjectName = sceneObjectBaseName;
        if (modelResource.sections.size() > 1)
        {
            sceneObjectName += "::" + section.sectionName;
        }
        if(resourceCache.GetSceneObject(sceneObjectName) != nullptr)
        {
            sceneObjectName += "_" + std::to_string(sectionIndex);
        }

        std::shared_ptr<SceneObject> sceneObject = std::make_shared<SceneObject>(renderableObject, materialInstance);
        sceneObject->SetName(sceneObjectName);
        sceneObject->SetPosition(position);
        sceneObject->SetRotation(rotation);
        sceneObject->SetScale(scale);
        sceneObject->UpdateModelMatrix();

        resourceCache.BindSceneObject(sceneObjectName, std::move(sceneObject));
    }
}

} // namespace VL
