#include "render/cloth/clothComputeReloadParticipant.h"

#include <memory>
#include <stdexcept>
#include <utility>

#include "pipeline/computePipeline.h"
#include "pipeline/pipelineFactory.h"
#include "render/backend/rendererBackendVulkan.h"
#include "render/cloth/clothResourceLoader.h"
#include "render/cloth/clothResourceSet.h"
#include "render/resource/resourceRetireQueue.h"
#include "render/resource/rendererResourceCache.h"

namespace VL
{
namespace
{

struct ClothReplacementPayload
{
    RendererResourceCache::PreparedClothResourceReplacement cacheReplacement;
    std::shared_ptr<ClothResourceSet> resourceSet;
};

} // namespace

void ClothComputeReloadParticipant::Initialize(
    PipelineFactory& pipelineFactory,
    RendererBackendVulkan& rendererBackend)
{
    if (initialized)
    {
        return;
    }

    ComputeShaderArtifact artifact;
    std::shared_ptr<ComputePipeline> pipeline =
        pipelineFactory.CreateComputePipeline(
            "generator/clothLookupTables",
            &artifact);
    if (!pipeline || artifact.artifactGenerationKey.empty())
    {
        throw std::runtime_error(
            "Cloth Compute reload participant received an invalid active pipeline");
    }

    this->pipelineFactory = &pipelineFactory;
    this->rendererBackend = &rendererBackend;
    activePipeline = std::move(pipeline);
    activeArtifact = std::move(artifact);
    activeResourceSet.reset();
    descriptorRefreshRequired = false;
    initialized = true;
}

void ClothComputeReloadParticipant::Shutdown() noexcept
{
    activeResourceSet.reset();
    activePipeline.reset();
    activeArtifact = {};
    rendererBackend = nullptr;
    pipelineFactory = nullptr;
    descriptorRefreshRequired = false;
    initialized = false;
}

std::string ClothComputeReloadParticipant::GetShaderName() const
{
    return "generator/clothLookupTables";
}

std::shared_ptr<ComputePipeline>
ClothComputeReloadParticipant::GetActivePipeline() const
{
    return activePipeline;
}

const ComputeShaderArtifact&
ClothComputeReloadParticipant::GetActiveArtifact() const
{
    return activeArtifact;
}

ComputeDescriptorReplacement
ClothComputeReloadParticipant::PrepareReplacementDescriptors(
    const ComputeShaderArtifact& candidate,
    const std::shared_ptr<ComputePipeline>& replacementPipeline) const
{
    if (!initialized || rendererBackend == nullptr ||
        !replacementPipeline || candidate.artifactGenerationKey.empty())
    {
        throw std::runtime_error(
            "Cloth Compute reload candidate is missing initialized owner state");
    }

    const RendererResourceCache::ImmutableWorldLocalResourceRefs activePackage =
        RendererResourceCache::GetInstance()
            .CaptureActiveWorldLocalResources();
    if (!activePackage || !activePackage->clothResources ||
        !activePackage->clothResources->directionalAlbedoLutTexture)
    {
        throw std::runtime_error(
            "Cloth Compute reload requires an active World-local Cloth resource package");
    }

    const std::string sourceDigest = BuildClothResourceSourceDigest(
        candidate.artifactGenerationKey);
    std::shared_ptr<ClothResourceSet> resourceSet =
        BuildClothResourceSet(
            *rendererBackend,
            replacementPipeline,
            sourceDigest);

    ClothReplacementPayload payload;
    payload.resourceSet = resourceSet;
    payload.cacheReplacement =
        RendererResourceCache::GetInstance()
            .PrepareClothResourceReplacement(
                std::shared_ptr<const ClothResourceSet>(resourceSet),
                resourceSet->directionalAlbedoLutTexture);

    const RendererResourceCache::ImmutableWorldLocalResourceRefs oldPackage =
        payload.cacheReplacement.previousPackage;
    ComputeDescriptorReplacement replacement;
    replacement.minimumRetirementEpoch =
        ResourceRetireQueue::GetInstance().GetLastSubmittedEpoch() + 1;
    replacement.replacementPayload =
        std::static_pointer_cast<void>(
            std::make_shared<ClothReplacementPayload>(std::move(payload)));
    replacement.retirement = MakePreparedRetiredResourcePackage(
        [oldPackage]() mutable
        {
            std::shared_ptr<const RendererResourceCache::WorldLocalResourcePackage>
                retiredPackage = oldPackage;
            retiredPackage.reset();
        });
    return replacement;
}

void ClothComputeReloadParticipant::CommitReplacement(
    ComputeShaderArtifact committedArtifact,
    std::shared_ptr<ComputePipeline> replacementPipeline,
    ComputeDescriptorReplacement&& replacementDescriptors) noexcept
{
    std::shared_ptr<ClothReplacementPayload> payload =
        std::static_pointer_cast<ClothReplacementPayload>(
            std::move(replacementDescriptors.replacementPayload));
    if (!payload || !payload->resourceSet ||
        !payload->cacheReplacement.replacementPackage)
    {
        return;
    }

    RendererResourceCache::GetInstance()
        .CommitPreparedClothResourceReplacement(
            std::move(payload->cacheReplacement));
    activePipeline = std::move(replacementPipeline);
    activeArtifact = std::move(committedArtifact);
    activeResourceSet = std::move(payload->resourceSet);
    descriptorRefreshRequired.store(true, std::memory_order_release);
}

} // namespace VL