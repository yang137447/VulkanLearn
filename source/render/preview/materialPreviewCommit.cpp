#include "render/preview/materialPreviewCommit.h"

#include "renderSystem.h"

#include "commonFunction.h"
#include "core/runtimeResult.h"
#include "material/loader/materialInstanceResolver.h"
#include "material/materialAssetUtils.h"
#include "material.h"
#include "materialInstance.h"
#include "pipeline/pipelineFactory.h"
#include "render/backend/rendererObjectResourceRegistry.h"
#include "render/offline/offlineGroomAsset.h"
#include "render/offline/offlineSceneAdapter.h"
#include "render/preview/materialPreviewDraftParsing.h"
#include "render/preview/materialPreviewRuntime.h"
#include "render/resource/rendererMaterialLoader.h"
#include "render/resource/rendererResourceCache.h"
#include "render/resource/rendererResourceLoadContext.h"
#include "render/resource/resourceRetireQueue.h"
#include "renderGraph.h"
#include "shader/build/contentHash.h"

#include <filesystem>
#include <fstream>
#include <iterator>
#include <stdexcept>
#include <string>

namespace VL
{
void ApplyMaterialOverrideToRenderScene(
    VL::RenderScene& renderScene,
    const std::string& materialInstancePath,
    const std::string& materialKey)
{
    for (VL::RenderDrawPacket& drawPacket : renderScene.drawPackets)
    {
        if (drawPacket.materialInstance.key == materialInstancePath)
        {
            drawPacket.material = VL::ResourceHandle{materialKey, 0};
        }
    }
}

void RebuildRenderSceneMaterialGroupsImpl(VL::RenderScene& renderScene)
{
    renderScene.materialGroups.clear();
    for (size_t drawPacketIndex = 0;
         drawPacketIndex < renderScene.drawPackets.size();
         ++drawPacketIndex)
    {
        const VL::RenderDrawPacket& drawPacket =
            renderScene.drawPackets[drawPacketIndex];
        VL::MaterialDrawGroup* materialGroup = nullptr;
        for (VL::MaterialDrawGroup& candidateGroup :
             renderScene.materialGroups)
        {
            if (candidateGroup.material.key == drawPacket.material.key &&
                candidateGroup.material.generation ==
                    drawPacket.material.generation)
            {
                materialGroup = &candidateGroup;
                break;
            }
        }
        if (materialGroup == nullptr)
        {
            VL::MaterialDrawGroup newGroup;
            newGroup.material = drawPacket.material;
            renderScene.materialGroups.push_back(std::move(newGroup));
            materialGroup = &renderScene.materialGroups.back();
        }

        VL::MaterialInstanceDrawGroup* materialInstanceGroup = nullptr;
        for (VL::MaterialInstanceDrawGroup& candidateGroup :
             materialGroup->materialInstances)
        {
            if (candidateGroup.materialInstance.key ==
                    drawPacket.materialInstance.key &&
                candidateGroup.materialInstance.generation ==
                    drawPacket.materialInstance.generation)
            {
                materialInstanceGroup = &candidateGroup;
                break;
            }
        }
        if (materialInstanceGroup == nullptr)
        {
            VL::MaterialInstanceDrawGroup newGroup;
            newGroup.materialInstance = drawPacket.materialInstance;
            materialGroup->materialInstances.push_back(std::move(newGroup));
            materialInstanceGroup =
                &materialGroup->materialInstances.back();
        }
        materialInstanceGroup->drawPacketIndices.push_back(drawPacketIndex);
    }
}

} // namespace VL

