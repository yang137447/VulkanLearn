#include "engine/editorThumbnailRenderSession.h"

#include <stdexcept>
#include "commonFunction.h"
#include "engine/editorThumbnailGraph.h"
#include "engine/editorThumbnailJob.h"
#include "engine/editorThumbnailPreparation.h"
#include "engine/modelPreviewCamera.h"
#include "engine/modelPreviewRequest.h"
#include "material.h"
#include "materialInstance.h"
#include "mesh/loader/common/meshAssetLoader.h"
#include "sceneNode.h"
#include "pipeline/pipelineFactory.h"
#include "profiler.h"
#include "shaderCompiler.h"
#include "render/backend/rendererBackendVulkan.h"
#include "render/backend/rendererFrameResources.h"
#include "render/backend/rendererObjectResourceRegistry.h"
#include "render/environment/environmentIblBaker.h"
#include "render/frontend/rendererFrontend.h"
#include "render/pass/passRuntime.h"
#include "render/resource/rendererEnvironmentLoader.h"
#include "render/resource/rendererMaterialLoader.h"
#include "render/resource/rendererMeshLoader.h"
#include "render/resource/rendererResourceCache.h"
#include "render/resource/rendererResourceLoadContext.h"
#include "renderGraph.h"
#include "world/loading/worldLoader.h"
#include "world/loading/worldBuilder.h"
#include "world/worldSnapshotBuilder.h"
#include "render/camera/cameraExposure.h"

namespace VL
{
namespace
{
template<class T> T Require(RuntimeResult<T> result)
{
    if (result.IsFailure()) throw std::runtime_error(FormatRuntimeError(result.Error()));
    return std::move(result.Value());
}

}

struct EditorThumbnailRenderSession::Impl : PassRuntimeServices
{
    RendererBackendVulkan& backend;
    PipelineFactory& factory;
    RendererResourceCache cache = RendererResourceCache::GetInstance().BeginCandidate(0);
    std::unique_ptr<RendererResourceCache> baseCache;
    RenderGraph graph;
    PipelineFactory::GraphicsCandidateState graphics;
    PipelineFactory::GraphicsCandidateState baseGraphics;
    RendererFrameResources frames;
    EnvironmentIblBaker ibl;
    WorldBuildPlan plan;
    RendererMeshLoadResult materialPreviewMesh;
    EditorThumbnailJob<EditorThumbnailPreparationResult> preparation;
    std::optional<EditorThumbnailPreparationResult> preparedAsset;
    size_t textureIndex = 0;
    std::unordered_map<std::string, GraphicsShaderVariantArtifact> thumbnailShaders;
    RenderScene scene;
    ResolvedRenderScene resolved;
    std::shared_ptr<const RendererGpuImageToken> image;
    std::string error;
    nlohmann::json graphJson;
    size_t objectIndex = 0;
    uint32_t mip = 0;
    int stage = 0;
    bool descriptorsReady = false;

