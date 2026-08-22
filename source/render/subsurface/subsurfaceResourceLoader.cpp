#include "render/subsurface/subsurfaceResourceLoader.h"

#include <filesystem>
#include <memory>
#include <stdexcept>
#include <string>

#include "commonFunction.h"
#include "pipeline/pipelineFactory.h"
#include "pipeline/subsurfaceLookupTableGenerator.h"
#include "render/backend/rendererBackendVulkan.h"
#include "render/resource/rendererResourceCache.h"
#include "render/resource/rendererResourceLoadContext.h"
#include "render/subsurface/subsurfaceAssets.h"
#include "render/subsurface/subsurfaceResourceSet.h"
#include "shader/build/contentHash.h"

namespace VL
{
namespace
{

std::string BuildSourceDigest(
    const std::filesystem::path& resourceRoot,
    const std::vector<SubsurfaceProfileAsset>& profiles,
    const std::vector<PreintegratedSkinLutAsset>& skinLuts)
{
    // generator shader 也是 lookup 数值定义的一部分；修改 GLSL 必须触发资源重建。
    CanonicalFieldHasher hasher(
        "vulkanlearn.subsurface-resource-set.v1");
    hasher.AddUInt32(
        "profileSchemaVersion",
        SubsurfaceProfileSchemaVersion);
    hasher.AddUInt32(
        "profileKernelVersion",
        SubsurfaceProfileKernelVersion);
    hasher.AddUInt32(
        "skinSchemaVersion",
        PreintegratedSkinSchemaVersion);
    hasher.AddUInt32(
        "skinLutVersion",
        PreintegratedSkinLutVersion);
    hasher.AddDigest(
        "generatorShader",
        ContentHasher::HashFile(CommonFunction::Path(
            "shader/glsl/generator/subsurfaceLookupTables.comp")));
    hasher.AddUInt32(
        "profileCount",
        static_cast<uint32_t>(profiles.size()));
    hasher.AddUInt32(
        "skinLutCount",
        static_cast<uint32_t>(skinLuts.size()));

    for (size_t index = 0; index < profiles.size(); ++index)
    {
        const SubsurfaceProfileAsset& asset = profiles[index];
        const std::string prefix =
            "profile." + std::to_string(index) + ".";
        hasher.AddString(prefix + "path", asset.assetPath);
        hasher.AddDigest(
            prefix + "digest",
            ContentHasher::HashFile(
                resourceRoot / asset.assetPath));
    }
    for (size_t index = 0; index < skinLuts.size(); ++index)
    {
        const PreintegratedSkinLutAsset& asset = skinLuts[index];
        const std::string prefix =
            "skinLut." + std::to_string(index) + ".";
        hasher.AddString(prefix + "path", asset.assetPath);
        hasher.AddDigest(
            prefix + "digest",
            ContentHasher::HashFile(
                resourceRoot / asset.assetPath));
    }
    return hasher.Finalize().ToHex();
}

std::shared_ptr<SubsurfaceResourceSet> BuildResourceSet(
    PipelineFactory& pipelineFactory,
    RendererBackendVulkan& rendererBackend,
    std::vector<SubsurfaceProfileAsset> profileAssets,
    std::vector<PreintegratedSkinLutAsset> skinLutAssets,
    std::string sourceDigest)
{
    std::shared_ptr<SubsurfaceResourceSet> resourceSet =
        std::make_shared<SubsurfaceResourceSet>();
    resourceSet->sourceDigest = std::move(sourceDigest);

    // path 与稳定 ID 都必须唯一，否则材质解析结果会随枚举顺序变化。
    resourceSet->profiles = std::move(profileAssets);
    for (size_t index = 0;
         index < resourceSet->profiles.size();
         ++index)
    {
        const SubsurfaceProfileAsset& asset =
            resourceSet->profiles[index];
        if (!resourceSet->profileIdsByAssetPath.emplace(
                asset.assetPath,
                asset.profileId).second ||
            !resourceSet->profileIndicesById.emplace(
                asset.profileId,
                index).second)
        {
            throw std::runtime_error(
                "Duplicate subsurface profile path or profileId: " +
                asset.assetPath);
        }
    }

    resourceSet->skinLuts = std::move(skinLutAssets);
    for (size_t index = 0;
         index < resourceSet->skinLuts.size();
         ++index)
    {
        const PreintegratedSkinLutAsset& asset =
            resourceSet->skinLuts[index];
        if (!resourceSet->skinLutIdsByAssetPath.emplace(
                asset.assetPath,
                asset.skinLutId).second ||
            !resourceSet->skinLutIndicesById.emplace(
                asset.skinLutId,
                index).second)
        {
            throw std::runtime_error(
                "Duplicate preintegrated skin path or skinLutId: " +
                asset.assetPath);
        }
    }

    SubsurfaceLookupTableGenerationResult generatedTables =
        SubsurfaceLookupTableGenerator::Generate(
            pipelineFactory,
            rendererBackend,
            resourceSet->profiles,
            resourceSet->skinLuts,
            resourceSet->sourceDigest);
    resourceSet->profileTableTexture =
        std::move(generatedTables.profileTableTexture);
    resourceSet->skinLutTableTexture =
        std::move(generatedTables.skinLutTableTexture);
    return resourceSet;
}

} // namespace

SubsurfaceResourceLoader::SubsurfaceResourceLoader(
    PipelineFactory& pipelineFactory,
    RendererBackendVulkan& rendererBackend,
    RendererResourceLoadContext& loadContext)
    : pipelineFactory(pipelineFactory)
    , rendererBackend(rendererBackend)
    , loadContext(loadContext)
{
}

void SubsurfaceResourceLoader::Load() const
{
    const std::filesystem::path resourceRoot =
        CommonFunction::GetResourcePath();
    std::vector<SubsurfaceProfileAsset> profileAssets =
        LoadSubsurfaceProfileAssets(resourceRoot);
    std::vector<PreintegratedSkinLutAsset> skinLutAssets =
        LoadPreintegratedSkinLutAssets(resourceRoot);
    const std::string sourceDigest = BuildSourceDigest(
        resourceRoot,
        profileAssets,
        skinLutAssets);

    // source digest 相同则复用上一 generation 的不可变 GPU 资源，
    // 避免仅场景对象变化时重复执行 Compute 生成。
    if (loadContext.previousWorldResources &&
        loadContext.previousWorldResources->subsurfaceResources &&
        loadContext.previousWorldResources
                ->subsurfaceResources->sourceDigest == sourceDigest)
    {
        loadContext.resourceCache.BindSubsurfaceResources(
            loadContext.previousWorldResources
                ->subsurfaceResources);
        return;
    }

    // 新资源只绑定到候选 resource cache，最终由 World/Graph transaction 发布。
    loadContext.resourceCache.BindSubsurfaceResources(
        BuildResourceSet(
            pipelineFactory,
            rendererBackend,
            std::move(profileAssets),
            std::move(skinLutAssets),
            sourceDigest));
}

} // namespace VL