VL::MaterialInstancePreviewAdapterResult
RenderSystem::CaptureMaterialInstancePreviewSession(
    const VL::MaterialInstancePreviewAdapterCommand& command,
    std::shared_ptr<
        VL::IRendererMaterialInstancePreviewSession>&
        outSession)
{
    using namespace VL;

    outSession.reset();
    MaterialInstancePreviewAdapterResult result;
    result.failureStage = PreviewAdapterFailureStage::Prepare;

    if (!command.world.IsValid() ||
        !command.materialInstancePath.IsValid())
    {
        result.status = PreviewAdapterOperationStatus::Failed;
        result.message =
            "Renderer owner received an invalid World identity or MI path.";
        return result;
    }

    const std::optional<MaterialInstancePreviewWorldIdentity> activeWorldIdentity =
        GetActiveWorldIdentity();
    if (!activeWorldIdentity.has_value())
    {
        result.status = PreviewAdapterOperationStatus::Unavailable;
        result.message =
            "Renderer owner cannot capture a preview target without an active World.";
        return result;
    }
    if (*activeWorldIdentity != command.world)
    {
        result.status = PreviewAdapterOperationStatus::Failed;
        result.message =
            "Renderer owner rejected a preview target from an obsolete World generation.";
        return result;
    }
    if (offlineMode)
    {
        if (!activeOfflineWorldAssets)
        {
            result.status = PreviewAdapterOperationStatus::Failed;
            result.message = "Frozen offline World assets are missing";
            return result;
        }
        const auto found = activeOfflineWorldAssets->materialInstanceSources.find(
            command.materialInstancePath.value);
        if (found == activeOfflineWorldAssets->materialInstanceSources.end())
        {
            result.status = PreviewAdapterOperationStatus::Failed;
            result.message = "Frozen offline MI is missing";
            return result;
        }
        MaterialInstancePreviewDraft baseline;
        baseline.materialInstancePath = command.materialInstancePath.value;
        baseline.documentRevision = command.documentRevision.value_or(0);
        baseline.serializedWorkingDraft = found->second.dump();
        outSession = VL::MakeOfflineMaterialInstancePreviewSession(
            *this, command.world, command.materialInstancePath, std::move(baseline));
        result.status = PreviewAdapterOperationStatus::Completed;
        result.runtimeResourceGeneration = command.world.generation;
        result.message = "Frozen offline material preview connected";
        return result;
    }

    const std::optional<
        VL::RendererResourceCache::WorldLocalMaterialInstanceCapture>
        capture =
            VL::RendererResourceCache::GetInstance()
                .CaptureMaterialInstanceForGeneration(
                    command.world.generation,
                    command.materialInstancePath.value);
    if (!capture.has_value() || !capture->materialInstance)
    {
        result.status = PreviewAdapterOperationStatus::Failed;
        result.message =
            "Active World-local MaterialInstance was not found for the normalized MI path.";
        return result;
    }

    try
    {
        const MaterialInstanceStateSnapshot snapshot =
            capture->materialInstance->CaptureStateSnapshot();
        if (!activeOfflineWorldAssets)
        {
            result.status = PreviewAdapterOperationStatus::Failed;
            result.message =
                "Renderer owner has no frozen material assets.";
            return result;
        }
        // Connect 的 baseline 与 World 提交时冻结的源一致；磁盘上后来的
        // MI/M_ 改动不能在 Restore 时悄悄混入当前有效版本。
        const nlohmann::json& materialInstanceJson =
            activeOfflineWorldAssets->materialInstanceSources.at(
                command.materialInstancePath.value);
        const MaterialInstanceResolveResult resolved =
            MaterialInstanceResolver::ResolveWithDefinition(
                command.materialInstancePath.value,
                materialInstanceJson,
                activeOfflineWorldAssets->materialDefinitions.at(
                    command.materialInstancePath.value));
        const VL::PreviewRenderStateValues baselineRenderStates =
            VL::BuildPreviewRenderStateValues(
                resolved.effectiveMaterialInstanceJson);
        VL::MaterialInstancePreviewDraft baselineDraft;
        baselineDraft.materialInstancePath =
            command.materialInstancePath.value;
        baselineDraft.documentRevision =
            command.documentRevision.value_or(0);
        baselineDraft.serializedWorkingDraft =
            materialInstanceJson.dump();
        outSession = VL::MakeMaterialInstancePreviewSession(
            *this,
            command.world,
            command.materialInstancePath,
            capture->ownerGeneration,
            capture->materialInstance,
            snapshot.parameters,
            snapshot.textures,
            baselineRenderStates,
            std::move(baselineDraft));
    }
    catch (const std::exception& exception)
    {
        result.status = PreviewAdapterOperationStatus::Failed;
        result.message =
            std::string("Renderer owner failed to capture MI numeric baseline: ") +
            exception.what();
        return result;
    }

    result.status = PreviewAdapterOperationStatus::Completed;
    result.runtimeResourceGeneration = capture->ownerGeneration;
    result.liveSwapCommitted = false;
    result.replacesLiveResource = false;
    result.message =
        "Renderer-owned numeric material instance preview connected.";
    return result;
}