    Impl(RendererBackendVulkan& backend, PipelineFactory& factory) : backend(backend), factory(factory) {}
    ~Impl()
    {
        // 退出先收口唯一 CPU worker，再销毁会话 GPU 资源。
        preparation.Wait();
        // 每个提交都已等本任务的 fence；这些私有资源从未进入主帧，无需全局 waitIdle。
        resolved = {};
        cache.Clear();
        baseCache.reset();
        graph.Shutdown(backend);
        ibl.Shutdown(backend);
        frames.Shutdown(backend);
    }
    RendererResourceLoadContext Context() { return {cache, graph, &graphics}; }
    void ReleaseAsset()
    {
        if (!baseCache) return;
        resolved = {};
        scene = {};
        plan = {};
        preparedAsset.reset();
        cache = baseCache->BeginActiveWorldCandidate(0);
        graphics = baseGraphics;
    }
    bool LoadPreviewMesh(
        const nlohmann::json& node, const MeshAssetLoadRequest& request,
        RendererResourceLoadContext& context, RendererMeshLoadResult& loaded)
    {
        const bool materialPreview = node.at("name") == "preview_material";
        if (!preparedAsset)
        {
            auto prepared = preparation.Poll();
            if (prepared)
            {
                for (auto& shader : prepared->shaders)
                {
                    const auto current = factory.GetShaderCompiler().ValidateCandidateSourcesStillCurrent(
                        shader.request, shader.artifact);
                    if (!current.current) throw std::runtime_error("Thumbnail shader changed during preparation: " + current.reason);
                    graphics.shaderVariants.insert_or_assign(shader.cacheKey, shader.graphicsArtifact);
                    // 会话只保留有界 CPU shader 产物，不保留源码快照、MI、贴图或对象 descriptor。
                    if (thumbnailShaders.size() < 64)
                        thumbnailShaders.insert_or_assign(shader.cacheKey, std::move(shader.graphicsArtifact));
                }
                prepared->shaders.clear();
                preparedAsset = std::move(prepared);
                textureIndex = 0;
                return false;
            }
            if (!preparation.HasTask())
            {
                EditorThumbnailPreparationRequest input;
                input.meshRequest = request;
                input.importMesh = !materialPreview || materialPreviewMesh.objectPlans.empty();
                input.materialOverride = node.value("materialOverride", std::string{});
                input.shaderRoot = factory.GetShaderCompiler().GetShaderRoot();
                input.supportsDualSourceBlend = backend.SupportsDualSourceBlend();
                for (const auto& pair : graph.GetRenderpasses())
                {
                    const auto& pass = pair.second;
                    if (pass.type == RenderGraphPassType::Geometry)
                    {
                        input.surfaceContracts.emplace(RenderMode::Opaque, pass.pipelineContractKey);
                        input.surfaceContracts.emplace(RenderMode::OpaqueClip, pass.pipelineContractKey);
                    }
                    else if (pass.type == RenderGraphPassType::ForwardOpaque)
                        input.surfaceContracts.emplace(RenderMode::ForwardOpaque, pass.pipelineContractKey);
                    else if (pass.type == RenderGraphPassType::ForwardTransparent)
                    {
                        input.surfaceContracts.emplace(RenderMode::TransparentAlphaBlend, pass.pipelineContractKey);
                        input.surfaceContracts.emplace(RenderMode::TransparentAlphaBlendWriteDepth, pass.pipelineContractKey);
                        input.surfaceContracts.emplace(RenderMode::TransparentAdditive, pass.pipelineContractKey);
                        input.surfaceContracts.emplace(RenderMode::ThinTranslucent, pass.pipelineContractKey);
                    }
                }
                for (const auto& pair : graphics.shaderVariants) input.preparedShaderKeys.insert(pair.first);
                preparation.Start(PrepareEditorThumbnailAsset, std::move(input));
            }
            return false;
        }
        if (textureIndex < preparedAsset->textures.size())
        {
            auto& texture = preparedAsset->textures[textureIndex++];
            if (!cache.GetTexture(texture.cacheKey))
                cache.BindTexture(texture.cacheKey, std::make_shared<Texture>(
                    backend, texture.image, ToTextureCreateDesc(texture.description)));
            // 每帧至多上传一张；上传后立即释放 CPU 像素，不跨 MI 累积贴图。
            texture.image = {};
            return false;
        }
        // 分帧上传期间作者文件仍可能改变；最终材质绑定前再确认 CPU 准备所依据的版本。
        for (const auto& pair : preparedAsset->materialDigests)
            if (ContentHasher::HashFile(pair.first) != pair.second)
                throw std::runtime_error("Thumbnail material changed during preparation: " + pair.first.string());
        if (!materialPreview || materialPreviewMesh.objectPlans.empty())
        {
            loaded = RendererMeshLoader(factory, backend, context).LoadMeshObject(
                node, request, std::move(preparedAsset->mesh));
            if (materialPreview)
            {
                // 同一会话的材质缩略图只换 MI；固定球的导入、切线、包围盒与 GPU 几何只准备一次。
                // 基底只保留不可变 renderable，不保留本次 MI、贴图或对象 descriptor。
                for (const auto& object : loaded.objectPlans)
                    baseCache->BindRenderableObject(object.meshKey, *cache.GetRenderableObject(object.meshKey));
                materialPreviewMesh = loaded;
            }
            preparedAsset.reset();
            return true;
        }

        loaded = materialPreviewMesh;
        // 每次请求仍在自己的候选中加载材质；只替换计划的绑定，不改缓存球或上一次 descriptor。
        const auto material = RendererMaterialLoader(factory, backend, context).LoadSceneMaterialInstance(
            node.at("materialOverride").get<std::string>());
        for (auto& object : loaded.objectPlans)
        {
            object.materialKey = material->GetBaseMaterial().lock()->GetMaterialKey();
            object.materialInstanceKey = material->GetName();
        }
        preparedAsset.reset();
        return true;
    }
    void UpdateGlobalUBOForPass(vk::CommandBuffer& command) override
    {
        UBOGlobal ubo;
        ubo.view = scene.camera.view;
        ubo.projection = scene.camera.projection;
        ubo.invView = ubo.view.inverse();
        ubo.invProjection = ubo.projection.inverse();
        ubo.viewProjection = ubo.projection * ubo.view;
        ubo.invViewProjection = ubo.viewProjection.inverse();
        ubo.previousViewProjection = ubo.viewProjection;
        ubo.cameraPosition = scene.camera.position;
        ubo.cascadeSplits.setZero();
        ubo.csmParameters.setZero();
        ubo.shadowBias.fill(Eigen::Vector4f::Zero());
        for (auto& matrix : ubo.lightViewProj) matrix.setIdentity();
        ubo.debugViewMode = 0;
        ubo.environmentIntensity = scene.environment.intensity;
        ubo.environmentType = 0;
        ubo.skyParameters = scene.environment.skyParameters;
        frames.UpdateGlobalUniformBuffer(command, 0, ubo);
    }
    void UpdateShadowGlobalUBOForPass(vk::CommandBuffer& command, uint32_t, uint32_t, uint32_t) override
    { UpdateGlobalUBOForPass(command); }
    void UpdateMaterialInstanceUBOForPass(const std::shared_ptr<MaterialInstance>& material) override
    { frames.UpdateMaterialInstanceUniformBuffer(0, *material); }
    void UpdateObjectUBOForPass(RendererObjectGpuResources& resources, const RenderDrawPacket& draw) override
    { frames.UpdateObjectUniformBuffer(0, resources, draw, nullptr); }
    void UploadLightsForPass(uint32_t, const std::vector<LightSnapshot>& lights) override
    { frames.UpdateLightBuffer(0, lights); }
    bool IsCsmEnabled() const override { return false; }
    bool IsShadowCascadeActive(uint32_t) const override { return false; }
    void DrawEditorGridGeometry(vk::CommandBuffer&) override {}

