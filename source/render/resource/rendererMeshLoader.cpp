#include "render/resource/rendererMeshLoader.h"

#include <memory>
#include <string>
#include <unordered_set>

#include "commonFunction.h"
#include "material.h"
#include "material/materialAssetUtils.h"
#include "materialInstance.h"
#include "mesh/loader/common/meshAssetLoader.h"
#include "mesh/validation/meshAssetValidator.h"
#include "mesh/meshCollisionGeometry.h"
#include "hair/tressfxGroupBinding.h"
#include "hair/tressfxGroupSimulation.h"
#include "pipeline/pipelineFactory.h"
#include "render/offline/offlineWorldAssets.h"
#include "render/backend/rendererBackendVulkan.h"
#include "render/resource/rendererMaterialLoader.h"
#include "render/resource/rendererResourceCache.h"
#include "render/resource/rendererResourceLoadContext.h"
#include "renderGraph.h"
#include "render/resource/renderableObjectKey.h"
#include "renderableObject.h"
#include "scene/sceneAssetTypes.h"
#include "animation/skeletalPlayback.h"
#include "scene/placementBinding.h"
#include "shader/build/contentHash.h"

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
    const MeshAssetLoadRequest& meshLoadRequest,
    std::shared_ptr<const MeshAssetImportResult> preparedImport) const
{
    // 缩略图可传入 worker 已完成的纯 CPU 导入；GPU 创建与材质绑定仍只由 GT 候选执行。
    const MeshAssetBuildPlan& requestedPlan = meshLoadRequest.buildPlan;
    const bool tressfxAsset = requestedPlan.assetType == "tressfx";
    std::shared_ptr<const MeshAssetImportResult> imported = std::move(preparedImport);
    TressfxGeometryCache* tressfxGeometryCache = nullptr;
    std::string tressfxGeometryKey;
    if (tressfxAsset)
    {
        // 几何缓存键 = 几何身份 + .tfx 字节 + 描述符字节；groups 不参与。
        // 组编辑因此命中同一份 ribbon/root cloud，只重建绑定与 group buffer。
        const std::string tfxDigest = VL::ContentHasher::HashFile(
            meshLoadRequest.importRequest.absoluteModelPath).ToHex();
        tressfxGeometryKey = MakeTressfxGeometryCacheKey(
            requestedPlan.modelCacheKey, tfxDigest);
        tressfxGeometryCache = &loadContext.resourceCache.GetTressfxGeometryCache();
        if (!imported)
            imported = tressfxGeometryCache->Find(tressfxGeometryKey);
    }
    if (!imported && loadContext.offlineWorldAssets != nullptr)
    {
        auto& imports = loadContext.offlineWorldAssets->meshImports;
        const auto found = imports.find(requestedPlan.meshAssetPath);
        if (found != imports.end())
            imported = found->second;
        else
        {
            imported = std::make_shared<const MeshAssetImportResult>(
                MeshAssetLoader::ImportSections(meshLoadRequest));
            imports.emplace(requestedPlan.meshAssetPath, imported);
        }
    }
    if (!imported)
    {
        imported = std::make_shared<const MeshAssetImportResult>(
            MeshAssetLoader::ImportSections(meshLoadRequest));
    }
    if (tressfxGeometryCache)
        tressfxGeometryCache->Store(tressfxGeometryKey, imported);
    const MeshAssetImportResult& importResult = *imported;
    // 缓存条目的 buildPlan 可能来自旧请求：几何身份（含 tfx digest）沿用，
    // 但 groups / 半径 / 资产 simulation defaults 必须取当前描述符。
    MeshAssetBuildPlan buildPlan = importResult.buildPlan;
    if (tressfxAsset)
    {
        // 缓存条目只拥有几何；路径、材质槽和 simulation 都属于当前描述符。
        buildPlan.meshAssetPath = requestedPlan.meshAssetPath;
        buildPlan.modelDataPath = requestedPlan.modelDataPath;
        buildPlan.materialSlots = requestedPlan.materialSlots;
        buildPlan.tressfxGuideGroups = requestedPlan.tressfxGuideGroups;
        buildPlan.tressfxGroupBindingKey = requestedPlan.tressfxGroupBindingKey;
        buildPlan.tressfxSimulation = requestedPlan.tressfxSimulation;
        buildPlan.strandRadius = requestedPlan.strandRadius;
    }
    const ModelResource& modelResource = importResult.modelResource;
    if (buildPlan.assetType == "tressfx" && loadContext.offlineOnly)
        throw std::runtime_error("TressFX strands require the realtime vertex factory; offline ribbon expansion is not implemented");
    if (modelResource.skeletalAsset && loadContext.offlineOnly)
        throw std::runtime_error("Skeletal meshes require realtime rendering; offline skinning is not implemented");
    auto animation = modelResource.skeletalAsset
        ? std::make_shared<Animation::SkeletalPlayback>(modelResource.skeletalAsset) : nullptr;
    // 缓存键只描述几何；材质槽/组/放置属于当前描述符，命中时必须按当前 plan
    // 重算 sectionPlans，不能复用缓存条目的旧 sectionPlans。
    const std::vector<MeshSectionLoadPlan> sectionPlans = tressfxAsset
        ? MeshAssetValidator::BuildSectionLoadPlans(buildPlan, modelResource)
        : importResult.sectionPlans;
    MeshAssetLoadRequest placementRequest = meshLoadRequest;
    if (buildPlan.assetType == "tressfx" && !placementRequest.tressfxEffective)
        MeshAssetLoader::PrepareTressfxPlacement(placementRequest, node);
    MeshAssetLoader::ValidateTressfxPlacementSources(placementRequest);
    VL::PlacementBinding placementBinding = VL::ReadPlacementBindingFromComponent(node);
    placementBinding.tressfx = placementRequest.tressfxEffective;
    placementBinding.tressfxGroupConfigs = placementRequest.tressfxGroupConfigs;
    const TressfxSimulationConfig* effective = placementRequest.tressfxEffective
        ? &*placementRequest.tressfxEffective : nullptr;

    RendererMeshLoadResult result;
    if (modelResource.hasSpeedTreeWind)
    {
        WindResponseProfile profile;
        profile.key = buildPlan.modelCacheKey;
        profile.sourceBoundsMin = modelResource.speedTreeSourceBoundsMin;
        profile.sourceBoundsMax = modelResource.speedTreeSourceBoundsMax;
        profile.config = modelResource.speedTreeWind;
        result.windResponseProfile = std::move(profile);
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
    std::vector<MeshObjectBuildPlan>& meshObjectPlans = result.objectPlans;
    meshObjectPlans.reserve(modelResource.sections.size());
    std::unordered_set<std::string> usedObjectNames;

    // 组声明只用于绑定展开；几何/点云来自缓存，不随组编辑变化。
    std::vector<VL::Hair::TressfxGroupDeclaration> groupDeclarations;
    if (tressfxAsset)
    {
        groupDeclarations.reserve(buildPlan.tressfxGuideGroups.size());
        for (const TressfxGuideGroupPlan& group : buildPlan.tressfxGuideGroups)
        {
            VL::Hair::TressfxGroupDeclaration declaration;
            declaration.name = group.name;
            declaration.ranges = group.guideRanges;
            groupDeclarations.push_back(std::move(declaration));
        }
    }
    // 解析结果按 placement 冻结一次；所有 section 与编辑器快照共享同一份只读数组。
    std::shared_ptr<const std::vector<TressfxSimulationConfig>> groupConfigs;
    if (!placementRequest.tressfxGroupConfigs.empty())
        groupConfigs = std::make_shared<const std::vector<TressfxSimulationConfig>>(
            placementRequest.tressfxGroupConfigs);

    for (size_t sectionIndex = 0; sectionIndex < modelResource.sections.size(); ++sectionIndex)
    {
        const MeshSection& section = modelResource.sections[sectionIndex];
        const MeshSectionLoadPlan& sectionPlan = sectionPlans[sectionIndex];
        std::shared_ptr<const VL::Hair::TressfxGroupBinding> groupBinding;
        if (section.isTressfx)
        {
            const uint32_t guideCount = section.tressfxGuideCount != 0
                ? section.tressfxGuideCount
                : static_cast<uint32_t>(section.tressfxRootPositions.size());
            groupBinding = VL::Hair::BuildTressfxGroupBinding(groupDeclarations,
                guideCount, buildPlan.tressfxGroupBindingKey,
                " (" + buildPlan.meshAssetPath + ", " + buildPlan.modelDataPath + ")");
        }
        // 覆盖属于场景实例，不能修改按 SM 路径共享的导入缓存或源资产槽位。
        const std::string materialPath = node.value("materialOverride", sectionPlan.materialInstancePath);
        // CPU/GPU 模拟和 off 骨骼跟随都有可变位置，必须按 placement 独占对象。
        // 有效配置与不可变输入摘要也进入键，模板不能把另一放置的行为带进来。
        const bool perPlacementState = effective &&
            (effective->enabled || effective->skinning.has_value());
        // 组 overrides 不再进入 renderable 键：同一对象通过 Update 原地刷新
        // group 参数；放置行为仍按值区分，per-placement 作用域保证不串用。
        const std::string configurationKey = effective
            ? "|behavior=" + Hair::MakeTressfxBehaviorKey(*effective) +
                (placementRequest.tressfxResources ? placementRequest.tressfxResources->identity : "")
            : std::string();
        // 组名、区间和组数都不进入 renderable 键：GPU 颜色/参数表按预留容量
        // 原地更新。组编辑或保存后的组数变化复用同一个 RenderableObject。
        std::string renderableObjectKey = MakeRenderableObjectKey(
            buildPlan.modelCacheKey + configurationKey, sectionIndex,
            section.sectionName, meshObjectBaseName, perPlacementState);
        if (loadContext.offlineWorldAssets != nullptr)
        {
            loadContext.offlineWorldAssets->sections.emplace(
                renderableObjectKey,
                OfflineWorldAssets::Section{imported, sectionIndex});
        }

        std::shared_ptr<RenderableObject> renderableObject;
        if (!loadContext.offlineOnly)
        {
        const std::shared_ptr<RenderableObject>* cachedRenderableObject =
            resourceCache.GetRenderableObject(renderableObjectKey);
        if(cachedRenderableObject != nullptr && *cachedRenderableObject != nullptr)
        {
            renderableObject = *cachedRenderableObject;
            if (groupBinding)
            {
                const std::string groupSimulationKey =
                    Hair::MakeTressfxGroupSimulationKey(
                        placementRequest.tressfxGroupConfigs);
                if (renderableObject->GetTressfxGroupBindingKey() != groupBinding->key ||
                    renderableObject->GetTressfxGroupSimulationKey() != groupSimulationKey)
                {
                    std::string updateError;
                    if (!renderableObject->UpdateTressfxGroupBinding(
                            *groupBinding, placementRequest.tressfxGroupConfigs,
                            updateError))
                        throw std::runtime_error(
                            "TressFX group binding update failed: " + updateError);
                }
            }
        }
        else
        {
            renderableObject = std::make_shared<RenderableObject>(
                imported,
                sectionIndex,
                rendererBackend,
                renderableObjectKey, effective, placementRequest.tressfxResources,
                &placementRequest.tressfxGroupConfigs, groupBinding);
            resourceCache.BindRenderableObject(renderableObjectKey, renderableObject);
        }
        }

        std::shared_ptr<MaterialInstance> materialInstance =
            materialLoader.LoadSceneMaterialInstance(
                materialPath);
        if (materialInstance)
        {
            const bool skinMaterial = HasMaterialMacroValue(
                materialInstance->GetBaseMaterial().lock()->GetShaderVariantKey().macros,
                "VL_SKINNED_MESH", "1");
            if (skinMaterial != static_cast<bool>(animation))
                throw std::runtime_error("Mesh and material vertex factory mismatch: " + materialPath);
            const bool strandMaterial = HasMaterialMacroValue(
                materialInstance->GetBaseMaterial().lock()->GetShaderVariantKey().macros,
                "VL_TRESSFX_STRANDS", "1");
            if (strandMaterial != (buildPlan.assetType == "tressfx"))
                throw std::runtime_error("TressFX mesh and material vertex factory mismatch: " + materialPath);
        }

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
        meshObjectPlan.sceneObjectIdentity = meshObjectBaseName;
        meshObjectPlan.materialSlotIndex = sectionPlan.materialSlotIndex;
        meshObjectPlan.materialSlotName = sectionPlan.materialSlotName;
        meshObjectPlan.debugName = meshObjectName;
        meshObjectPlan.model = BuildModelMatrix(position, rotation, scale);
        meshObjectPlan.scenePosition = position;
        meshObjectPlan.sceneRotation = rotation;
        meshObjectPlan.sceneScale = scale;
        meshObjectPlan.localBoundsMin = section.vertices.front().position;
        meshObjectPlan.localBoundsMax = section.vertices.front().position;
        // 普通渲染网格不额外保留展开三角；仅蓝图的已导入 mesh 用于显式 CPU mesh collision。
        if (meshObjectBaseName.rfind("__blueprint/", 0) == 0)
            meshObjectPlan.collisionTriangles = FreezeMeshCollisionTriangles(section);
        for (const auto& vertex : section.vertices)
        {
            meshObjectPlan.localBoundsMin = meshObjectPlan.localBoundsMin.cwiseMin(vertex.position);
            meshObjectPlan.localBoundsMax = meshObjectPlan.localBoundsMax.cwiseMax(vertex.position);
        }
        meshObjectPlan.localBoundsMin.array() -= section.boundsPadding;
        meshObjectPlan.localBoundsMax.array() += section.boundsPadding;
        meshObjectPlan.meshKey = renderableObjectKey;
        meshObjectPlan.tressfxPointsPerGuide =
            section.isTressfx ? section.tressfxPointsPerStrand : 0;
        meshObjectPlan.tressfxRootCloud = section.tressfxRootCloud;
        meshObjectPlan.tressfxGroupBinding = groupBinding;
        meshObjectPlan.tressfxGroupConfigs = groupConfigs;
        meshObjectPlan.tressfxBoneAsset = placementRequest.tressfxResources
            ? placementRequest.tressfxResources->boneAsset : nullptr;
        meshObjectPlan.placementBinding = placementBinding;
        if (animation)
        {
            meshObjectPlan.animation = animation;
            meshObjectPlan.skinSection = sectionIndex;
            meshObjectPlan.skinPose = animation->Pose(sectionIndex);
            meshObjectPlan.localBoundsMin = meshObjectPlan.skinPose->boundsMin;
            meshObjectPlan.localBoundsMax = meshObjectPlan.skinPose->boundsMax;
        }
        if (materialInstance)
        {
            meshObjectPlan.materialKey = materialInstance->GetBaseMaterial().lock()->GetMaterialKey();
            meshObjectPlan.materialInstanceKey = materialInstance->GetName();
        }
        else
        {
            // 逻辑身份不代表 GPU Material；离线路径不会 resolve GPU groups。
            meshObjectPlan.materialInstanceKey =
                MaterialAssetUtils::NormalizeAssetPath(materialPath);
            meshObjectPlan.materialKey =
                loadContext.offlineWorldAssets->effectiveMaterialInstances.at(
                    meshObjectPlan.materialInstanceKey).at("material").get<std::string>();
        }
        if (result.windResponseProfile.has_value())
        {
            meshObjectPlan.windResponseProfileKey = result.windResponseProfile->key;
        }

        meshObjectPlans.push_back(std::move(meshObjectPlan));
    }

    // 保留导入结果供跨对象解析（collision.mesh 的权重转移读取源骨骼 section），
    // 不复制几何；World 资源包与 renderable 共享同一份导入数据。
    result.importResult = imported;
    return result;
}

} // namespace VL