std::shared_ptr<const VL::OfflineWorldAssets>
RenderSystem::PrepareOfflineMaterialPreview(
    const std::string& materialInstancePath,
    const nlohmann::json& materialInstanceJson) const
{
    if (!activeOfflineWorldAssets)
        throw std::runtime_error("Active World has no frozen offline assets");

    const std::string key =
        MaterialAssetUtils::NormalizeAssetPath(materialInstancePath);
    if (activeOfflineWorldAssets->effectiveMaterialInstances.find(key) ==
        activeOfflineWorldAssets->effectiveMaterialInstances.end())
        throw std::runtime_error("Preview MI is absent from the frozen World: " + key);
    // MI 可以覆盖参数，却不能在旧 World 的几何/材质槽下改绑另一份 M_。
    // 这种改绑须经过 World 事务，避免 CPU 与 GPU 候选解释不同的定义。
    const std::string frozenMaterial =
        activeOfflineWorldAssets->materialInstanceSources.at(key)
            .at("material").get<std::string>();
    if (!materialInstanceJson.is_object() ||
        !materialInstanceJson.contains("material") ||
        !materialInstanceJson["material"].is_string() ||
        MaterialAssetUtils::NormalizeAssetPath(
            materialInstanceJson["material"].get<std::string>()) !=
            MaterialAssetUtils::NormalizeAssetPath(frozenMaterial))
        throw std::runtime_error("Preview MI cannot change its frozen material definition: " + key);

    const MaterialInstanceResolveResult resolved =
        MaterialInstanceResolver::ResolveWithDefinition(
            key, materialInstanceJson,
            activeOfflineWorldAssets->materialDefinitions.at(key));
    // 候选只替换一个 MI 的有效值；旧包和正在运行的 CPU 任务始终只读旧版本。
    auto candidate = std::make_shared<VL::OfflineWorldAssets>(
        *activeOfflineWorldAssets);
    candidate->effectiveMaterialInstances.insert_or_assign(
        key, resolved.effectiveMaterialInstanceJson);
    candidate->materialDefinitions.insert_or_assign(key, resolved.materialJson);
    candidate->materialInstanceSources.insert_or_assign(key, materialInstanceJson);
    VL::FreezeOfflineMaterialTextures(*candidate, resolved.effectiveMaterialInstanceJson);

    if (resolved.effectiveMaterialInstanceJson.contains("modelProfile"))
    {
        const std::string reference =
            resolved.effectiveMaterialInstanceJson["modelProfile"].get<std::string>();
        const std::filesystem::path physical = CommonFunction::Path(reference);
        std::ifstream file(physical, std::ios::binary);
        if (!file)
            throw std::runtime_error("Cannot open modelProfile: " + reference);
        const std::string bytes(
            std::istreambuf_iterator<char>(file),
            std::istreambuf_iterator<char>{});
        if (file.bad())
            throw std::runtime_error("Cannot read modelProfile: " + reference);
        candidate->modelProfiles.insert_or_assign(
            reference, nlohmann::json::parse(bytes));
        candidate->modelProfilePaths.insert_or_assign(reference, physical);
        candidate->modelProfileDigests.insert_or_assign(
            reference, VL::ContentHasher::HashString(bytes).ToHex());
    }
    VL::PruneUnusedOfflineMaterialAssets(*candidate);
    return candidate;
}