    void ConfigureScene()
    {
        auto world = Require(WorldBuilder().BuildFromLoadedScene(1, plan, cache, 4));
        ModelPreviewCamera camera;
        if (!camera.Synchronize(*world, 1.0f)) throw std::runtime_error("Thumbnail model has no bounds");
        auto& projection = *world->GetCamera();
        projection.SetProjection(projection.GetHFOV(), 1.0f, projection.GetClipNear(), projection.GetClipFar());
        WorldSnapshotBuilder snapshots;
        scene = Require(RendererFrontend().BuildRenderScene(Require(snapshots.Build(*world, {}))));
        resolved = Require(ResolvedRenderSceneBuilder().Build(scene, cache));
        frames.EnsureLightCapacity(scene.lights.size(), backend);
        RendererDescriptorContext descriptors;
        descriptors.globalUniformBufferInfos = &frames.GetGlobalUniformBufferInfos();
        descriptors.lightBufferInfos = &frames.GetLightBufferInfos();
        descriptors.resourceCache = &cache;
        descriptors.commonOpaqueShadowMaterial = graph.FindCanonicalShadowPass()->materialInstance.lock()->GetBaseMaterial().lock();
        if (descriptorsReady) graph.RefreshRuntimeDescriptors(backend, descriptors);
        else { graph.RenderInitialize(backend, descriptors); descriptorsReady = true; }
        RendererObjectResourceRegistry().InitializeResolvedSceneResources(backend, descriptors, scene, resolved, cache);
        // 统一固定曝光，与中央预览的相机契约一致；不读取活动图上用户正编辑的参数。
        const float exposure = ExposureScale(scene.camera.exposure);
        for (auto& pair : graph.GetRenderpasses())
        {
            auto material = pair.second.materialInstance.lock();
            if (!material) continue;
            if (pair.first == "toneMapping")
            {
                auto value = material->GetParameter<Eigen::Vector4f>("u_toneMappingParams");
                value[0] = exposure;
                material->SetParameter("u_toneMappingParams", value);
                material->SetParameter("u_exposureSettings", Eigen::Vector4f::Zero().eval());
            }
            if (pair.first == "bloomPrefilter")
            {
                auto value = material->GetParameter<Eigen::Vector4f>("u_bloomPrefilterParams");
                value[3] = exposure;
                material->SetParameter("u_bloomPrefilterParams", value);
                material->SetParameter("u_exposureSettings", Eigen::Vector4f::Zero().eval());
            }
        }
    }

