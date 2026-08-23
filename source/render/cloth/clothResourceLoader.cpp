#include "render/cloth/clothResourceLoader.h"
#include "render/cloth/clothComputeReloadParticipant.h"

#include <memory>
#include <stdexcept>
#include <string>

#include "commonFunction.h"
#include "pipeline/clothLookupTableGenerator.h"
#include "pipeline/computePipeline.h"
#include "pipeline/pipelineFactory.h"
#include "render/backend/rendererBackendVulkan.h"
#include "render/cloth/clothAssets.h"
#include "render/cloth/clothResourceSet.h"
#include "render/resource/rendererResourceCache.h"
#include "render/resource/rendererResourceLoadContext.h"
#include "shader/build/contentHash.h"
#include "shader/reload/computeShaderArtifact.h"

namespace VL
{

std::string BuildClothResourceSourceDigest(
    std::string_view artifactGenerationKey)
{
    CanonicalFieldHasher hasher("vulkanlearn.cloth-resource-set.v1");
    hasher.AddUInt32("clothModelVersion", ClothModelVersion);
    hasher.AddUInt32(
        "sheenRoughnessMappingVersion",
        ClothSheenRoughnessMappingVersion);
    hasher.AddUInt32(
        "charlieDistributionVersion",
        ClothCharlieDistributionVersion);
    hasher.AddUInt32(
        "neubeltVisibilityVersion",
        ClothNeubeltVisibilityVersion);
    hasher.AddUInt32(
        "directionalAlbedoLutVersion",
        ClothDirectionalAlbedoLutVersion);
    hasher.AddUInt32("sheenIblVersion", ClothSheenIblVersion);
    hasher.AddUInt32("lutWidth", ClothDirectionalAlbedoLutWidth);
    hasher.AddUInt32("lutHeight", ClothDirectionalAlbedoLutHeight);
    hasher.AddString("artifactGenerationKey", artifactGenerationKey);
    hasher.AddDigest(
        "generatorShader",
        ContentHasher::HashFile(CommonFunction::Path(
            "shader/glsl/generator/clothLookupTables.comp")));
    return hasher.Finalize().ToHex();
}

std::shared_ptr<ClothResourceSet> BuildClothResourceSet(
    RendererBackendVulkan& rendererBackend,
    std::shared_ptr<ComputePipeline> computePipeline,
    std::string sourceDigest)
{
    if (!computePipeline)
    {
        throw std::runtime_error(
            "Cloth resource set requires an initialized Compute pipeline");
    }

    std::shared_ptr<ClothResourceSet> resourceSet =
        std::make_shared<ClothResourceSet>();
    resourceSet->sourceDigest = std::move(sourceDigest);
    ClothLookupTableGenerationResult generated =
        ClothLookupTableGenerator::GenerateWithPipeline(
            computePipeline,
            rendererBackend,
            resourceSet->sourceDigest);
    resourceSet->directionalAlbedoLutTexture =
        std::move(generated.directionalAlbedoLutTexture);
    if (!resourceSet->directionalAlbedoLutTexture)
    {
        throw std::runtime_error(
            "Cloth lookup generation returned an empty directional albedo LUT");
    }
    return resourceSet;
}

ClothResourceLoader::ClothResourceLoader(
    PipelineFactory& pipelineFactory,
    RendererBackendVulkan& rendererBackend,
    RendererResourceLoadContext& loadContext,
    ClothComputeReloadParticipant* reloadParticipant)
    : pipelineFactory(pipelineFactory)
    , rendererBackend(rendererBackend)
    , loadContext(loadContext)
    , reloadParticipant(reloadParticipant)
{
}

void ClothResourceLoader::Load() const
{
    std::shared_ptr<ComputePipeline> computePipeline;
    std::string artifactGenerationKey;
    if (reloadParticipant != nullptr)
    {
        computePipeline = reloadParticipant->GetActivePipeline();
        artifactGenerationKey =
            reloadParticipant->GetActiveArtifact().artifactGenerationKey;
        if (!computePipeline || artifactGenerationKey.empty())
        {
            throw std::runtime_error(
                "Cloth Compute reload participant is not initialized before World resource loading");
        }
    }
    else
    {
        ComputeShaderArtifact artifact;
        computePipeline = pipelineFactory.CreateComputePipeline(
            "generator/clothLookupTables",
            &artifact);
        artifactGenerationKey = artifact.artifactGenerationKey;
    }

    const std::string sourceDigest = BuildClothResourceSourceDigest(
        artifactGenerationKey);
    if (loadContext.previousWorldResources &&
        loadContext.previousWorldResources->clothResources &&
        loadContext.previousWorldResources->clothResources->sourceDigest ==
            sourceDigest &&
        loadContext.previousWorldResources->clothResources
            ->directionalAlbedoLutTexture)
    {
        loadContext.resourceCache.BindClothResources(
            loadContext.previousWorldResources->clothResources);
        loadContext.resourceCache.BindWorldTexture(
            "clothDirectionalAlbedoLut",
            loadContext.previousWorldResources->clothResources
                ->directionalAlbedoLutTexture);
        return;
    }

    std::shared_ptr<ClothResourceSet> resourceSet =
        BuildClothResourceSet(
            rendererBackend,
            std::move(computePipeline),
            sourceDigest);
    loadContext.resourceCache.BindWorldTexture(
        "clothDirectionalAlbedoLut",
        resourceSet->directionalAlbedoLutTexture);
    loadContext.resourceCache.BindClothResources(std::move(resourceSet));
}

} // namespace VL