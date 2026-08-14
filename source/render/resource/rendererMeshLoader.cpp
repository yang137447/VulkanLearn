#include "render/resource/rendererMeshLoader.h"

#include <memory>
#include <string>
#include <unordered_set>

#include "commonFunction.h"
#include "material.h"
#include "materialInstance.h"
#include "mesh/loader/common/meshAssetLoader.h"
#include "pipeline/pipelineFactory.h"
#include "render/backend/rendererBackendVulkan.h"
#include "render/resource/rendererMaterialLoader.h"
#include "render/resource/rendererResourceCache.h"
#include "render/resource/rendererResourceLoadContext.h"
#include "renderGraph.h"
#include "renderableObject.h"
#include "scene/sceneAssetTypes.h"

namespace VL
{
namespace
{

Eigen::Matrix4f BuildModelMatrix(
    const Eigen::Vector3f& position,
    const Eigen::Vector3f& rotation,
    const Eigen::Vector3f& scale)
{
    Eigen::Matrix4f scaleMatrix;
    scaleMatrix <<
        scale.x(), 0.0f, 0.0f, 0.0f,
        0.0f, scale.y(), 0.0f, 0.0f,
        0.0f, 0.0f, scale.z(), 0.0f,
        0.0f, 0.0f, 0.0f, 1.0f;

    Eigen::Matrix4f translationMatrix;
    translationMatrix <<
        1.0f, 0.0f, 0.0f, position.x(),
        0.0f, 1.0f, 0.0f, position.y(),
        0.0f, 0.0f, 1.0f, position.z(),
        0.0f, 0.0f, 0.0f, 1.0f;

    return translationMatrix *
        CommonFunction::RotationToMatrix(rotation) *
        scaleMatrix;
}

} // namespace

RendererMeshLoader::RendererMeshLoader(
    PipelineFactory& pipelineFactory,
    RendererBackendVulkan& rendererBackend,
    RendererResourceLoadContext& loadContext)
    : pipelineFactory(pipelineFactory)
    , rendererBackend(rendererBackend)
    , loadContext(loadContext)
{
}

RendererMeshLoadResult RendererMeshLoader::LoadMeshObject(
    const nlohmann::basic_json<>& node,
    const MeshAssetLoadRequest& meshLoadRequest) const
{
    MeshAssetImportResult importResult = MeshAssetLoader::ImportSections(meshLoadRequest);
    const MeshAssetBuildPlan& buildPlan = importResult.buildPlan;
    const ModelResource& modelResource = importResult.modelResource;
    const std::vector<MeshSectionLoadPlan>& sectionPlans = importResult.sectionPlans;

    RendererMeshLoadResult result;
    if (modelResource.hasSpeedTreeWind)
    {
        SpeedTreeWindProfile profile;
        profile.key = buildPlan.modelCacheKey;
        profile.sourceBoundsMin = modelResource.speedTreeSourceBoundsMin;
        profile.sourceBoundsMax = modelResource.speedTreeSourceBoundsMax;
        profile.config = modelResource.speedTreeWind;
        result.speedTreeWindProfile = std::move(profile);
    }

    RenderGraph& renderGraph = loadContext.renderGraph;
    RendererResourceCache& resourceCache =
        loadContext.resourceCache;
    RendererMaterialLoader materialLoader(
        pipelineFactory,
        rendererBackend,
        loadContext);

    Eigen::Vector3f position = JsonParser::ParseValue<Eigen::Vector3f>(node["position"]);
    Eigen::Vector3f rotation = JsonParser::ParseValue<Eigen::Vector3f>(node["rotation"]);
    Eigen::Vector3f scale = JsonParser::ParseValue<Eigen::Vector3f>(node["scale"]);
    const std::string meshObjectBaseName = node["name"];
    Renderpass& geometryPass = renderGraph.GetRenderpasses().at("geometry");
    std::vector<MeshObjectBuildPlan>& meshObjectPlans = result.objectPlans;
    meshObjectPlans.reserve(modelResource.sections.size());
    std::unordered_set<std::string> usedObjectNames;

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

        std::shared_ptr<MaterialInstance> materialInstance =
            materialLoader.LoadMaterialInstance(sectionPlan.materialInstancePath, geometryPass);

        std::string meshObjectName = meshObjectBaseName;
        if (modelResource.sections.size() > 1)
        {
            meshObjectName += "::" + section.sectionName;
        }
        if(usedObjectNames.find(meshObjectName) != usedObjectNames.end())
        {
            meshObjectName += "_" + std::to_string(sectionIndex);
        }
        usedObjectNames.insert(meshObjectName);

        MeshObjectBuildPlan meshObjectPlan;
        meshObjectPlan.objectName = meshObjectName;
        meshObjectPlan.debugName = meshObjectName;
        meshObjectPlan.model = BuildModelMatrix(position, rotation, scale);
        meshObjectPlan.localBoundsMin = renderableObject->GetBoundsMin();
        meshObjectPlan.localBoundsMax = renderableObject->GetBoundsMax();
        meshObjectPlan.meshKey = renderableObject->GetName();
        meshObjectPlan.materialKey = materialInstance->GetBaseMaterial().lock()->GetMaterialKey();
        meshObjectPlan.materialInstanceKey = materialInstance->GetName();
        if (result.speedTreeWindProfile.has_value())
        {
            meshObjectPlan.speedTreeWindProfileKey = result.speedTreeWindProfile->key;
        }

        meshObjectPlans.push_back(std::move(meshObjectPlan));
    }

    return result;
}

} // namespace VL