    void Render()
    {
        vk::CommandBuffer command;
        try
        {
            command = backend.BeginSingleTimeCommands();
            size_t index = 0;
            for (const auto& name : graph.GetRenderpassesOrdered())
            {
                PassRuntimeContext context{command, graph.GetRenderpasses().at(name), graph, index++, 0, scene, resolved, *this};
                PassRuntime().RecordPass(name, context);
            }
            const auto& output = graph.GetResourcesResolve().at("thumbnailOutput").at(0);
            vk::ImageMemoryBarrier barrier;
            barrier.setImage(output.image).setOldLayout(vk::ImageLayout::eColorAttachmentOptimal)
                .setNewLayout(vk::ImageLayout::eTransferSrcOptimal)
                .setSrcAccessMask(vk::AccessFlagBits::eColorAttachmentWrite)
                .setDstAccessMask(vk::AccessFlagBits::eTransferRead)
                .setSrcQueueFamilyIndex(VK_QUEUE_FAMILY_IGNORED).setDstQueueFamilyIndex(VK_QUEUE_FAMILY_IGNORED)
                .setSubresourceRange({vk::ImageAspectFlagBits::eColor, 0, 1, 0, 1});
            command.pipelineBarrier(vk::PipelineStageFlagBits::eColorAttachmentOutput, vk::PipelineStageFlagBits::eTransfer,
                {}, {}, {}, barrier);
            // 成图必须复制到独立槽位，下一资产会覆盖 graph 输出；CPU/UI 只接收逻辑 token。
            auto completedImage = backend.RecordThumbnailImage(command, output.image, 256, 256);
            backend.EndSingleTimeCommands(command);
            image = std::move(completedImage);
        }
        catch (...)
        {
            if (command) backend.AbortSingleTimeCommands(command);
            throw;
        }
    }

