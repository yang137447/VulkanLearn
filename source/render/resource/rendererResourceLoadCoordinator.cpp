#include "render/resource/rendererResourceLoadCoordinator.h"

#include <iostream>
#include <iterator>
#include <stdexcept>
#include <string>
#include <utility>

#include <nlohmann/json.hpp>

#include "animation/skeletalPlayback.h"
#include "hair/tressfxProxySkinning.h"
#include "hair/tressfxRootWeights.h"
#include "hair/tressfxSkinning.h"
#include "mesh/loader/common/meshAssetLoader.h"
#include "scene/placementBinding.h"
#include "render/resource/rendererEnvironmentLoader.h"
#include "render/resource/rendererMaterialLoader.h"
#include "render/resource/rendererMeshLoader.h"
#include "render/resource/rendererResourceCache.h"
#include "render/resource/rendererResourceLoadContext.h"
#include "renderGraph.h"
#include "renderableObject.h"
#include "world/loading/worldLoader.h"

namespace VL
{
namespace
{
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
    std::vector<LoadedSceneMesh> loadedMeshes;

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
