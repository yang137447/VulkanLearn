#include "render/preview/materialPreviewRuntime.h"

#include <memory>
#include <utility>

#include "material.h"
#include "render/preview/materialPreviewDraftParsing.h"
#include "render/offline/offlineGroomAsset.h"
#include "render/resource/rendererResourceCache.h"
#include "renderSystem.h"

namespace VL
{

class MaterialInstancePreviewSession final
    : public VL::IRendererMaterialInstancePreviewSession
{
public:
    MaterialInstancePreviewSession(
        RenderSystem& ownerValue,
        VL::MaterialInstancePreviewWorldIdentity worldValue,
        VL::NormalizedMaterialInstancePath pathValue,
        uint64_t ownerGenerationValue,
        std::shared_ptr<MaterialInstance> materialInstanceValue,
        MaterialInstanceNumericParameterValues baselineParametersValue,
        MaterialInstanceStateSnapshot::TextureMap baselineTexturesValue,
        PreviewRenderStateValues baselineRenderStatesValue,
        VL::MaterialInstancePreviewDraft baselineDraftValue)
        : owner(ownerValue)
        , world(std::move(worldValue))
        , materialInstancePath(std::move(pathValue))
        , ownerGeneration(ownerGenerationValue)
        , materialInstance(std::move(materialInstanceValue))
        , baselineParameters(std::move(baselineParametersValue))
        , baselineTextures(std::move(baselineTexturesValue))
        , baselineRenderStates(std::move(baselineRenderStatesValue))
        , activeRenderStates(baselineRenderStates)
        , baselineDraft(std::move(baselineDraftValue))
    {
    }

    VL::MaterialInstancePreviewAdapterResult Apply(
        const VL::MaterialInstancePreviewDraft& draft) override
    {
        return Commit(draft, false);
    }

    VL::MaterialInstancePreviewAdapterResult RestoreBaseline(
        const VL::MaterialInstancePreviewDraft& baselineDraft) override
    {
        // 仍使用调用方 revision 参与门禁，但内容来自 connect 时捕获的源快照，
        // 避免外部 document 在连接期间改写 baseline 后影响运行时 Restore。
        VL::MaterialInstancePreviewDraft restoreDraft =
            this->baselineDraft;
        restoreDraft.documentRevision = baselineDraft.documentRevision;
        return Commit(restoreDraft, true);
    }

private:
    VL::MaterialInstancePreviewAdapterResult Commit(
        const VL::MaterialInstancePreviewDraft& draft,
        bool restoreBaseline)
    {
        if (owner.offlineMode)
            return owner.CommitMaterialInstancePreviewDraft(world, materialInstancePath, draft);
        const auto activeWorld = owner.GetActiveWorldIdentity();
        if (!activeWorld || *activeWorld != world ||
            !owner.activeOfflineWorldAssets ||
            !owner.activeOfflineWorldAssets->materialDefinitions.count(
                materialInstancePath.value))
        {
            VL::MaterialInstancePreviewAdapterResult result;
            result.status = VL::PreviewAdapterOperationStatus::Failed;
            result.failureStage = VL::PreviewAdapterFailureStage::Prepare;
            result.message = "Material instance preview lost its frozen World definition.";
            return result;
        }
        NumericPreviewDraftParseResult parsed =
        ParseNumericPreviewDraft(
                draft,
                materialInstancePath,
                baselineTextures,
                owner.activeOfflineWorldAssets->materialDefinitions.at(
                    materialInstancePath.value));
        if (!parsed.values.has_value())
        {
            VL::MaterialInstancePreviewAdapterResult result;
            result.status =
                VL::PreviewAdapterOperationStatus::Failed;
            result.failureStage =
                VL::PreviewAdapterFailureStage::Prepare;
            result.message = std::move(parsed.errorMessage);
            return result;
        }

        const bool renderStateChanged =
            parsed.renderStates != activeRenderStates;

        const std::optional<
            VL::RendererResourceCache::WorldLocalMaterialInstanceCapture>
            currentCapture =
                VL::RendererResourceCache::GetInstance()
                    .CaptureMaterialInstanceForGeneration(
                        ownerGeneration,
                        materialInstancePath.value);
        if (!currentCapture.has_value() ||
            currentCapture->ownerGeneration != ownerGeneration ||
            currentCapture->materialInstance.get() != materialInstance.get())
        {
            VL::MaterialInstancePreviewAdapterResult result;
            result.status =
                VL::PreviewAdapterOperationStatus::Failed;
            result.failureStage =
                VL::PreviewAdapterFailureStage::Prepare;
            result.message =
                "Material instance preview target is no longer the active World-local MI.";
            return result;
        }

        if (renderStateChanged)
        {
            VL::MaterialInstancePreviewAdapterResult result =
                owner.CommitMaterialInstancePreviewDraft(
                    world,
                    materialInstancePath,
                    draft);
            if (result.status ==
                VL::PreviewAdapterOperationStatus::Completed)
            {
                const std::optional<
                    VL::RendererResourceCache::WorldLocalMaterialInstanceCapture>
                    replacementCapture =
                        VL::RendererResourceCache::GetInstance()
                            .CaptureMaterialInstanceForGeneration(
                                ownerGeneration,
                                materialInstancePath.value);
                if (!replacementCapture.has_value() ||
                    !replacementCapture->materialInstance)
                {
                    result.status =
                        VL::PreviewAdapterOperationStatus::Failed;
                    result.failureStage =
                        VL::PreviewAdapterFailureStage::Commit;
                    result.liveSwapCommitted = false;
                    result.replacesLiveResource = false;
                    result.message =
                        "Material instance preview committed without an active replacement MI.";
                    return result;
                }
                materialInstance = replacementCapture->materialInstance;
                ownerGeneration = replacementCapture->ownerGeneration;
                activeRenderStates = parsed.renderStates;
            }
            return result;
        }

        std::shared_ptr<const VL::OfflineWorldAssets> preparedOfflineAssets;
        try
        {
            preparedOfflineAssets = owner.PrepareOfflineMaterialPreview(
                materialInstancePath.value, *parsed.serializedJson);
        }
        catch (const std::exception& exception)
        {
            VL::MaterialInstancePreviewAdapterResult result;
            result.status =
                VL::PreviewAdapterOperationStatus::Failed;
            result.failureStage =
                VL::PreviewAdapterFailureStage::Prepare;
            result.message =
                std::string("Offline material preview prepare failed: ") +
                exception.what();
            return result;
        }
        try
        {
            // 外部 profile 可能在准备与数值提交之间变化；先复核字节身份，
            // 否则 GPU 草稿与冻结 CPU 参数会指向不同版本。
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
            materialInstance->CommitNumericParameterValues(
                restoreBaseline ? baselineParameters : *parsed.values);
        }
        catch (const std::exception& exception)
        {
            VL::MaterialInstancePreviewAdapterResult result;
            result.status =
                VL::PreviewAdapterOperationStatus::Failed;
            result.failureStage =
                VL::PreviewAdapterFailureStage::Commit;
            result.message =
                std::string("Material instance numeric preview commit failed: ") +
                exception.what();
            return result;
        }
        owner.activeOfflineWorldAssets.swap(preparedOfflineAssets);

        VL::MaterialInstancePreviewAdapterResult result;
        result.status =
            VL::PreviewAdapterOperationStatus::Completed;
        result.runtimeResourceGeneration = ownerGeneration;
        result.liveSwapCommitted = true;
        result.replacesLiveResource = false;
        result.message = restoreBaseline
            ? "Material instance numeric preview baseline restored."
            : "Material instance numeric preview applied.";
        return result;
    }

    RenderSystem& owner;
    VL::MaterialInstancePreviewWorldIdentity world;
    VL::NormalizedMaterialInstancePath materialInstancePath;
    uint64_t ownerGeneration = 0;
    std::shared_ptr<MaterialInstance> materialInstance;
    MaterialInstanceNumericParameterValues baselineParameters;
    MaterialInstanceStateSnapshot::TextureMap baselineTextures;
    PreviewRenderStateValues baselineRenderStates;
    PreviewRenderStateValues activeRenderStates;
    VL::MaterialInstancePreviewDraft baselineDraft;
};

class OfflineMaterialInstancePreviewSession final
    : public VL::IRendererMaterialInstancePreviewSession
{
public:
    OfflineMaterialInstancePreviewSession(RenderSystem& owner,
        VL::MaterialInstancePreviewWorldIdentity world,
        VL::NormalizedMaterialInstancePath path,
        VL::MaterialInstancePreviewDraft baseline)
        : owner(owner), world(std::move(world)), path(std::move(path)),
          baseline(std::move(baseline)) {}
    VL::MaterialInstancePreviewAdapterResult Apply(
        const VL::MaterialInstancePreviewDraft& draft) override
    {
        return owner.CommitMaterialInstancePreviewDraft(world, path, draft);
    }
    VL::MaterialInstancePreviewAdapterResult RestoreBaseline(
        const VL::MaterialInstancePreviewDraft& draft) override
    {
        auto restored = baseline;
        restored.documentRevision = draft.documentRevision;
        return owner.CommitMaterialInstancePreviewDraft(world, path, restored);
    }
private:
    RenderSystem& owner;
    VL::MaterialInstancePreviewWorldIdentity world;
    VL::NormalizedMaterialInstancePath path;
    VL::MaterialInstancePreviewDraft baseline;
};
std::shared_ptr<IRendererMaterialInstancePreviewSession>
MakeMaterialInstancePreviewSession(
    RenderSystem& owner,
    MaterialInstancePreviewWorldIdentity world,
    NormalizedMaterialInstancePath path,
    uint64_t ownerGeneration,
    std::shared_ptr<MaterialInstance> materialInstance,
    MaterialInstanceNumericParameterValues baselineParameters,
    MaterialInstanceStateSnapshot::TextureMap baselineTextures,
    PreviewRenderStateValues baselineRenderStates,
    MaterialInstancePreviewDraft baselineDraft)
{
    return std::make_shared<MaterialInstancePreviewSession>(
        owner,
        std::move(world),
        std::move(path),
        ownerGeneration,
        std::move(materialInstance),
        std::move(baselineParameters),
        std::move(baselineTextures),
        std::move(baselineRenderStates),
        std::move(baselineDraft));
}

std::shared_ptr<IRendererMaterialInstancePreviewSession>
MakeOfflineMaterialInstancePreviewSession(
    RenderSystem& owner,
    MaterialInstancePreviewWorldIdentity world,
    NormalizedMaterialInstancePath path,
    MaterialInstancePreviewDraft baseline)
{
    return std::make_shared<OfflineMaterialInstancePreviewSession>(
        owner,
        std::move(world),
        std::move(path),
        std::move(baseline));
}

} // namespace VL
