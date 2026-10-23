#include "render/resource/rendererResourceLoadCoordinator.h"

#include <iterator>
#include <stdexcept>
#include <string>
#include <utility>

#include <nlohmann/json.hpp>

#include "animation/skeletalPlayback.h"
#include "hair/tressfxProxySkinning.h"
#include "mesh/loader/common/meshAssetLoader.h"
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
// simulation.skinning 的跨对象引用；不要在材质/网格加载中途读尚未创建的源对象。
struct LoadedSceneMesh
{
    std::string objectName;
    const MeshAssetLoadRequest* request = nullptr;
    RendererMeshLoadResult result;
};

// Hair 与源角色都必须处于同一对象空间：骨骼矩阵把绑定空间映射到源角色的模型空间，
// 发丝顶点却按自己的 model 绘制。两个场景对象的 model 不同就要在加载期拒绝，
// 不能让错误的根点位置进入 SSBO。
bool ModelMatricesMatch(const Eigen::Matrix4f& left, const Eigen::Matrix4f& right)
{
    return (left - right).cwiseAbs().maxCoeff() <= 1.0e-4f;
}

// `collision.mesh` 代理没有作者权重：绑定姿态下把每个代理顶点匹配到最近的
// 身体顶点，复制其完整影响列表并归一化。源身体顶点先乘 section.meshToModel
// 进入与代理相同的“渲染绑定空间”，否则带缩放的骨架会把转移算错。
struct ProxySkinBodyData
{
    std::vector<Eigen::Vector3f> positions;
    std::vector<uint32_t> offsets;
    std::vector<VL::Hair::SdfProxySkinSourceInfluence> influences;
};

void BuildProxySkinBodyData(
    const Animation::SkeletalAsset& asset, ProxySkinBodyData& out)
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

std::shared_ptr<const VL::Hair::SdfProxySkinTransfer> BuildProxySkinTransfer(
    const LoadedSceneMesh& hair, const LoadedSceneMesh& source)
{
    if (!hair.result.importResult || !source.result.importResult ||
        !source.result.importResult->modelResource.skeletalAsset)
        return nullptr;
    const VL::Hair::SdfTriangleMesh* proxy = nullptr;
    for (const MeshSection& section : hair.result.importResult->modelResource.sections)
    {
        if (section.tressfxCollisionMesh)
        {
            proxy = section.tressfxCollisionMesh.get();
            break;
        }
    }
    if (proxy == nullptr)
        return nullptr;

    ProxySkinBodyData body;
    const Animation::SkeletalAsset& skeletal =
        *source.result.importResult->modelResource.skeletalAsset;
    BuildProxySkinBodyData(skeletal, body);
    const VL::Hair::SdfProxySkinSource view{
        body.positions.data(), body.positions.size(), body.offsets.data(),
        body.influences.data(), static_cast<uint32_t>(skeletal.mesh.joints.size())};
    return std::make_shared<const VL::Hair::SdfProxySkinTransfer>(
        VL::Hair::BuildTressfxProxySkinTransfer(proxy->positions, view,
            TressfxProxySkinMaxDistanceCm));
}

void ResolveHairSkinningBindings(
    const std::vector<LoadedSceneMesh>& loadedMeshes,
    RendererResourceCache& resourceCache,
    const std::string& scenePath)
{
    for (const LoadedSceneMesh& hair : loadedMeshes)
    {
        const MeshAssetBuildPlan& hairPlan = hair.request->buildPlan;
        if (hairPlan.assetType != "tressfx" || !hairPlan.tressfxSimulation.skinning)
            continue;
        const TressfxSimulationSkinning& requested = *hairPlan.tressfxSimulation.skinning;

        const LoadedSceneMesh* source = nullptr;
        for (const LoadedSceneMesh& candidate : loadedMeshes)
        {
            if (candidate.objectName == requested.source)
            {
                source = &candidate;
                break;
            }
        }
        if (source == nullptr)
            throw std::runtime_error(
                "TressFX simulation.skinning source object \"" + requested.source +
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
                "TressFX simulation.skinning source object \"" + requested.source +
                "\" has no skeleton (SK): " + hairPlan.meshAssetPath);

        std::vector<std::string> jointNames;
        jointNames.reserve(playback->Asset().skeleton.joints.size());
        for (const Animation::SkeletonJoint& joint : playback->Asset().skeleton.joints)
            jointNames.push_back(joint.name);
        const uint32_t jointIndex = VL::Hair::ResolveTressfxRootJoint(
            jointNames, requested.source, requested.rootBone, hairPlan.meshAssetPath);

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
                hair.objectName + "\" source=\"" + requested.source + "\": " +
                hairPlan.meshAssetPath);

        VL::Hair::TressfxSkinningBinding binding;
        binding.enabled = true;
        binding.sourceSceneObject = requested.source;
        binding.rootBone = requested.rootBone;
        binding.jointIndex = jointIndex;
        // 胶囊骨骼与根骨共用同一个源对象/骨架；每个胶囊独立解析，未知骨在加载期
        // 硬错误（消息带胶囊下标），没有 bone 的槽保留 -1 的静态行为。
        binding.capsuleJointIndices.assign(
            hairPlan.tressfxSimulation.capsules.size(), -1);
        for (size_t capsuleIndex = 0;
             capsuleIndex < hairPlan.tressfxSimulation.capsules.size(); ++capsuleIndex)
        {
            const VL::Hair::TressfxCollider& capsule =
                hairPlan.tressfxSimulation.capsules[capsuleIndex];
            if (capsule.bone.empty())
                continue;
            binding.capsuleJointIndices[capsuleIndex] = static_cast<int32_t>(
                VL::Hair::ResolveTressfxJointByName(jointNames, requested.source,
                    capsule.bone,
                    "collision.capsules[" + std::to_string(capsuleIndex) + "].bone",
                    hairPlan.meshAssetPath));
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
        if (hairPlan.tressfxSimulation.collisionMesh)
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
            loadedMeshes.push_back({
                objectPlan.objectName,
                &*objectPlan.meshLoadRequest,
                meshLoader.LoadMeshObject(obj, *objectPlan.meshLoadRequest)});
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

    // 所有 mesh renderable 都已创建，现在才能跨对象解析根点骨骼绑定。
    // 失败直接抛出，由 World 事务回滚；不留下部分绑定或静默回退 rest pin。
    ResolveHairSkinningBindings(loadedMeshes, loadContext.resourceCache,
        worldBuildPlan.scenePath);

    for (LoadedSceneMesh& loaded : loadedMeshes)
    {
        loadResult.meshObjectPlans.insert(
            loadResult.meshObjectPlans.end(),
            std::make_move_iterator(loaded.result.objectPlans.begin()),
            std::make_move_iterator(loaded.result.objectPlans.end()));
        if (loaded.result.speedTreeWindProfile.has_value())
        {
            loadResult.speedTreeWindProfiles.push_back(
                std::move(*loaded.result.speedTreeWindProfile));
        }
    }

    return loadResult;
}

} // namespace VL
