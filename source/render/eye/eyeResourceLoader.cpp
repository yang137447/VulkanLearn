#include "render/eye/eyeResourceLoader.h"

#include <filesystem>
#include <cstdlib>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>

#include <nlohmann/json.hpp>

#include "commonFunction.h"
#include "pipeline/pipelineFactory.h"
#include "render/backend/rendererBackendVulkan.h"
#include "render/eye/eyeAssets.h"
#include "render/eye/eyeComputeReloadParticipant.h"
#include "render/eye/eyeLookupTableGenerator.h"
#include "render/eye/eyeResourceSet.h"
#include "render/resource/rendererResourceCache.h"
#include "render/resource/rendererResourceLoadContext.h"
#include "shader/build/contentHash.h"

namespace VL
{

std::string BuildEyeResourceSourceDigest(
    const std::filesystem::path& resourceRoot,
    const std::vector<EyeProfileAsset>& profiles,
    std::string_view artifactGenerationKey)
{
    CanonicalFieldHasher hasher("vulkanlearn.eye-resource-set.v1");
    hasher.AddUInt32("profileSchemaVersion", EyeProfileSchemaVersion);
    hasher.AddUInt32("profileVersion", EyeProfileVersion);
    hasher.AddUInt32("lutVersion", EyeCausticLutVersion);
    hasher.AddUInt32("kernelVersion", EyeCausticLutKernelVersion);
    hasher.AddUInt32("lutWidth", EyeCausticLutWidth);
    hasher.AddUInt32("lutHeight", EyeCausticLutHeight);
    hasher.AddUInt32("elevationSlices", EyeCausticLutElevationSliceCount);
    hasher.AddUInt32("maximumProfileId", EyeCausticLutMaximumProfileId);
    hasher.AddString("computeArtifactGenerationKey", artifactGenerationKey);
    hasher.AddDigest(
        "generatorShader",
        ContentHasher::HashFile(CommonFunction::Path(
            "shader/glsl/generator/eyeCausticLut.comp")));
    hasher.AddUInt32(
        "profileCount",
        static_cast<uint32_t>(profiles.size()));

    for (size_t index = 0; index < profiles.size(); ++index)
    {
        const EyeProfileAsset& asset = profiles[index];
        const std::string prefix = "profile." + std::to_string(index) + ".";
        hasher.AddString(prefix + "path", asset.assetPath);
        hasher.AddDigest(
            prefix + "digest",
            ContentHasher::HashFile(resourceRoot / asset.assetPath));
        hasher.AddUInt32(prefix + "profileId", asset.profileId);
        hasher.AddUInt32(
            prefix + "lodSchemaVersion",
            asset.lodContract.schemaVersion);
        hasher.AddUInt32(
            prefix + "lodUvHandedness",
            static_cast<uint32_t>(asset.lodContract.uvHandedness + 1));
        hasher.AddUInt32(
            prefix + "lodTierCount",
            static_cast<uint32_t>(asset.lodContract.tiers.size()));
        for (size_t tierIndex = 0;
             tierIndex < asset.lodContract.tiers.size();
             ++tierIndex)
        {
            const EyeLodTierContract& tier = asset.lodContract.tiers[tierIndex];
            const std::string tierPrefix =
                prefix + "lod." + std::to_string(tierIndex) + ".";
            hasher.AddUInt32(
                tierPrefix + "tier",
                static_cast<uint32_t>(tier.tier));
            hasher.AddString(
                tierPrefix + "minimumScreenRadius",
                std::to_string(tier.minimumScreenRadius));
            hasher.AddString(
                tierPrefix + "irisDistanceScale",
                std::to_string(tier.irisDistanceScale));
            hasher.AddString(
                tierPrefix + "irisRadiusScale",
                std::to_string(tier.irisRadiusScale));
        }
        hasher.AddString(prefix + "sourceIdentity", asset.sourceIdentity);
    }
    return hasher.Finalize().ToHex();
}

std::shared_ptr<EyeResourceSet> BuildEyeResourceSet(
    RendererBackendVulkan& rendererBackend,
    std::vector<EyeProfileAsset> profiles,
    std::string sourceDigest,
    EyeComputePipelineCandidate pipelineCandidate,
    EyeLutReadbackReport* readbackReport)
{
    std::shared_ptr<EyeResourceSet> resourceSet =
        std::make_shared<EyeResourceSet>();
    resourceSet->sourceDigest = std::move(sourceDigest);
    resourceSet->computeArtifactGenerationKey =
        pipelineCandidate.artifactGenerationKey;
    resourceSet->profiles = std::move(profiles);
    resourceSet->lutMetadata.sourceDigest = resourceSet->sourceDigest;
    resourceSet->lutMetadata.computeArtifactGenerationKey =
        resourceSet->computeArtifactGenerationKey;

    for (size_t index = 0; index < resourceSet->profiles.size(); ++index)
    {
        const EyeProfileAsset& asset = resourceSet->profiles[index];
        if (!resourceSet->profileIdsByAssetPath.emplace(
                asset.assetPath,
                asset.profileId).second ||
            !resourceSet->profileIndicesById.emplace(
                asset.profileId,
                index).second)
        {
            throw std::runtime_error(
                "Duplicate Eye profile path or profileId: " + asset.assetPath);
        }
    }

    if (pipelineCandidate.artifactGenerationKey !=
        resourceSet->computeArtifactGenerationKey)
    {
        throw std::runtime_error(
            "Eye Compute artifact identity changed during resource preparation");
    }
    resourceSet->causticLutTexture = GenerateEyeCausticLutTexture(
        rendererBackend,
        pipelineCandidate,
        resourceSet->profiles,
        resourceSet->sourceDigest,
        readbackReport);
    return resourceSet;
}

EyeResourceLoader::EyeResourceLoader(
    PipelineFactory& pipelineFactory,
    RendererBackendVulkan& rendererBackend,
    RendererResourceLoadContext& loadContext,
    EyeComputeReloadParticipant* reloadParticipant)
    : pipelineFactory(pipelineFactory)
    , rendererBackend(rendererBackend)
    , loadContext(loadContext)
    , reloadParticipant(reloadParticipant)
{
}

void EyeResourceLoader::Load() const
{
    const std::filesystem::path resourceRoot =
        CommonFunction::GetResourcePath();
    std::vector<EyeProfileAsset> profiles =
        LoadEyeProfileAssets(resourceRoot);

    EyeLutReadbackReport readbackReport;
    EyeLutReadbackReport* readbackReportPointer = nullptr;
    const char* readbackEnvironment = std::getenv(
        "VULKANLEARN_EYE_LUT_READBACK");
    if (readbackEnvironment != nullptr &&
        std::string(readbackEnvironment) == "1")
    {
        readbackReportPointer = &readbackReport;
    }

    // World candidate 复用长期存活的 active pipeline，避免每次 World reload
    // 都创建一条未注册的临时 pipeline；独立热重载仍在 coordinator 的 GT
    // prepare/commit 边界中替换它。
    EyeComputePipelineCandidate pipelineCandidate;
    if (reloadParticipant != nullptr)
    {
        pipelineCandidate.pipeline = reloadParticipant->GetActivePipeline();
        pipelineCandidate.artifactGenerationKey =
            reloadParticipant->GetActiveArtifact().artifactGenerationKey;
        if (!pipelineCandidate.pipeline ||
            pipelineCandidate.artifactGenerationKey.empty())
        {
            throw std::runtime_error(
                "Eye reload participant is not initialized before World resource loading");
        }
    }
    else
    {
        pipelineCandidate = CreateEyeComputePipelineCandidate(pipelineFactory);
    }
    const std::string sourceDigest = BuildEyeResourceSourceDigest(
            resourceRoot,
            profiles,
            pipelineCandidate.artifactGenerationKey);

    if (loadContext.previousWorldResources &&
        loadContext.previousWorldResources->eyeResources &&
        loadContext.previousWorldResources->eyeResources->sourceDigest == sourceDigest &&
        loadContext.previousWorldResources->eyeResources->causticLutTexture)
    {
        loadContext.resourceCache.BindEyeResources(
            loadContext.previousWorldResources->eyeResources);
        loadContext.resourceCache.BindWorldTexture(
            "eyeCausticLut",
            loadContext.previousWorldResources->eyeResources->causticLutTexture);
        return;
    }

    std::shared_ptr<EyeResourceSet> resourceSet =
        BuildEyeResourceSet(
            rendererBackend,
            std::move(profiles),
            sourceDigest,
            std::move(pipelineCandidate),
            readbackReportPointer);
    if (readbackReportPointer != nullptr)
    {
        if (!readbackReport.allFinite ||
            readbackReport.maxAbsoluteGainError > 0.01f ||
            readbackReport.maxAbsoluteTransmissionError > 0.01f ||
            readbackReport.maxAbsoluteCoverageError > 0.01f ||
            readbackReport.maxAbsoluteJacobianError > 0.01f ||
            readbackReport.normalizationError > 0.01f)
        {
            throw std::runtime_error(
                "Eye LUT GPU readback failed CPU reference tolerance: maxGain=" +
                std::to_string(readbackReport.maxAbsoluteGainError) +
                ", meanGain=" +
                std::to_string(readbackReport.meanAbsoluteGainError) +
                ", maxTransmission=" +
                std::to_string(readbackReport.maxAbsoluteTransmissionError) +
                ", maxCoverage=" +
                std::to_string(readbackReport.maxAbsoluteCoverageError) +
                ", maxJacobian=" +
                std::to_string(readbackReport.maxAbsoluteJacobianError) +
                ", normalization=" +
                std::to_string(readbackReport.normalizationError));
        }
        std::clog
            << "Eye LUT readback: samples=" << readbackReport.sampleCount
            << ", valid=" << readbackReport.validDomainSampleCount
            << ", maxGainError=" << readbackReport.maxAbsoluteGainError
            << ", meanGainError=" << readbackReport.meanAbsoluteGainError
            << ", normalizationError=" << readbackReport.normalizationError
            << '\n';
    }
    const nlohmann::json metadataJson =
        SerializeEyeCausticLutMetadata(resourceSet->lutMetadata);
    const std::string metadataBytes = metadataJson.dump(4) + "\n";
    loadContext.QueueGeneratedFile(
        resourceRoot / "generated" / "eyeCausticLut.json",
        std::vector<uint8_t>(metadataBytes.begin(), metadataBytes.end()));

    loadContext.resourceCache.BindWorldTexture(
        "eyeCausticLut",
        resourceSet->causticLutTexture);
    loadContext.resourceCache.BindEyeResources(std::move(resourceSet));
}

} // namespace VL