VL::MaterialInstancePreviewAdapterResult
RenderSystem::CommitMaterialInstancePreviewDraft(
    const VL::MaterialInstancePreviewWorldIdentity& world,
    const VL::NormalizedMaterialInstancePath& materialInstancePath,
    const VL::MaterialInstancePreviewDraft& draft)
{
    using namespace VL;

    MaterialInstancePreviewAdapterResult result;
    result.failureStage = PreviewAdapterFailureStage::Prepare;
    if (offlineMode)
    {
        try
        {
            const auto draftPath = NormalizeMaterialInstancePath(draft.materialInstancePath);
            if (!world.IsValid() || !materialInstancePath.IsValid() ||
                GetActiveWorldIdentity() != world || !activeOfflineWorldAssets ||
                !draftPath.Succeeded() ||
                *draftPath.path != materialInstancePath)
                throw std::runtime_error("Offline preview targets an obsolete World or MI");
            const auto source = nlohmann::json::parse(draft.serializedWorkingDraft);
            auto candidate = PrepareOfflineMaterialPreview(materialInstancePath.value, source);
            VL::WorldSnapshotBuilder builder;
            VL::WorldSnapshotBuildDesc desc;
            desc.worldGeneration = world.generation;
            auto snapshot = builder.Build(*activeWorld, desc);
            if (snapshot.IsFailure())
                throw std::runtime_error(VL::FormatRuntimeError(snapshot.Error()));
            auto settings = offlineSettings;
            settings.width = GetSceneRenderExtent().width;
            settings.height = GetSceneRenderExtent().height;
            const auto input = VL::PreflightOfflineRenderInput(snapshot.Value(), *candidate, settings);
            if (input.IsFailure())
                throw std::runtime_error(VL::FormatRuntimeError(input.Error()));
            for (const auto& [reference, digest] : candidate->modelProfileDigests)
                if (VL::ContentHasher::HashFile(candidate->modelProfilePaths.at(reference)).ToHex() != digest)
                    throw std::runtime_error("modelProfile changed before preview commit: " + reference);
            VL::ValidateOfflineTexturesStillCurrent(*candidate);
            auto published = std::make_shared<VL::OfflineWorldAssets>(*candidate);
            // GPU 仍是旧版本；回切实时必须按此成功 Apply 的冻结值重建。
            published->realtimePrepared = false;
            WaitForRendererMutationSafePoint();
            activeOfflineWorldAssets = std::move(published);
            result.status = PreviewAdapterOperationStatus::Completed;
            result.failureStage = PreviewAdapterFailureStage::None;
            result.runtimeResourceGeneration = world.generation;
            result.liveSwapCommitted = true;
            result.message = "Offline effective preview committed; realtime pending synchronization";
        }
        catch (const std::exception& exception)
        {
            result.status = PreviewAdapterOperationStatus::Failed;
            result.message = exception.what();
        }
        return result;
    }
    if (!world.IsValid() || !materialInstancePath.IsValid())
    {
        result.status = PreviewAdapterOperationStatus::Failed;
        result.message =
            "Material instance preview replacement received an invalid identity.";
        return result;
    }
    if (rendererBackend == nullptr || pipelineFactory == nullptr)
    {
        result.status = PreviewAdapterOperationStatus::Unavailable;
        result.message =
            "Material instance preview replacement requires initialized renderer services.";
        return result;
    }
    if (!activeWorld || !hasRenderScene ||
        activeWorld->GetGeneration() != world.generation ||
        currentRenderScene.worldGeneration != world.generation)
    {
        result.status = PreviewAdapterOperationStatus::Failed;
        result.message =
            "Material instance preview replacement targets an obsolete or uninitialized World.";
        return result;
    }

    VL::RendererResourceCache& activeCache =
        VL::RendererResourceCache::GetInstance();
    const std::optional<VL::RendererResourceCache::WorldLocalMaterialInstanceCapture>
        activeCapture = activeCache.CaptureMaterialInstanceForGeneration(
            world.generation,
            materialInstancePath.value);
    if (!activeCapture.has_value() || !activeCapture->materialInstance)
    {
        result.status = PreviewAdapterOperationStatus::Failed;
        result.message =
            "Active World-local MaterialInstance was not found for the preview replacement.";
        return result;
    }

    VL::NumericPreviewDraftParseResult parsed =
        VL::ParseNumericPreviewDraft(
            draft,
            materialInstancePath,
            activeCapture->materialInstance->CaptureStateSnapshot().textures,
            activeOfflineWorldAssets->materialDefinitions.at(
                materialInstancePath.value));
    if (!parsed.serializedJson.has_value())
    {
        result.status = PreviewAdapterOperationStatus::Failed;
        result.message = std::move(parsed.errorMessage);
        return result;
    }

    size_t targetDrawCount = 0;
    for (const VL::RenderDrawPacket& drawPacket : currentRenderScene.drawPackets)
    {
        if (drawPacket.materialInstance.key == materialInstancePath.value)
        {
            ++targetDrawCount;
        }
    }
    if (targetDrawCount == 0)
    {
        result.status = PreviewAdapterOperationStatus::Failed;
        result.message =
            "The preview material instance is not referenced by the active RenderScene.";
        return result;
    }

    try
    {
        std::shared_ptr<const VL::OfflineWorldAssets> preparedOfflineAssets =
            PrepareOfflineMaterialPreview(
                materialInstancePath.value, *parsed.serializedJson);
        VL::RendererResourceCache candidateCache =
            activeCache.BeginActiveWorldCandidate(world.generation);
        RenderGraph& renderGraph = RenderGraph::GetInstance();
        PipelineFactory::GraphicsCandidateState graphicsCandidateState;
        VL::RendererResourceLoadContext loadContext{
            candidateCache,
            renderGraph};
        loadContext.graphicsCandidateState = &graphicsCandidateState;
        // GPU 候选与离线候选消费同一份冻结定义和有效覆写，不能再读磁盘 M_。
        auto frozenGpuAssets = std::make_shared<VL::OfflineWorldAssets>(
            *preparedOfflineAssets);
        loadContext.offlineWorldAssets = frozenGpuAssets.get();
        loadContext.useFrozenMaterials = true;
        // MI 预览候选复用主渲染的预蒙皮选择，避免同一材质在预览里切回混合路径。
        loadContext.prerenderSkin = prerenderSkinEnabled;

        VL::RendererMaterialLoader materialLoader(
            *pipelineFactory,
            *rendererBackend,
            loadContext);
        const std::shared_ptr<MaterialInstance> replacementMaterialInstance =
            materialLoader.LoadSceneMaterialInstance(materialInstancePath.value);
        if (!replacementMaterialInstance)
        {
            throw std::runtime_error(
                "Renderer material loader returned an empty preview MaterialInstance");
        }
        const std::shared_ptr<Material> replacementMaterial =
            replacementMaterialInstance->GetBaseMaterial().lock();
        if (!replacementMaterial)
        {
            throw std::runtime_error(
                "Preview MaterialInstance has no replacement base Material");
        }

        VL::RenderScene candidateRenderScene = currentRenderScene;
        ApplyMaterialOverrideToRenderScene(
            candidateRenderScene,
            materialInstancePath.value,
            replacementMaterial->GetMaterialKey());
        RebuildRenderSceneMaterialGroupsImpl(candidateRenderScene);

        auto resolvedSceneResult = resolvedRenderSceneBuilder.Build(
            candidateRenderScene,
            candidateCache);
        if (resolvedSceneResult.IsFailure())
        {
            throw std::runtime_error(
                VL::FormatRuntimeError(resolvedSceneResult.Error()));
        }
        VL::ResolvedRenderScene candidateResolvedRenderScene =
            std::move(resolvedSceneResult.Value());

        const VL::RendererDescriptorContext descriptorContext =
            BuildRendererDescriptorContext(
                candidateCache,
                renderGraph,
                GetLightBufferInfo());
        VL::RendererObjectResourceRegistry objectResourceRegistry;
        objectResourceRegistry.InitializeResolvedSceneResources(
            *rendererBackend,
            descriptorContext,
            candidateRenderScene,
            candidateResolvedRenderScene,
            candidateCache,
            prerenderSkinEnabled);

        PipelineFactory::PreparedGraphicsCandidateCommit preparedPipelineCommit =
            pipelineFactory->PrepareCandidateCommit(graphicsCandidateState);

        const std::string replacementMaterialKey =
            replacementMaterial->GetMaterialKey();
        std::string replacementPath = materialInstancePath.value;
        std::string replacementKey = replacementMaterialKey;
        VL::RenderScene candidatePickingRenderScene =
            pickingRuntime.CopyPickingScene();
        ApplyMaterialOverrideToRenderScene(
            candidatePickingRenderScene,
            materialInstancePath.value,
            replacementMaterialKey);
        RebuildRenderSceneMaterialGroupsImpl(candidatePickingRenderScene);

        VL::ResourceRetireQueue& retireQueue =
            VL::ResourceRetireQueue::GetInstance();
        std::vector<VL::RetiredResource> retirementResources;
        retirementResources.reserve(1);
        retirementResources.push_back({
            "MaterialInstancePreview:WorldLocalResources",
            world.generation,
            retireQueue.GetLastSubmittedEpoch(),
            {}});
        VL::PreparedResourceRetirements preparedRetirements =
            retireQueue.PrepareRetirements(std::move(retirementResources));

        for (const auto& [reference, captured] :
             preparedOfflineAssets->modelProfileDigests)
        {
            const auto& path =
                preparedOfflineAssets->modelProfilePaths.at(reference);
            if (!std::filesystem::is_regular_file(path) ||
                VL::ContentHasher::HashFile(path).ToHex() != captured)
                throw std::runtime_error(
                    "modelProfile changed before preview commit: " + reference);
        }
        VL::ValidateOfflineTexturesStillCurrent(*preparedOfflineAssets);
        // Candidate resources are isolated; only the publication below touches
        // state that RT reads. Wait exactly at the ownership hand-off.
        WaitForRendererMutationSafePoint();
        pipelineFactory->CommitPreparedCandidate(
            std::move(preparedPipelineCommit));
        VL::RendererResourceCache::WorldLocalResourcePackageHandle retiredPackage =
        activeCache.CommitCandidate(std::move(candidateCache));
        std::swap(currentRenderScene, candidateRenderScene);
        std::swap(currentResolvedRenderScene, candidateResolvedRenderScene);
        pickingRuntime.SwapPickingScene(candidatePickingRenderScene);
        activeOfflineWorldAssets.swap(preparedOfflineAssets);

        hasMaterialInstancePreviewMaterialOverride = true;
        materialInstancePreviewOverridePath.swap(replacementPath);
        materialInstancePreviewOverrideMaterialKey.swap(replacementKey);

        preparedRetirements.GetAdditionalResource(0).resource =
            std::static_pointer_cast<void>(std::move(retiredPackage));
        retireQueue.CommitPreparedRetirements(
            std::move(preparedRetirements));

        result.status = PreviewAdapterOperationStatus::Completed;
        result.failureStage = PreviewAdapterFailureStage::None;
        result.runtimeResourceGeneration = world.generation;
        result.liveSwapCommitted = true;
        result.replacesLiveResource = true;
        result.message =
            "Material instance render-state preview applied with a live resource replacement.";
        return result;
    }
    catch (const std::exception& exception)
    {
        result.status = PreviewAdapterOperationStatus::Failed;
        result.failureStage = PreviewAdapterFailureStage::Prepare;
        result.message =
            std::string("Material instance preview resource replacement failed: ") +
            exception.what();
        return result;
    }
}
