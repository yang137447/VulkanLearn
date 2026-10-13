#include "engine/editorThumbnailRenderSession.h"

#include <cstring>
#include <stdexcept>
#include "commonFunction.h"
#include "engine/editorThumbnailGraph.h"
#include "engine/modelPreviewCamera.h"
#include "engine/modelPreviewRequest.h"
#include "material.h"
#include "materialInstance.h"
#include "sceneNode.h"
#include "pipeline/pipelineFactory.h"
#include "profiler.h"
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
    RenderScene scene;
    ResolvedRenderScene resolved;
    std::shared_ptr<const EditorUi::EditorTexturePreview> image;
    std::string error;
    nlohmann::json graphJson;
    size_t objectIndex = 0;
    uint32_t mip = 0;
    int stage = 0;
    bool descriptorsReady = false;

    Impl(RendererBackendVulkan& backend, PipelineFactory& factory) : backend(backend), factory(factory) {}
    ~Impl()
    {
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
        cache = baseCache->BeginActiveWorldCandidate(0);
        graphics = baseGraphics;
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
        constexpr size_t bytes = 256 * 256 * 4;
        const auto staging = backend.CreateBuffer(bytes, vk::BufferUsageFlagBits::eTransferDst,
            vk::MemoryPropertyFlagBits::eHostVisible | vk::MemoryPropertyFlagBits::eHostCoherent, "ThumbnailReadback");
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
            vk::BufferImageCopy copy;
            copy.setImageSubresource({vk::ImageAspectFlagBits::eColor, 0, 0, 1}).setImageExtent({256, 256, 1});
            command.copyImageToBuffer(output.image, vk::ImageLayout::eTransferSrcOptimal, staging.first, copy);
            backend.EndSingleTimeCommands(command);
            auto pixels = std::make_shared<EditorUi::EditorTexturePreview>();
            pixels->width = pixels->height = pixels->sourceWidth = pixels->sourceHeight = 256;
            pixels->pixels.resize(bytes);
            void* mapped = backend.MapMemory(staging.second, bytes);
            std::memcpy(pixels->pixels.data(), mapped, bytes);
            backend.UnmapMemory(staging.second);
            image = std::move(pixels);
        }
        catch (...)
        {
            if (command) backend.AbortSingleTimeCommands(command);
            backend.DestroyBuffer(staging.first, staging.second);
            throw;
        }
        backend.DestroyBuffer(staging.first, staging.second);
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
                if (mip == ibl.GetPrefilterMipCount()) { ibl.RecordCommit(command); ++stage; }
                backend.EndSingleTimeCommands(command);
            }
            catch (...) { if (command) backend.AbortSingleTimeCommands(command); throw; }
            if (stage == 5)
            {
                // 只保留环境与 pass 资源。每次资产请求从这份基底创建候选，
                // 上一个模型/贴图/对象 descriptor 不累积，也不影响活动 World cache。
                baseCache = std::make_unique<RendererResourceCache>(cache.BeginActiveWorldCandidate(0));
                baseGraphics = graphics;
            }
            break;
        }
        case 5:
            for (const auto& object : plan.sceneAssetPlan.objectPlans)
            {
                if (object.objectType != "mesh" ||
                    (object.objectName != "preview_model" && object.objectName != "preview_material")) continue;
                auto loaded = RendererMeshLoader(factory, backend, context).LoadMeshObject(
                    plan.sceneJson.at("objects").at(object.objectIndex), *object.meshLoadRequest);
                plan.meshObjectPlans.insert(plan.meshObjectPlans.end(), loaded.objectPlans.begin(), loaded.objectPlans.end());
                if (loaded.speedTreeWindProfile) plan.speedTreeWindProfiles.push_back(*loaded.speedTreeWindProfile);
            }
            ++stage;
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
void EditorThumbnailRenderSession::Begin(const std::string& path, EditorUi::EditorAssetKind kind, const nlohmann::json& graph)
{
    if (!impl->baseCache || impl->graphJson != BuildEditorThumbnailGraph(graph))
        impl = std::make_unique<Impl>(impl->backend, impl->factory);
    impl->image.reset();
    impl->error.clear();
    impl->resolved = {};
    impl->scene = {};
    if (impl->baseCache)
    {
        impl->cache = impl->baseCache->BeginActiveWorldCandidate(0);
        impl->graphics = impl->baseGraphics;
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
std::shared_ptr<const EditorUi::EditorTexturePreview> EditorThumbnailRenderSession::Image() const { return impl->image; }
const std::string& EditorThumbnailRenderSession::Error() const { return impl->error; }
}
