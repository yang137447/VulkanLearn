#include "engine/blueprintAssetLoadSession.h"

#include <chrono>
#include <atomic>
#include <future>
#include <map>
#include <set>
#include <stdexcept>
#include <thread>
#include "commonFunction.h"
#include "engine/editorThumbnailPreparation.h"
#include "engine/assetPreparationSourceSnapshot.h"
#include "mesh/loader/common/meshAssetLoader.h"
#include "pipeline/pipelineFactory.h"
#include "render/backend/rendererBackendVulkan.h"
#include "render/resource/rendererMeshLoader.h"
#include "render/resource/rendererResourceCache.h"
#include "render/resource/rendererResourceLoadContext.h"
#include "render/resource/resourceRetireQueue.h"
#include "renderSystem.h"
#include "renderGraph.h"
#include "scene/sceneAssetTypes.h"
#include "script/scriptBlueprintAsset.h"
#include "script/scriptPropertySchema.h"
#include "shaderCompiler.h"
#include "texture.h"
#include "world/scriptTemplateExtension.h"

namespace VL
{
namespace
{
struct PreparedBlueprintMesh
{
    nlohmann::json node;
    MeshAssetLoadRequest request;
    EditorThumbnailPreparationResult asset;
};
struct PreparedBlueprintAsset
{
    nlohmann::json declaration;
    std::string digest;
    std::vector<PreparedBlueprintMesh> meshes;
};
struct BlueprintCpuRequest
{
    std::filesystem::path resourceRoot;
    std::string scenePath;
    std::string path;
    nlohmann::json catalog;
    EditorThumbnailPreparationRequest preparation;
};

PreparedBlueprintAsset PrepareBlueprintAsset(BlueprintCpuRequest request)
{
    PreparedBlueprintAsset result;
    auto asset = ReadScriptBlueprintAsset(request.resourceRoot, request.path, request.scenePath, &result.digest);
    auto& declaration = result.declaration;
    declaration = asset;
    declaration["type"] = "blueprintInstance";
    declaration["id"] = "~dynamic." + ContentHasher::HashString(request.path).ToHex();
    declaration["position"] = {0, 0, 0};
    declaration["rotation"] = asset.value("rotation", nlohmann::json::array({0, 0, 0}));
    declaration["scale"] = asset.value("scale", nlohmann::json::array({1, 1, 1}));
    declaration["enabled"] = true;
    declaration["__templatePath"] = request.path;
    declaration["__instanceEditable"] = asset.value("instanceEditable", nlohmann::json::array());
    declaration["__componentInstanceEditable"] = asset.value("componentInstanceEditable", nlohmann::json::object());
    declaration.erase("componentInstanceEditable");
    ValidateScriptPropertySchema(declaration, request.catalog);
    for (const auto& type : request.catalog)
        if (ScriptTypeNameMatches(type, declaration.at("class").get<std::string>()))
            for (const auto& property : type.at("properties"))
                if (!declaration.at("properties").contains(property.at("name").get<std::string>()))
                    declaration["properties"][property.at("name").get<std::string>()] = property.at("value");
    ValidateScriptBlueprint(declaration);
    ValidateScriptPhysicsReferences(nlohmann::json::array(), {{request.path, declaration}});
    const auto components = declaration.value("components", nlohmann::json::array());
    for (size_t index = 0; index < components.size(); ++index)
    {
        // tressfx 组件在场景加载侧就是 mesh 对象（type 收敛 + 放置级绑定字段）。
        // 预准备必须与正式加载同集合，否则带毛发的蓝图资产只准备身体、不准备毛发；
        // 绑定字段由 ScriptBlueprintMeshObject 生成，父 mesh 组件按 id 顺序先被准备。
        if (!IsScriptMeshComponent(components[index].at("type").get<std::string>())) continue;
        PreparedBlueprintMesh mesh;
        mesh.node = ScriptBlueprintMeshObject(declaration, index);
        const auto descriptor = CommonFunction::Path(mesh.node.at("modelPath").get<std::string>());
        const auto before = ContentHasher::HashFile(descriptor);
        mesh.request = MeshAssetLoader::Prepare(mesh.node.at("modelPath").get<std::string>());
        // 同一路径可能已经有活动 GPU 网格；动态导入按源内容区分身份，不能拿旧顶点配新 CPU 碰撞三角。
        const auto modelDigest = ContentHasher::HashFile(mesh.request.importRequest.absoluteModelPath);
        mesh.request.buildPlan.modelCacheKey += "|async=" +
            ContentHasher::HashString(before.ToHex() + modelDigest.ToHex()).ToHex();
        mesh.request.importRequest.buildPlan = mesh.request.buildPlan;
        request.preparation.meshRequest = mesh.request;
        request.preparation.materialOverride = mesh.node.value("materialOverride", "");
        mesh.asset = PrepareEditorThumbnailAsset(request.preparation);
        if (ContentHasher::HashFile(descriptor) != before ||
            ContentHasher::HashFile(mesh.request.importRequest.absoluteModelPath) != modelDigest)
            throw std::runtime_error("Blueprint mesh descriptor changed during preparation");
        for (const auto& shader : mesh.asset.shaders) request.preparation.preparedShaderKeys.insert(shader.cacheKey);
        result.meshes.push_back(std::move(mesh));
    }
    if (ContentHasher::HashFile(ResolveScriptAssetPath(request.resourceRoot, request.path)).ToHex() != result.digest)
        throw std::runtime_error("Blueprint asset changed during preparation");
    return result;
}

void RunBlueprintWorker(std::packaged_task<PreparedBlueprintAsset(BlueprintCpuRequest)> task,
    BlueprintCpuRequest request, std::shared_ptr<std::atomic<bool>> finished)
{
    task(std::move(request));
    finished->store(true, std::memory_order_release);
}

// 注册/回收只由 GT 调用；worker 仅发布完成标记，不能访问 registry 容器。
class BlueprintAssetCpuWorkers
{
public:
    ~BlueprintAssetCpuWorkers() { Wait(); }
    void Start(std::packaged_task<PreparedBlueprintAsset(BlueprintCpuRequest)> task, BlueprintCpuRequest request)
    {
        for (auto it = jobs.begin(); it != jobs.end();)
            if (it->finished && it->finished->load(std::memory_order_acquire))
            {
                if (it->thread.joinable()) it->thread.join();
                it = jobs.erase(it);
            }
            else ++it;
        jobs.emplace_back();
        auto& job = jobs.back();
        job.finished = std::make_shared<std::atomic<bool>>(false);
        job.thread = std::thread(RunBlueprintWorker, std::move(task), std::move(request), job.finished);
    }
    void Wait()
    {
        for (auto& job : jobs) if (job.thread.joinable()) job.thread.join();
        jobs.clear();
    }
private:
    struct Job
    {
        std::thread thread;
        std::shared_ptr<std::atomic<bool>> finished;
    };
    std::vector<Job> jobs;
};

BlueprintAssetCpuWorkers& BlueprintWorkers()
{
    static BlueprintAssetCpuWorkers workers;
    return workers;
}

WorldMeshObject ToWorldMesh(const MeshObjectBuildPlan& plan)
{
    WorldMeshObject mesh;
    // 异步蓝图模板也要携带动画播放器与当前 pose：spawn 克隆出来的骨骼网格
    // 若首帧没有 pose，逐对象 pre-deform 资源会在首次初始化时被建缺，之后
    // 无法补齐（对象资源包 Initialize 是一次性的）。
    mesh.animation = plan.animation;
    mesh.skinPose = plan.skinPose;
    mesh.skinSection = plan.skinSection;
    mesh.debugName = plan.debugName;
    mesh.sceneObjectIdentity = plan.sceneObjectIdentity;
    mesh.materialSlotIndex = plan.materialSlotIndex;
    mesh.materialSlotName = plan.materialSlotName;
    mesh.model = plan.model;
    mesh.scenePosition = plan.scenePosition;
    mesh.sceneRotation = plan.sceneRotation;
    mesh.sceneScale = plan.sceneScale;
    mesh.localBoundsMin = plan.localBoundsMin;
    mesh.localBoundsMax = plan.localBoundsMax;
    mesh.collisionTriangles = plan.collisionTriangles;
    mesh.meshKey = plan.meshKey;
    mesh.materialKey = plan.materialKey;
    mesh.materialInstanceKey = plan.materialInstanceKey;
    mesh.speedTreeWindProfileKey = plan.speedTreeWindProfileKey;
    return mesh;
}

}

struct BlueprintAssetLoadSession::Impl
{
    RendererBackendVulkan& backend;
    PipelineFactory& factory;
    RenderGraph& graph;
    uint64_t generation = 0;
    uint64_t requestId = 0;
    std::string path;
    std::string error;
    bool cancelled = false;
    bool done = false;
    bool committed = false;
    std::future<PreparedBlueprintAsset> worker;
    std::optional<PreparedBlueprintAsset> prepared;
    nlohmann::json declaration;
    RendererResourceCache::ImmutableWorldLocalResourceRefs baseline;
    std::unique_ptr<RendererResourceCache> candidate;
    PipelineFactory::GraphicsCandidateState graphics;
    size_t meshIndex = 0, textureIndex = 0;
    std::vector<WorldMeshObject> meshes;
    std::vector<SpeedTreeWindProfile> windProfiles;

