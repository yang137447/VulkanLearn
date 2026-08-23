#include "render/eye/eyeComputeReloadParticipant.h"

#include <cstdlib>
#include <iostream>
#include <stdexcept>
#include <utility>
#include <vector>

#include "commonFunction.h"
#include "pipeline/computePipeline.h"
#include "pipeline/pipelineFactory.h"
#include "render/backend/rendererBackendVulkan.h"
#include "render/eye/eyeAssets.h"
#include "render/eye/eyeLookupTableGenerator.h"
#include "render/eye/eyeResourceLoader.h"
#include "render/eye/eyeResourceSet.h"
#include "render/resource/resourceRetireQueue.h"
#include "render/resource/rendererResourceCache.h"

namespace VL
{
namespace
{

struct EyeReplacementPayload
{
    RendererResourceCache::PreparedEyeResourceReplacement cacheReplacement;
    std::shared_ptr<EyeResourceSet> resourceSet;
};

bool ShouldReadbackEyeLut() noexcept
{
    const char* value = std::getenv("VULKANLEARN_EYE_LUT_READBACK");
    return value != nullptr && std::string(value) == "1";
}

void ValidateEyeLutReadback(const EyeLutReadbackReport& report)
{
    if (!report.allFinite ||
        report.maxAbsoluteGainError > 0.01f ||
        report.maxAbsoluteTransmissionError > 0.01f ||
        report.maxAbsoluteCoverageError > 0.01f ||
        report.maxAbsoluteJacobianError > 0.01f ||
        report.normalizationError > 0.01f)
    {
        throw std::runtime_error(
            "Eye LUT reload candidate failed GPU readback tolerance: maxGain=" +
            std::to_string(report.maxAbsoluteGainError) +
            ", normalization=" +
            std::to_string(report.normalizationError));
    }
}

} // namespace

void EyeComputeReloadParticipant::Initialize(
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
            "generator/eyeCausticLut",
            &artifact);
    if (!pipeline || artifact.artifactGenerationKey.empty())
    {
        throw std::runtime_error(
            "Eye Compute reload participant received an invalid active pipeline");
    }

    this->pipelineFactory = &pipelineFactory;
    this->rendererBackend = &rendererBackend;
    activePipeline = std::move(pipeline);
    activeArtifact = std::move(artifact);
    activeResourceSet.reset();
    descriptorRefreshRequired = false;
    initialized = true;
}

void EyeComputeReloadParticipant::Shutdown() noexcept
{
    activeResourceSet.reset();
    activePipeline.reset();
    activeArtifact = {};
    rendererBackend = nullptr;
    pipelineFactory = nullptr;
    descriptorRefreshRequired = false;
    initialized = false;
}

std::string EyeComputeReloadParticipant::GetShaderName() const
{
    return "generator/eyeCausticLut";
}

std::shared_ptr<ComputePipeline>
EyeComputeReloadParticipant::GetActivePipeline() const
{
    return activePipeline;
}

const ComputeShaderArtifact&
EyeComputeReloadParticipant::GetActiveArtifact() const
{
    return activeArtifact;
}

ComputeDescriptorReplacement
EyeComputeReloadParticipant::PrepareReplacementDescriptors(
    const ComputeShaderArtifact& candidate,
    const std::shared_ptr<ComputePipeline>& replacementPipeline) const
{
    if (!initialized || rendererBackend == nullptr ||
        !replacementPipeline || candidate.artifactGenerationKey.empty())
    {
        throw std::runtime_error(
            "Eye Compute reload candidate is missing initialized owner state");
    }

    const RendererResourceCache::ImmutableWorldLocalResourceRefs activePackage =
        RendererResourceCache::GetInstance()
            .CaptureActiveWorldLocalResources();
    if (!activePackage || !activePackage->eyeResources ||
        !activePackage->eyeResources->causticLutTexture)
    {
        throw std::runtime_error(
            "Eye Compute reload requires an active World-local Eye resource package");
    }

    std::vector<EyeProfileAsset> profiles =
        activePackage->eyeResources->profiles;
    const std::string sourceDigest = BuildEyeResourceSourceDigest(
        CommonFunction::GetResourcePath(),
        profiles,
        candidate.artifactGenerationKey);
    EyeComputePipelineCandidate pipelineCandidate;
    pipelineCandidate.pipeline = replacementPipeline;
    pipelineCandidate.artifactGenerationKey = candidate.artifactGenerationKey;

    EyeLutReadbackReport readbackReport;
    EyeLutReadbackReport* readbackReportPointer =
        ShouldReadbackEyeLut() ? &readbackReport : nullptr;
    std::shared_ptr<EyeResourceSet> resourceSet = BuildEyeResourceSet(
        *rendererBackend,
        std::move(profiles),
        sourceDigest,
        std::move(pipelineCandidate),
        readbackReportPointer);
    if (readbackReportPointer != nullptr)
    {
        ValidateEyeLutReadback(readbackReport);
        std::clog
            << "Eye LUT reload readback: samples="
            << readbackReport.sampleCount
            << ", maxGainError="
            << readbackReport.maxAbsoluteGainError
            << '\n';
    }

    EyeReplacementPayload payload;
    payload.resourceSet = resourceSet;
    payload.cacheReplacement =
        RendererResourceCache::GetInstance()
            .PrepareEyeResourceReplacement(
                std::shared_ptr<const EyeResourceSet>(resourceSet),
                resourceSet->causticLutTexture);

    // 旧 package 由同一个 prepared retirement 持有；它会和旧 pipeline
    // 在相同 lastUsedEpoch 后释放，避免 descriptor set 仍采样旧 LUT。
    const RendererResourceCache::ImmutableWorldLocalResourceRefs oldPackage =
        payload.cacheReplacement.previousPackage;
    ComputeDescriptorReplacement replacement;
    replacement.minimumRetirementEpoch =
        ResourceRetireQueue::GetInstance().GetLastSubmittedEpoch() + 1;

    replacement.replacementPayload =
        std::static_pointer_cast<void>(
            std::make_shared<EyeReplacementPayload>(std::move(payload)));
    replacement.retirement = MakePreparedRetiredResourcePackage(
        [oldPackage]() mutable
        {
            std::shared_ptr<const RendererResourceCache::WorldLocalResourcePackage>
                retiredPackage = oldPackage;
            retiredPackage.reset();
        });
    return replacement;
}

void EyeComputeReloadParticipant::CommitReplacement(
    ComputeShaderArtifact committedArtifact,
    std::shared_ptr<ComputePipeline> replacementPipeline,
    ComputeDescriptorReplacement&& replacementDescriptors) noexcept
{
    std::shared_ptr<EyeReplacementPayload> payload =
        std::static_pointer_cast<EyeReplacementPayload>(
            std::move(replacementDescriptors.replacementPayload));
    if (!payload || !payload->resourceSet ||
        !payload->cacheReplacement.replacementPackage)
    {
        return;
    }

    RendererResourceCache::GetInstance()
        .CommitPreparedEyeResourceReplacement(
            std::move(payload->cacheReplacement));
    activePipeline = std::move(replacementPipeline);
    activeArtifact = std::move(committedArtifact);
    activeResourceSet = std::move(payload->resourceSet);
    descriptorRefreshRequired.store(true, std::memory_order_release);
}

} // namespace VL