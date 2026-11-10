#include "render/resource/rendererResourceLoadCoordinator.h"

#include <algorithm>
#include <iostream>
#include <iterator>
#include <stdexcept>
#include <string>
#include <utility>

#include <nlohmann/json.hpp>

#include "commonFunction.h"
#include "render/backend/rendererBackendVulkan.h"
#include "animation/skeletalPlayback.h"
#include "hair/tressfxProxySkinning.h"
#include "hair/tressfxRootWeights.h"
#include "hair/tressfxSkinning.h"
#include "mesh/loader/common/meshAssetLoader.h"
#include "scene/placementBinding.h"
#include "commonFunction.h"
#include "material.h"
#include "materialInstance.h"
#include "render/resource/rendererEnvironmentLoader.h"
#include "render/resource/rendererMaterialLoader.h"
#include "render/resource/rendererMeshLoader.h"
#include "render/resource/rendererResourceCache.h"
#include "render/resource/rendererResourceLoadContext.h"
#include "render/resource/terrainFeedbackGpu.h"
#include "render/resource/terrainLayerFeedbackGpu.h"
#include "render/resource/terrainPageBakeGpu.h"
#include "render/resource/rendererTerrainLoader.h"
#include "terrain/terrainPageManager.h"
#include "terrain/terrainLayerRegistry.h"
#include "terrain/terrainLayerTable.h"
#include "renderGraph.h"
#include "renderableObject.h"
#include "world/loading/worldLoader.h"