    bool Step()
    {
        PROFILE_SCOPE("EditorThumbnail:Step");
        auto context = Context();
        switch (stage)
        {
        case 0:
            graph.LoadRenderGraph(graphJson, backend);
            frames.Initialize(backend);
            ++stage;
            break;
        case 1:
            RendererMaterialLoader(factory, backend, context).LoadPassMaterials();
            ++stage;
            break;
        case 2:
            if (objectIndex < plan.sceneAssetPlan.objectPlans.size())
            {
                const auto& object = plan.sceneAssetPlan.objectPlans[objectIndex++];
                const auto& json = plan.sceneJson.at("objects").at(object.objectIndex);
                if (object.objectType == "environment")
                    RendererEnvironmentLoader(factory, backend, context).LoadEnvironmentObject(json);
            }
            else ++stage;
            break;
        case 3:
            ibl.Initialize(factory, backend, frames.GetGlobalUniformBufferInfos());
            cache.BindWorldTexture("prefilteredEnvironmentCube", ibl.GetPrefilteredEnvironmentCube());
            ++stage;
            break;
        case 4:
        {
            const auto cube = cache.GetWorldTexture("environmentCube");
            if (!cube) throw std::runtime_error("Thumbnail environment must contain HDRI");
            auto command = backend.BeginSingleTimeCommands();
            try
            {
                if (mip == 0) ibl.RecordSphericalHarmonics(command, *cube, 0);
                ibl.RecordPrefilterMip(command, *cube, 0, mip++);
                if (mip == ibl.GetPrefilterMipCount()) { ibl.RecordCommit(command); ++stage; objectIndex = 0; }
                backend.EndSingleTimeCommands(command);
            }
            catch (...) { if (command) backend.AbortSingleTimeCommands(command); throw; }
            if (stage == 5)
            {
                // 先固定环境与 pass 基底；材质请求首次加载后只追加固定球的不可变几何。
                // 任意模型、MI、贴图与对象 descriptor 仍按请求释放，不影响活动 World cache。
                baseCache = std::make_unique<RendererResourceCache>(cache.BeginActiveWorldCandidate(0));
                baseGraphics = graphics;
            }
            break;
        }
        case 5:
            if (objectIndex < plan.sceneAssetPlan.objectPlans.size())
            {
                const auto& object = plan.sceneAssetPlan.objectPlans[objectIndex];
                if (object.objectType != "mesh" ||
                    (object.objectName != "preview_model" && object.objectName != "preview_material"))
                { ++objectIndex; break; }
                RendererMeshLoadResult loaded;
                if (!LoadPreviewMesh(plan.sceneJson.at("objects").at(object.objectIndex),
                    *object.meshLoadRequest, context, loaded)) break;
                plan.meshObjectPlans.insert(plan.meshObjectPlans.end(), loaded.objectPlans.begin(), loaded.objectPlans.end());
                if (loaded.speedTreeWindProfile) plan.speedTreeWindProfiles.push_back(*loaded.speedTreeWindProfile);
                ++objectIndex;
            }
            else ++stage;
            break;
        case 6: ConfigureScene(); ++stage; break;
        case 7: Render(); ++stage; return true;
        default: return true;
        }
        return false;
    }
};

EditorThumbnailRenderSession::EditorThumbnailRenderSession(RendererBackendVulkan& backend, PipelineFactory& factory)
    : impl(std::make_unique<Impl>(backend, factory)) {}
EditorThumbnailRenderSession::~EditorThumbnailRenderSession() = default;
void EditorThumbnailRenderSession::Reset()
{
    // 图重建也保留唯一在途 CPU 任务；新 Impl 只丢弃其结果，不发起并行无界导入。
    auto pendingPreparation = std::move(impl->preparation);
    impl = std::make_unique<Impl>(impl->backend, impl->factory);
    impl->preparation = std::move(pendingPreparation);
    impl->preparation.Invalidate();
}
void EditorThumbnailRenderSession::Begin(const std::string& path, EditorUi::EditorAssetKind kind, const nlohmann::json& graph)
{
    if (!impl->baseCache || impl->graphJson != BuildEditorThumbnailGraph(graph))
        Reset();
    impl->preparation.Invalidate();
    impl->preparedAsset.reset();
    impl->objectIndex = 0;
    impl->image.reset();
    impl->error.clear();
    impl->resolved = {};
    impl->scene = {};
    if (impl->baseCache)
    {
        impl->cache = impl->baseCache->BeginActiveWorldCandidate(0);
        impl->graphics = impl->baseGraphics;
        impl->graphics.shaderVariants.insert(impl->thumbnailShaders.begin(), impl->thumbnailShaders.end());
        impl->stage = 5;
    }
    try
    {
        impl->graphJson = BuildEditorThumbnailGraph(graph);
        impl->plan = Require(WorldLoader().Load(CommonFunction::Path(ModelPreviewScenePath),
            kind == EditorUi::EditorAssetKind::MaterialInstance ? BuildMaterialPreviewObjects(path) : BuildModelPreviewObjects(path)));
    }
    catch (const std::exception& error) { impl->error = error.what(); impl->stage = 8; }
}
bool EditorThumbnailRenderSession::Step()
{
    bool done = true;
    try { done = impl->Step(); }
    catch (const std::exception& error) { impl->error = error.what(); }
    if (done) impl->ReleaseAsset();
    return done;
}
std::shared_ptr<const RendererGpuImageToken> EditorThumbnailRenderSession::Image() const { return impl->image; }
const std::string& EditorThumbnailRenderSession::Error() const { return impl->error; }
}