    Impl(RendererBackendVulkan& backend, PipelineFactory& factory, RenderGraph& graph)
        : backend(backend), factory(factory), graph(graph) {}
    void Discard()
    {
        candidate.reset();
        baseline.reset();
        prepared.reset();
        graphics = {};
        meshes.clear();
        windProfiles.clear();
    }
    void Validate() const
    {
        if (ContentHasher::HashFile(ResolveScriptAssetPath(CommonFunction::GetResourcePath(), path)).ToHex() != prepared->digest)
            throw std::runtime_error("Blueprint asset changed before publication");
        for (const auto& mesh : prepared->meshes)
            ValidatePreparedAssetSources(mesh.asset.materialDigests);
        for (const auto& shader : graphics.shaderBuildArtifacts)
        {
            const auto current = factory.GetShaderCompiler().ValidateCandidateSourcesStillCurrent(shader.request, shader.artifact);
            if (!current.current) throw std::runtime_error("Blueprint shader changed: " + current.reason);
        }
    }
    void Commit(World& world)
    {
        auto& cache = RendererResourceCache::GetInstance();
        auto extension = world.PrepareScriptTemplateExtension(path, prepared->digest, declaration, meshes, windProfiles);
        candidate->TrackDynamicAdditions(baseline);
        *candidate = cache.RebaseWorldExtensionCandidate(std::move(*candidate), baseline);
        auto pipelineCommit = factory.PrepareCandidateCommit(graphics);
        auto& retire = ResourceRetireQueue::GetInstance();
        auto retirements = retire.PrepareRetirements({{"BlueprintAssetPreviousPackage", generation,
            retire.GetLastSubmittedEpoch(), {}}});
        std::vector<ShaderBuildArtifact> artifacts;
        artifacts.reserve(graphics.shaderBuildArtifacts.size());
        for (const auto& shader : graphics.shaderBuildArtifacts) artifacts.push_back(shader.artifact);
        Validate();
        // manifest 最后发布成功后，活动 CPU owner 只做无抛交换；World/Play 不经历重建。
        factory.GetShaderCompiler().CommitArtifacts(artifacts);
        factory.CommitPreparedCandidate(std::move(pipelineCommit));
        retirements.GetAdditionalResource(0).resource = cache.CommitCandidate(std::move(*candidate));
        world.CommitScriptTemplateExtension(std::move(extension));
        retire.CommitPreparedRetirements(std::move(retirements));
        committed = true;
        done = true;
        Discard();
    }
};

BlueprintAssetLoadSession::BlueprintAssetLoadSession(RendererBackendVulkan& backend, PipelineFactory& factory, RenderGraph& graph)
    : impl(std::make_unique<Impl>(backend, factory, graph)) {}
BlueprintAssetLoadSession::~BlueprintAssetLoadSession() = default;

void BlueprintAssetLoadSession::Begin(const World& world, uint64_t requestId, const std::string& path,
    const nlohmann::json& classCatalog)
{
    if (impl->worker.valid() || impl->generation) throw std::logic_error("Blueprint asset session already started");
    impl->generation = world.GetGeneration();
    impl->requestId = requestId;
    impl->path = path;
    if (world.GetScriptTemplates().contains(path))
    {
        impl->declaration = world.GetScriptTemplates().at(path);
        impl->done = true;
        return;
    }
    BlueprintCpuRequest request;
    request.resourceRoot = CommonFunction::GetResourcePath();
    request.scenePath = world.GetScenePath();
    request.path = path;
    request.catalog = classCatalog;
    request.preparation.shaderRoot = impl->factory.GetShaderCompiler().GetShaderRoot();
    request.preparation.supportsDualSourceBlend = impl->backend.SupportsDualSourceBlend();
    for (const auto& pair : impl->graph.GetRenderpasses())
    {
        const auto& pass = pair.second;
        auto& contracts = request.preparation.surfaceContracts;
        if (pass.type == RenderGraphPassType::Geometry)
        {
            contracts.emplace(RenderMode::Opaque, pass.pipelineContractKey);
            contracts.emplace(RenderMode::OpaqueClip, pass.pipelineContractKey);
        }
        else if (pass.type == RenderGraphPassType::ForwardOpaque)
            contracts.emplace(RenderMode::ForwardOpaque, pass.pipelineContractKey);
        else if (pass.type == RenderGraphPassType::ForwardTransparent)
            for (auto mode : {RenderMode::TransparentAlphaBlend, RenderMode::TransparentAlphaBlendWriteDepth,
                RenderMode::TransparentAdditive, RenderMode::ThinTranslucent})
                contracts.emplace(mode, pass.pipelineContractKey);
    }
    // packaged_task 的 future 析构不等待；registry 在 Engine shutdown 前统一 join CPU worker。
    std::packaged_task<PreparedBlueprintAsset(BlueprintCpuRequest)> task(PrepareBlueprintAsset);
    impl->worker = task.get_future();
    BlueprintWorkers().Start(std::move(task), std::move(request));
}

bool BlueprintAssetLoadSession::Step(World& world)
{
    auto& state = *impl;
    if (state.done) return true;
    if (world.GetGeneration() != state.generation) { Cancel(); return true; }
    try
    {
        if (!state.prepared)
        {
            if (!state.worker.valid()) throw std::logic_error("Blueprint asset session was not started");
            if (state.worker.wait_for(std::chrono::seconds(0)) != std::future_status::ready) return false;
            state.prepared = state.worker.get();
            if (state.cancelled) throw std::runtime_error("Blueprint asset request cancelled");
            state.declaration = state.prepared->declaration;
            auto& active = RendererResourceCache::GetInstance();
            state.baseline = active.CaptureActiveWorldLocalResources();
            if (state.baseline->ownerGeneration != state.generation)
                throw std::runtime_error("Blueprint renderer generation is stale");
            state.candidate = std::make_unique<RendererResourceCache>(active.BeginWorldExtensionCandidate(state.generation));
            for (auto& mesh : state.prepared->meshes)
                for (auto& shader : mesh.asset.shaders)
                {
                    state.graphics.shaderVariants.emplace(shader.cacheKey, shader.graphicsArtifact);
                    state.graphics.publications.push_back({shader.cacheKey, shader.graphicsArtifact});
                    state.graphics.shaderBuildArtifacts.push_back({std::move(shader.request), std::move(shader.artifact)});
                }
            state.Validate();
            return false;
        }
        if (state.cancelled) throw std::runtime_error("Blueprint asset request cancelled");
        if (state.meshIndex < state.prepared->meshes.size())
        {
            auto& mesh = state.prepared->meshes[state.meshIndex];
            if (state.textureIndex < mesh.asset.textures.size())
            {
                auto& texture = mesh.asset.textures[state.textureIndex++];
                if (!state.candidate->GetTexture(texture.cacheKey))
                    state.candidate->BindTexture(texture.cacheKey, std::make_shared<Texture>(
                        state.backend, texture.image, ToTextureCreateDesc(texture.description)));
                texture.image = {};
                return false;
            }
            state.Validate();
            RendererResourceLoadContext context{*state.candidate, state.graph, &state.graphics};
            context.preparedMaterialSources = &mesh.asset.materialSources;
            // 异步 Spawn 的蒙皮材质必须与主渲染选择同一条蒙皮路径，否则同一
            // 场景会同时存在两种 vertex variant / 对象资源包。
            context.prerenderSkin = RenderSystem::GetInstance().IsPrerenderSkinEnabled();
            auto loaded = RendererMeshLoader(state.factory, state.backend, context).LoadMeshObject(
                mesh.node, mesh.request, mesh.asset.mesh);
            if (!context.pendingGeneratedFiles.empty())
                throw std::runtime_error("Async Blueprint asset generated unexpected resource files");
            for (const auto& plan : loaded.objectPlans) state.meshes.push_back(ToWorldMesh(plan));
            if (loaded.speedTreeWindProfile) state.windProfiles.push_back(*loaded.speedTreeWindProfile);
            ++state.meshIndex;
            state.textureIndex = 0;
            return false;
        }
        state.Commit(world);
    }
    catch (const std::exception& exception)
    {
        state.error = exception.what();
        state.done = true;
        state.Discard();
    }
    return state.done;
}

void BlueprintAssetLoadSession::Cancel()
{
    impl->cancelled = true;
    impl->error = "Blueprint asset request cancelled";
    impl->done = true;
    impl->Discard();
}
void BlueprintAssetLoadSession::WaitForWorkers() { BlueprintWorkers().Wait(); }
bool BlueprintAssetLoadSession::IsDone() const { return impl->done; }
const std::string& BlueprintAssetLoadSession::Error() const { return impl->error; }
const std::string& BlueprintAssetLoadSession::Path() const { return impl->path; }
uint64_t BlueprintAssetLoadSession::RequestId() const { return impl->requestId; }
const nlohmann::json& BlueprintAssetLoadSession::Template() const { return impl->declaration; }

void BlueprintAssetLoadSession::Release(World& world)
{
    auto& state = *impl;
    if (!state.committed) return;
    if (world.GetGeneration() != state.generation) { state.committed = false; return; }
    auto extension = world.PrepareScriptTemplateRemoval(state.path);
    std::set<std::string> usedMeshes, usedMaterials, usedInstances;
    for (const auto& pair : extension->meshes)
    {
        usedMeshes.insert(pair.second.meshKey);
        usedMaterials.insert(pair.second.materialKey);
        usedInstances.insert(pair.second.materialInstanceKey);
    }
    auto& active = RendererResourceCache::GetInstance();
    auto candidate = active.BeginWorldExtensionCandidate(state.generation);
    candidate.PruneDynamicResources(usedMeshes, usedMaterials, usedInstances);
    for (const auto& pair : world.GetMeshObjects())
        if (!extension->meshes.count(pair.first)) candidate.BindObjectResource(pair.first, {});
    auto& retire = ResourceRetireQueue::GetInstance();
    auto retirements = retire.PrepareRetirements({{"BlueprintAssetReleasedPackage", state.generation,
        retire.GetLastSubmittedEpoch(), {}}});
    retirements.GetAdditionalResource(0).resource = active.CommitCandidate(std::move(candidate));
    world.CommitScriptTemplateExtension(std::move(extension));
    retire.CommitPreparedRetirements(std::move(retirements));
    state.committed = false;
}
}