namespace VL
{
namespace
{

// 几何 chunk 驻留的初始窗口来自作者相机。加载后才执行的 `camera pose`
// 不会改变窗口：第一版没有运行时异步重传，窗口外的细 LOD 永远不存在，
// 运行时只会在“目标级缺失”时回退到 resident 的更粗一级。
TerrainChunkResidencyDesc ReadInitialTerrainChunkResidency(
    const WorldBuildPlan& worldBuildPlan)
{
    TerrainChunkResidencyDesc residency;
    const nlohmann::json& sceneJson = worldBuildPlan.sceneJson;
    if (!sceneJson.is_object() || !sceneJson.contains("objects") ||
        !sceneJson.at("objects").is_array())
    {
        return residency;
    }
    for (const nlohmann::json& objectJson : sceneJson.at("objects"))
    {
        if (!objectJson.is_object() ||
            !objectJson.contains("type") ||
            !objectJson.at("type").is_string() ||
            objectJson.at("type").get<std::string>() != "camera" ||
            !objectJson.contains("position"))
        {
            continue;
        }
        residency.cameraPositionCm =
            JsonParser::ParseValue<Eigen::Vector3f>(
                objectJson.at("position"));
        residency.cameraKnown = true;
        break;
    }
    return residency;
}

// 每个场景 mesh 对象的加载结果先保留到本地，等全部对象创建完成后再解析
// 放置级绑定；不要在材质/网格加载中途读尚未创建的源对象。
struct LoadedSceneMesh
{
    std::string objectName;
    const MeshAssetLoadRequest* request = nullptr;
    RendererMeshLoadResult result;
    // 通用放置级绑定：蓝图组件声称的绑定意图（Kind + 参数）。空表示没有放置级
    // 绑定，绑定由描述符自己声明（例如 TressFX 的 simulation.skinning.source）。
    VL::PlacementBinding placementBinding;
};

// Hair 与源角色都必须处于同一对象空间：骨骼矩阵把绑定空间映射到源角色的模型空间，
// 发丝顶点却按自己的 model 绘制。两个场景对象的 model 不同就要在加载期拒绝，
// 不能让错误的根点位置进入 SSBO。
bool ModelMatricesMatch(const Eigen::Matrix4f& left, const Eigen::Matrix4f& right)
{
    return (left - right).cwiseAbs().maxCoeff() <= 1.0e-4f;
}

// 身体侧蒙皮数据：位置 + CSR 影响。`collision.mesh` 代理与发丝根点派生权重
// 共用同一份组装（同一绑定姿态、同一 `meshToModel`），避免两条路径各写一份
// 空间变换；身体顶点必须与查询点处于同一“渲染绑定空间”。
struct TressfxBodySkinData
{
    std::vector<Eigen::Vector3f> positions;
    std::vector<uint32_t> offsets;
    std::vector<VL::Hair::SdfProxySkinSourceInfluence> influences;
};

void BuildTressfxBodySkinData(
    const Animation::SkeletalAsset& asset, TressfxBodySkinData& out)
{
    out.offsets.push_back(0);
    for (const Animation::SkinSection& section : asset.mesh.sections)
    {
        if (section.influenceOffsets.size() != section.vertices.size() + 1)
            throw std::runtime_error(
                "TressFX proxy skin transfer source section has an invalid influence CSR: " +
                section.name);
        for (size_t vertex = 0; vertex < section.vertices.size(); ++vertex)
        {
            out.positions.push_back((section.meshToModel *
                section.vertices[vertex].position.homogeneous()).head<3>());
            for (uint32_t influence = section.influenceOffsets[vertex];
                 influence < section.influenceOffsets[vertex + 1]; ++influence)
            {
                const Animation::Influence& source = section.influences.at(influence);
                out.influences.push_back({source.joint, source.weight});
            }
            out.offsets.push_back(static_cast<uint32_t>(out.influences.size()));
        }
    }
}

// 代理权重只在加载期转移一次；2 cm 上限来自正式代理实测（最近身体顶点最大
// 0.93 cm）。超过上限说明 collision.mesh 与蒙皮身体不是同一绑定空间或不是同一
// 角色，必须在加载期拒绝而不是把错位权重烧进 SSBO。
constexpr float TressfxProxySkinMaxDistanceCm = 2.0f;

// 发丝根点派生权重的上限与代理同量级：正式资产 40,334 个发根到最近身体顶点的
// 实测最大值为 0.516 cm。超距只作为“毛发与身体不在同一绑定空间”的检测器，
// 不承担“头皮顶点稀疏”的判断（稀疏只影响量化误差，不影响可匹配性）。
constexpr float TressfxRootWeightMaxDistanceCm = 2.0f;

std::shared_ptr<const VL::Hair::SdfProxySkinTransfer> BuildProxySkinTransfer(
    const LoadedSceneMesh& hair, const LoadedSceneMesh& source)
{
    if (!hair.result.importResult || !source.result.importResult ||
        !source.result.importResult->modelResource.skeletalAsset)
        return nullptr;
    const VL::Hair::SdfTriangleMesh* proxy = hair.request->tressfxResources
        ? hair.request->tressfxResources->collisionMesh.get() : nullptr;
    if (proxy == nullptr)
        return nullptr;

    TressfxBodySkinData body;
    const Animation::SkeletalAsset& skeletal =
        *source.result.importResult->modelResource.skeletalAsset;
    BuildTressfxBodySkinData(skeletal, body);
    const VL::Hair::SdfProxySkinSource view{
        body.positions.data(), body.positions.size(), body.offsets.data(),
        body.influences.data(), static_cast<uint32_t>(skeletal.mesh.joints.size())};
    return std::make_shared<const VL::Hair::SdfProxySkinTransfer>(
        VL::Hair::BuildTressfxProxySkinTransfer(proxy->positions, view,
            TressfxProxySkinMaxDistanceCm));
}

// 发丝根点只在导入期保留一份 model 空间副本（GPU 位置 buffer 是 device-local，
// 运行期不可回读）；派生逐 strand 权重必须从这里取，不重建几何。
const std::vector<Eigen::Vector3f>* HairRootPositions(const LoadedSceneMesh& hair)
{
    if (!hair.result.importResult)
        return nullptr;
    for (const MeshSection& section : hair.result.importResult->modelResource.sections)
        if (section.isTressfx && !section.tressfxRootPositions.empty())
            return &section.tressfxRootPositions;
    return nullptr;
}

// 通用放置绑定解析入口：按 Kind 分派到具体 handler。目前唯一 handler 是 TressFX
// 毛发（它同时处理组件绑定和描述符 simulation.skinning.source 两种来源）。
// 新增一种 PlacementBinding::Kind 时：
//   1. 在下面的 kind 校验里放行；
//   2. 写一个同签名的 handler（或把现有 handler 拆成按对象处理的函数）；
//   3. 在这里按 kind 调用。
// 不要在数据通路（scene/World/snapshot/packet）里加具体类型的分支。
void ResolveTressfxPlacementBindings(
    const std::vector<LoadedSceneMesh>& loadedMeshes,
    RendererResourceCache& resourceCache,
    const std::string& scenePath);

void ResolvePlacementBindings(
    const std::vector<LoadedSceneMesh>& loadedMeshes,
    RendererResourceCache& resourceCache,
    const std::string& scenePath)
{
    for (const LoadedSceneMesh& mesh : loadedMeshes)
        if (!mesh.placementBinding.IsEmpty() &&
            mesh.placementBinding.kind != VL::PlacementBinding::Kind::TressfxSkinning)
            throw std::runtime_error(
                "Unsupported placement binding kind for object \"" + mesh.objectName + "\"");
    ResolveTressfxPlacementBindings(loadedMeshes, resourceCache, scenePath);
}

void ResolveTressfxPlacementBindings(
    const std::vector<LoadedSceneMesh>& loadedMeshes,
    RendererResourceCache& resourceCache,
    const std::string& scenePath)
{
    for (const LoadedSceneMesh& hair : loadedMeshes)
    {
        const MeshAssetBuildPlan& hairPlan = hair.request->buildPlan;
        const TressfxSimulationConfig& hairConfig = hair.request->tressfxEffective
            ? *hair.request->tressfxEffective : hairPlan.tressfxSimulation;
        if (hairPlan.assetType != "tressfx" || !hairConfig.skinning)
            continue;
        const TressfxSimulationSkinning& requested = *hairConfig.skinning;

        // 绑定来源：蓝图 tressfx 组件的父 mesh 身份优先；场景路径仍读
        // simulation.skinning.source。两者都没有时不能在运行期猜。
        const bool componentBound =
            hair.placementBinding.kind == VL::PlacementBinding::Kind::TressfxSkinning &&
            !hair.placementBinding.sourceSceneObject.empty();
        const std::string sourceName = componentBound
            ? hair.placementBinding.sourceSceneObject : requested.source;
        if (sourceName.empty())
            throw std::runtime_error(
                "TressFX simulation.skinning has no binding source: set "
                "simulation.skinning.source for scene hair or mount the hair through a "
                "Blueprint TressFX component: " + hair.objectName + " " +
                hairPlan.meshAssetPath);
        const std::string rootBone = hair.placementBinding.rootBone.empty()
            ? requested.rootBone : hair.placementBinding.rootBone;

        const LoadedSceneMesh* source = nullptr;
        for (const LoadedSceneMesh& candidate : loadedMeshes)
        {
            if (candidate.objectName == sourceName)
            {
                source = &candidate;
                break;
            }
        }
        if (source == nullptr)
            throw std::runtime_error(
                "TressFX simulation.skinning source object \"" + sourceName +
                "\" not found in scene " + scenePath + ": " + hairPlan.meshAssetPath);

        // 只接受同一场景内正式加载的蒙皮对象；动画源/目标骨骼的权威下标来自
        // 目标 SkeletalAsset::skeleton，与 SkinPose::matrices 使用同一关节顺序。
        const Animation::SkeletalPlayback* playback = nullptr;
        for (const MeshObjectBuildPlan& plan : source->result.objectPlans)
        {
            if (plan.animation)
            {
                playback = plan.animation.get();
                break;
            }
        }
        if (playback == nullptr || !playback->Asset().HasSkeleton())
            throw std::runtime_error(
                "TressFX simulation.skinning source object \"" + sourceName +
                "\" has no skeleton (SK): " + hairPlan.meshAssetPath);

        std::vector<std::string> jointNames;
        jointNames.reserve(playback->Asset().skeleton.joints.size());
        for (const Animation::SkeletonJoint& joint : playback->Asset().skeleton.joints)
            jointNames.push_back(joint.name);
        const uint32_t jointIndex = VL::Hair::ResolveTressfxRootJoint(
            jointNames, sourceName, rootBone, hairPlan.meshAssetPath);

        if (hair.result.objectPlans.empty() || source->result.objectPlans.empty())
            throw std::runtime_error(
                "TressFX simulation.skinning requires loaded hair and source sections: " +
                hairPlan.meshAssetPath);
        if (!ModelMatricesMatch(
                hair.result.objectPlans.front().model,
                source->result.objectPlans.front().model))
            throw std::runtime_error(
                "TressFX simulation.skinning requires hair and source object to share the "
                "same object space (both scene model matrices must match): hair=\"" +
                hair.objectName + "\" source=\"" + sourceName + "\": " +
                hairPlan.meshAssetPath);

        VL::Hair::TressfxSkinningBinding binding;
        binding.enabled = true;
        binding.sourceSceneObject = sourceName;
        binding.rootBone = rootBone;
        binding.jointIndex = jointIndex;
        // 胶囊骨骼与根骨共用同一个源对象/骨架；每个胶囊独立解析，未知骨在加载期
        // 硬错误（消息带胶囊下标），没有 bone 的槽保留 -1 的静态行为。
        binding.capsuleJointIndices.assign(
            hairConfig.capsules.size(), -1);
        for (size_t capsuleIndex = 0;
             capsuleIndex < hairConfig.capsules.size(); ++capsuleIndex)
        {
            const VL::Hair::TressfxCollider& capsule =
                hairConfig.capsules[capsuleIndex];
            if (capsule.bone.empty())
                continue;
            binding.capsuleJointIndices[capsuleIndex] = static_cast<int32_t>(
                VL::Hair::ResolveTressfxJointByName(jointNames, sourceName,
                    capsule.bone,
                    "collision.capsules[" + std::to_string(capsuleIndex) + "].bone",
                    hairPlan.meshAssetPath));
        }
        // 权重来源：组件显式值 > SM 显式 weights > 组件绑定默认 mesh。mesh 派生与
        // 显式 .tfxbone 同时出现按作者冲突拒绝，不做运行期择一。
        const int weightChoice = VL::Hair::ResolveTressfxRootWeightChoice(false,
            requested.rootWeights == TressfxRootWeightSource::Mesh,
            requested.rootWeightsAuthored, hair.placementBinding.weights,
            !requested.boneWeightsPath.empty());
        if (weightChoice < 0)
            throw std::runtime_error(
                "TressFX root weights conflict: \"mesh\" derivation cannot be combined "
                "with simulation.skinning.boneWeights: " + hairPlan.meshAssetPath);
        std::cout << "[TressFX] hair binding: object=" << hair.objectName
                  << " source=" << sourceName
                  << " binding=" << (componentBound ? "component" : "scene")
                  << " weights=" << (weightChoice == 1 ? "mesh" :
                      (!requested.boneWeightsPath.empty() ? "file" : "bone"))
                  << " rootBone=" << rootBone << std::endl;
        // 可选 .tfxbone：在加载期把逐 strand 骨名映射到目标骨架；运行期只上传
        // 归一化后的 joint/weight，不再读文件。未知骨按源行为跳过并记录。
        if (!requested.boneWeightsPath.empty() &&
            hair.result.objectPlans.front().tressfxBoneAsset)
        {
            const std::shared_ptr<RenderableObject>* firstRenderable =
                resourceCache.GetRenderableObject(hair.result.objectPlans.front().meshKey);
            if (firstRenderable == nullptr || *firstRenderable == nullptr)
                throw std::runtime_error(
                    "TressFX simulation.skinning.boneWeights: hair renderable was not created: " +
                    hairPlan.meshAssetPath);
            std::vector<std::string> unmappedBones;
            auto mapped = VL::Hair::MapTfxBoneToSkeleton(
                *hair.result.objectPlans.front().tressfxBoneAsset, jointNames,
                (*firstRenderable)->GetTressfxGuideCount(), unmappedBones,
                requested.boneWeightsPath);
            if (mapped.IsFailure())
                throw std::runtime_error(
                    "TressFX simulation.skinning.boneWeights failed: " +
                    VL::FormatRuntimeError(mapped.Error()));
            for (const std::string& bone : unmappedBones)
                std::cerr << "[TressFX] boneWeights: bone \"" << bone
                          << "\" is not in the source skeleton and was skipped: "
                          << hairPlan.meshAssetPath << std::endl;
            binding.strandBones = std::move(mapped.Value());
            std::cout << "[TressFX] boneWeights: strands="
                      << binding.strandBones.size() << " joints="
                      << jointNames.size() << " path="
                      << requested.boneWeightsPath << std::endl;
        }
        // 根点权重从绑定姿态网格派生：每个 guide 根点复制最近身体顶点的完整
        // 影响列表并归一化。个别根超距按根回退 rootBone（RecordUnmatched），
        // 全部超距说明毛发与 source 不在同一绑定空间，加载期硬错误。
        if (weightChoice == 1)
        {
            const std::vector<Eigen::Vector3f>* roots = HairRootPositions(hair);
            if (roots == nullptr || roots->empty())
                throw std::runtime_error(
                    "TressFX simulation.skinning.weights = \"mesh\" requires imported "
                    "guide roots: " + hairPlan.meshAssetPath);
            TressfxBodySkinData body;
            BuildTressfxBodySkinData(playback->Asset(), body);
            const VL::Hair::SdfProxySkinSource view{
                body.positions.data(), body.positions.size(), body.offsets.data(),
                body.influences.data(), static_cast<uint32_t>(jointNames.size())};
            const VL::Hair::TressfxRootWeightInput weightInput{
                roots, &view, TressfxRootWeightMaxDistanceCm};
            auto derived = VL::Hair::BuildTressfxRootWeights(weightInput);
            if (derived.IsFailure())
            {
                // 既有语义保持不变：全部根点未匹配 = 毛发与 source 不在同一绑定
                // 空间，用原文案硬错误；输入/CSR 损坏属于资产缺陷，报原始消息。
                if (derived.Error().code == "TressFXRootWeights.NoMatch")
                    throw std::runtime_error(
                        "TressFX simulation.skinning.weights = \"mesh\" matched no guide root "
                        "to the source mesh within " +
                        std::to_string(TressfxRootWeightMaxDistanceCm) + " cm: " +
                        hairPlan.meshAssetPath);
                throw std::runtime_error(
                    "TressFX simulation.skinning.weights = \"mesh\" failed: " +
                    VL::FormatRuntimeError(derived.Error()) + ": " +
                    hairPlan.meshAssetPath);
            }
            // 未匹配根留空权重，SetTressfxSkinningBinding 会按根回退 rootBone；
            // 这里只报告数量，不静默把空权重变成零矩阵。
            binding.strandBones = std::move(derived.Value().strandBones);
            std::cout << "[TressFX] rootWeights: strands=" << binding.strandBones.size()
                      << " joints=" << jointNames.size()
                      << " maxNearest=" << derived.Value().maxNearestDistanceCm
                      << " fallback=" << derived.Value().unmatchedCount
                      << std::endl;
        }
        for (const MeshObjectBuildPlan& plan : hair.result.objectPlans)
        {
            const std::shared_ptr<RenderableObject>* renderable =
                resourceCache.GetRenderableObject(plan.meshKey);
            if (renderable == nullptr || *renderable == nullptr)
                throw std::runtime_error(
                    "TressFX simulation.skinning: renderable \"" + plan.debugName +
                    "\" was not created: " + hairPlan.meshAssetPath);
            (*renderable)->SetTressfxSkinningBinding(binding);
        }

        // `collision.mesh` 与 skinning 同时存在时，代理改用逐顶点多骨权重。
        // 转移失败/距离超限在加载期硬错误；没有 collision.mesh 的对象不创建
        // 权重 buffer，SDF splat 保持单骨根矩阵回退。
        if (hairConfig.enabled && hairConfig.collisionMesh)
        {
            const std::shared_ptr<const VL::Hair::SdfProxySkinTransfer> transfer =
                BuildProxySkinTransfer(hair, *source);
            for (const MeshObjectBuildPlan& plan : hair.result.objectPlans)
            {
                const std::shared_ptr<RenderableObject>* renderable =
                    resourceCache.GetRenderableObject(plan.meshKey);
                if (transfer && renderable != nullptr && *renderable != nullptr)
                    (*renderable)->SetTressfxSdfProxySkin(transfer);
            }
        }
    }
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
    terrainStreaming->layerFeedbackRing =
        std::make_shared<TerrainLayerFeedbackGpuRing>(*rendererBackend);
    // S1：先在整个 World 上建立全局层注册表。多个 TR 可以有不同层定义；
    // 同 key 且参数一致的去重成同一个 globalId，冲突在注册任何页之前失败。
    TerrainLayerRegistry layerRegistry;
    std::vector<const TerrainBuildPlan*> layerTerrains;
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
        layerRegistry.RegisterTerrain(terrainPlan);
        layerTerrains.push_back(&terrainPlan);
    }
    const std::vector<TerrainLayerDesc> worldTerrainLayers =
        layerRegistry.LayerDescriptions();
    const bool hasTerrainLayers = !layerTerrains.empty();
    for (const TerrainBuildPlan* terrainPlan : layerTerrains)
    {
        const std::vector<uint8_t>* layerIdMap =
            layerRegistry.FindLayerIdMap(TerrainPlacementIdentity(*terrainPlan));
        // P3：不再全量 preload。注册只建立虚拟页/槽位身份，驻留由初始集合
        // 与 feedback 驱动；否则 32×32 地形会在加载期烘焙所有页并互相淘汰。
        terrainPageManager.RegisterTerrain(
            *terrainPlan,
            layerIdMap == nullptr ? std::vector<uint8_t>() : *layerIdMap,
            false);
    }
    if (!terrainPageManager.Registrations().empty())
    {
        // 驻留预算必须覆盖整个世界，否则 LRU 会在世界内部切出一条直线：
        // 一侧的页常驻、另一侧永远缺页（实测 4×4 tile × 8×8 页 = 1024 页，
        // 而默认预算只有 512，近半张图的层权重读成 0）。
        // 上限是全部物理槽位数（1024）。
        uint32_t registeredPages = 0;
        for (const TerrainPageManager::Registration& registration :
             terrainPageManager.Registrations())
        {
            registeredPages +=
                registration.tilesPerSide * registration.tilesPerSide;
        }
        terrainPageManager.SetResidentCapacity(
            ComputeTerrainResidentCapacity(
                registeredPages,
                TerrainPageManager::DefaultResidentCapacity,
                TerrainPageManager::UsablePhysicalSlotCount));
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

        // CPU 初始页上传完成后打开 GPU 烘焙：之后所有 RequestPageAtEpoch
        // 只记录 job，渲染线程不再同步烘焙 128² 层权重。
        terrainStreaming->pageBake =
            std::make_shared<TerrainPageBakeGpu>(
                *rendererBackend,
                *pipelineFactory,
                terrainPageManager);
        if (terrainStreaming->pageBake->Available())
        {
            terrainPageManager.SetGpuPageBakeEnabled(true);
        }
        else
        {
            std::cerr
                << "[Terrain] Falling back to CPU page bake: "
                << terrainStreaming->pageBake->UnavailableReason()
                << std::endl;
        }
    }
    if (hasTerrainLayers)
    {
        terrainStreaming->layerDefinitions = worldTerrainLayers;
        // S1 统计：registry 已把跨 TR 同 key 的层去重；这个数字是省下的
        // 局部层总数，供 terrainlayerstats 验证去重效果。
        terrainStreaming->layerDedupedLayerCount =
            layerRegistry.DedupedLayerCount();
        // 层页虚拟编号依赖各层源分辨率，必须在创建页表/请求页之前配置。
        terrainStreaming->layerPageManager.ConfigureLayers(worldTerrainLayers);
        // 与覆盖页同一条规则：驻留预算必须覆盖整个世界的层页虚拟页数，
        // 否则 LRU 会在世界内部切出一条边界，界外的层页永远“请求 -> 淘汰 ->
        // 再请求”。上限是层页管理器的可用物理槽位（fallback 槽位除外）。
        // 全 1K 层时每层 mip0..3 = 85 页：4 层 340 页仍取下限 512；
        // 2K 层（341 页/层）或更多层会把容量抬到物理上限，再由 mip 预算在
        // 容量内做粗化分配，而不是靠淘汰追尾。
        uint32_t layerVirtualPages = 0;
        for (size_t layerIndex = 0; layerIndex < worldTerrainLayers.size();
             ++layerIndex)
        {
            layerVirtualPages += terrainStreaming->layerPageManager
                .TotalPagesForLayer(static_cast<uint32_t>(layerIndex));
        }
        terrainStreaming->layerPageManager.SetResidentCapacity(
            ComputeTerrainResidentCapacity(
                layerVirtualPages,
                TerrainLayerPageManager::DefaultResidentCapacity,
                terrainStreaming->layerPageManager.UsablePhysicalPageCount()));
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
    // L4b 层页 feedback 的 3 个附加输入。World 没有分层地形时先用 page table
    // 占位（shader 由 u_layerFeedbackEnabled=0 保证不采样），有层地形时在
    // 下方被真实的 catalog / ID / 权重纹理覆盖。
    loadContext.resourceCache.BindWorldTexture(
        "terrainLayerCatalog",
        terrainStreaming->pageResources.pageTable);
    loadContext.resourceCache.BindWorldTexture(
        "terrainLayerIds",
        terrainStreaming->pageResources.pageTable);
    loadContext.resourceCache.BindWorldTexture(
        "terrainLayerWeights",
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
    const TerrainChunkResidencyDesc initialChunkResidency =
        ReadInitialTerrainChunkResidency(worldBuildPlan);
    RendererTerrainLoader terrainLoader(
        *pipelineFactory,
        *rendererBackend,
        loadContext,
        *terrainStreaming,
        initialChunkResidency);
    RendererWorldResourceLoadResult loadResult;
    std::vector<LoadedSceneMesh> loadedMeshes;

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
            LoadedSceneMesh loaded;
            loaded.objectName = objectPlan.objectName;
            loaded.request = &*objectPlan.meshLoadRequest;
            // 蓝图组件把放置级绑定写在生成对象上；通用记录优先于描述符自带的
            // 绑定来源（例如 SM 的 simulation.skinning.source）。旧 __tressfx*
            // 字段由 ReadPlacementBindingFromComponent 兼容读取。
            loaded.placementBinding = VL::ReadPlacementBindingFromComponent(obj);
            loaded.result = meshLoader.LoadMeshObject(obj, *objectPlan.meshLoadRequest);
            loadedMeshes.push_back(std::move(loaded));
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
        else if (type == "particleSystem")
        {
            // 粒子资产已由 SceneAssetValidator 冻结；GPU 池和管线由
            // ParticleRenderer 按 RenderScene 管理，这里不创建材质/网格资源。
            continue;
        }
        else if (type == "decal")
        {
            // Decal 的参数与 unit cube 几何由 DecalRenderer 直接消费；
            // 这条资源准备路径不创建材质/网格。
            continue;
        }
        else if (type == "directionalLight" ||
                 type == "pointLight" ||
                 type == "spotLight" ||
                 type == "camera" ||
                 type == "wind")
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

    if (hasTerrainLayers && !terrainStreaming->layerSources.empty())
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
        // L4b：层页 feedback 的 worldTexture 必须指向真实的 catalog/ID/权重，
        // 并且要在 pass descriptor 建立之前绑定（本函数返回后才建 descriptor）。
        loadContext.resourceCache.BindWorldTexture(
            "terrainLayerCatalog",
            terrainStreaming->layerResources.layerTable);
        loadContext.resourceCache.BindWorldTexture(
            "terrainLayerIds",
            terrainStreaming->pageResources.layerIds);
        loadContext.resourceCache.BindWorldTexture(
            "terrainLayerWeights",
            terrainStreaming->pageResources.layerWeights);
        RequestInitialTerrainLayerPages(
            *terrainStreaming,
            0,
            rendererBackend->GetFrameFenceCount());
        terrainStreaming->NotifyContentChanged();
        ApplyTerrainLayerStreamingUploads(*rendererBackend, *terrainStreaming);
    }
    // 所有 mesh renderable 都已创建，现在才能跨对象解析根点骨骼绑定。
    // 失败直接抛出，由 World 事务回滚；不留下部分绑定或静默回退 rest pin。
    // GPU 发丝的 renderable key 已含 placement 身份（renderableObjectKey.h），
    // 因此同一 SM 的多个 placement 各自持有可变模拟状态，不再在加载期拒绝。
    ResolvePlacementBindings(loadedMeshes, loadContext.resourceCache,
        worldBuildPlan.scenePath);

    for (LoadedSceneMesh& loaded : loadedMeshes)
    {
        loadResult.meshObjectPlans.insert(
            loadResult.meshObjectPlans.end(),
            std::make_move_iterator(loaded.result.objectPlans.begin()),
            std::make_move_iterator(loaded.result.objectPlans.end()));
        if (loaded.result.windResponseProfile.has_value())
        {
            loadResult.windResponseProfiles.push_back(
                std::move(*loaded.result.windResponseProfile));
        }
    }

    return loadResult;
}

} // namespace VL
