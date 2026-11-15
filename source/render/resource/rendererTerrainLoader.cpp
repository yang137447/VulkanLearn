#include "render/resource/rendererTerrainLoader.h"

#include <cmath>
#include <cstring>
#include <iostream>
#include <memory>
#include <set>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <utility>

#include "commonFunction.h"
#include "material.h"
#include "materialInstance.h"
#include "pipeline/pipelineFactory.h"
#include "shaderVariant.h"
#include "render/backend/rendererBackendVulkan.h"
#include "render/resource/rendererMaterialLoader.h"
#include "render/resource/rendererResourceCache.h"
#include "render/resource/rendererResourceLoadContext.h"
#include "render/resource/terrain/terrainAtlasGpu.h"
#include "render/resource/terrain/terrainLayerPageUpload.h"
#include "render/resource/terrain/terrainPageUpload.h"
#include "render/resource/terrain/terrainSourceSets.h"
#include "render/resource/terrain/terrainUploadRings.h"
#include "renderableObject.h"
#include "terrain/terrainPageManager.h"
#include "terrain/terrainLayerTable.h"
#include "terrain/terrainLayerVT.h"
#include "terrain/terrainMeshBuilder.h"
#include "terrain/terrainWeightMap.h"
#include "texture.h"

namespace VL
{
Eigen::Matrix4f BuildModelMatrix(
    const Eigen::Vector3f& position,
    const Eigen::Vector3f& rotation,
    const Eigen::Vector3f& scale)
{
    Eigen::Matrix4f scaleMatrix;
    scaleMatrix <<
        scale.x(), 0.0f, 0.0f, 0.0f,
        0.0f, scale.y(), 0.0f, 0.0f,
        0.0f, 0.0f, scale.z(), 0.0f,
        0.0f, 0.0f, 0.0f, 1.0f;

    Eigen::Matrix4f translationMatrix;
    translationMatrix <<
        1.0f, 0.0f, 0.0f, position.x(),
        0.0f, 1.0f, 0.0f, position.y(),
        0.0f, 0.0f, 1.0f, position.z(),
        0.0f, 0.0f, 0.0f, 1.0f;

    return translationMatrix *
        CommonFunction::RotationToMatrix(rotation) *
        scaleMatrix;
}

RendererTerrainLoader::RendererTerrainLoader(
    PipelineFactory& pipelineFactory,
    RendererBackendVulkan& rendererBackend,
    RendererResourceLoadContext& loadContext,
    TerrainStreamingState& streamingState,
    const TerrainChunkResidencyDesc& chunkResidency)
    : pipelineFactory(pipelineFactory)
    , rendererBackend(rendererBackend)
    , loadContext(loadContext)
    , streamingState(streamingState)
    , chunkResidency(chunkResidency)
{
}

RendererTerrainLoadResult RendererTerrainLoader::LoadTerrainObject(
    const nlohmann::json& node,
    const TerrainBuildPlan& plan) const
{
    if (loadContext.offlineOnly)
        throw std::runtime_error("Terrain requires realtime rendering; offline scenes do not support it");

    const Eigen::Vector3f position =
        JsonParser::ParseValue<Eigen::Vector3f>(node.at("position"));
    const Eigen::Vector3f rotation =
        JsonParser::ParseValue<Eigen::Vector3f>(node.at("rotation"));
    const Eigen::Vector3f scale =
        JsonParser::ParseValue<Eigen::Vector3f>(node.at("scale"));
    const std::string objectName = node.at("name").get<std::string>();
    const Eigen::Matrix4f model = BuildModelMatrix(position, rotation, scale);

    RendererMaterialLoader materialLoader(
        pipelineFactory,
        rendererBackend,
        loadContext);
    const std::shared_ptr<MaterialInstance> materialInstance =
        materialLoader.LoadSceneMaterialInstance(plan.desc.materialInstancePath);
    if (!materialInstance)
        throw std::runtime_error(
            "Terrain material instance was not created: " + plan.desc.materialInstancePath);
    const std::string materialKey =
        materialInstance->GetBaseMaterial().lock()->GetMaterialKey();
    // 多 LOD 地形只有 LOD0 允许细分：基础 MI 自己开启该宏时，LOD1..N 会
    // 复用同一条 PATCH_LIST pipeline，而粗 LOD 的语义是三角形拓扑。
    // 单 LOD（lodLevels==1）没有这个问题，全部 chunk 都是 LOD0。
    if (plan.desc.lodLevels > 1 &&
        HasMaterialMacroValue(
            materialInstance->GetBaseMaterial().lock()->GetShaderVariantKey().macros,
            "VL_TERRAIN_TESSELLATION",
            "1"))
    {
        throw std::runtime_error(
            "Terrain base material instance must not enable "
            "VL_TERRAIN_TESSELLATION for multi-LOD terrain: " +
            plan.desc.materialInstancePath);
    }
    // 可选细分 MI：失败必须硬错误，不能回退到基础 MI，否则近场 LOD0 会
    // 静默丢失 tessellation 材质。
    std::shared_ptr<MaterialInstance> detailMaterialInstance;
    std::string detailMaterialKey;
    if (!plan.desc.detailMaterialInstancePath.empty())
    {
        detailMaterialInstance =
            materialLoader.LoadSceneMaterialInstance(
                plan.desc.detailMaterialInstancePath);
        if (!detailMaterialInstance)
        {
            throw std::runtime_error(
                "Terrain detail material instance was not created: " +
                plan.desc.detailMaterialInstancePath);
        }
        if (detailMaterialInstance == materialInstance)
        {
            throw std::runtime_error(
                "Terrain detail material instance must differ from the base "
                "instance: " + plan.desc.detailMaterialInstancePath);
        }
        const std::shared_ptr<Material> detailBaseMaterial =
            detailMaterialInstance->GetBaseMaterial().lock();
        if (!detailBaseMaterial)
        {
            throw std::runtime_error(
                "Terrain detail material instance has no base material: " +
                plan.desc.detailMaterialInstancePath);
        }
        detailMaterialKey = detailBaseMaterial->GetMaterialKey();
    }

    // Phase A：M_terrain 通过生成的 8x2 LUT 读取 TR.layers。
    // LUT 是 world-local 生成资源，由 MI 持有；后续 VT/页面化会替换它的上传方式。
    const auto materialUsesTexture = [](
        const std::shared_ptr<MaterialInstance>& material,
        const char* textureName) {
        const std::shared_ptr<Material> baseMaterial =
            material ? material->GetBaseMaterial().lock() : nullptr;
        if (!baseMaterial)
            return false;
        for (const MaterialTextureSchemaEntry& texture :
             baseMaterial->GetMaterialDescriptorSchema().GetTextures())
        {
            if (texture.name == textureName)
                return true;
        }
        return false;
    };
    const bool usesLayerTable =
        materialUsesTexture(materialInstance, "layerTable");
    if (detailMaterialInstance &&
        materialUsesTexture(detailMaterialInstance, "layerTable") !=
            usesLayerTable)
    {
        throw std::runtime_error(
            "Terrain detail material instance must use the same layerTable "
            "material path as the base instance: " +
            plan.desc.detailMaterialInstancePath);
    }
    if (usesLayerTable)
    {
        if (!streamingState.layerResources.layerTable)
            throw std::runtime_error("Terrain layer table resource is not initialized");
        // 层表允许 256 层，但 GPU 数组的层数由资产决定；越界层号必须在这里失败。
        const std::shared_ptr<Texture> layerAlbedo = materialInstance->GetTexture("layerAlbedo");
        const std::shared_ptr<Texture> layerNormal = materialInstance->GetTexture("layerNormal");
        const std::shared_ptr<Texture> layerPbr = materialInstance->GetTexture("layerPbr");
        // layerHeight 是近场细分主位移的来源；detail MI 若漏绑会静默退化为
        // 0.5 占位数组（零位移），因此与三张颜色/法线数组一起校验并共享。
        const std::shared_ptr<Texture> layerHeight = materialInstance->GetTexture("layerHeight");
        if (!layerAlbedo || !layerNormal || !layerPbr || !layerHeight)
            throw std::runtime_error("Terrain layer arrays are not bound");
        const bool vtRequested =
            materialInstance->HasParameter("u_terrainLayerMode") &&
            materialInstance->GetParameter<float>("u_terrainLayerMode") > 0.5f;
        // 权重图是作者提供的单块地形覆盖层；detail MI 必须复用同一个
        // Texture 对象，否则 LOD0 与粗 LOD 会采样到不同掩码。
        const std::shared_ptr<Texture> weightMaps =
            materialInstance->GetTexture("layerWeightMaps");
        // mode 1 的逐层权重数组属于单块地形。按纹理身份记录使用它的摆放，
        // 等 World 全部地形加载完再判定是否被共享；共享时不能静默采同一份掩码。
        if (materialInstance->HasParameter("u_terrainWeightMapMode") &&
            materialInstance->GetParameter<float>("u_terrainWeightMapMode") > 0.5f)
        {
            if (!weightMaps)
            {
                throw std::runtime_error(
                    "Terrain u_terrainWeightMapMode=1 requires a bound "
                    "layerWeightMaps texture: " + plan.desc.materialInstancePath);
            }
            if (weightMaps->GetArrayLayers() !=
                static_cast<uint32_t>(plan.desc.layers.size()))
            {
                throw std::runtime_error(
                    "Terrain layerWeightMaps slice count " +
                    std::to_string(weightMaps->GetArrayLayers()) +
                    " does not match TR layer count " +
                    std::to_string(plan.desc.layers.size()) + ": " +
                    plan.desc.materialInstancePath);
            }
            if (plan.desc.heightmapMargin != 1)
            {
                throw std::runtime_error(
                    "Terrain u_terrainWeightMapMode=1 expects heightmapMargin=1 "
                    "(the weight-map UV formula reserves one margin texel): " +
                    plan.desc.materialInstancePath);
            }
            const std::string placementId = TerrainPlacementIdentity(plan);
            streamingState.weightMapBindings.Record(
                weightMaps.get(), plan.desc.materialInstancePath, placementId);
        }
        // S1/S2：按 layer.sourceSet 分组登记源数组；同一 set 共享同一组
        // shared_ptr，不同 set 允许不同数组但共享同一组 atlas。
        // mode 0 直接用 globalId 作为数组 slice，因此必须显式校验全局 ID
        // 落在该 MI 的数组层数内；否则会按 repeat 静默采到错误层。
        RegisterTerrainSourceSets(
            rendererBackend,
            streamingState,
            plan,
            layerAlbedo,
            layerNormal,
            layerPbr,
            vtRequested);
        // L4b：层页 feedback pass 只有一个 u_uvScale，必须与所有 layer-table
        // 地形的 M_terrain 实例一致，否则 feedback 会报告错误的层 UV/页号。
        const float uvScale = materialInstance->HasParameter("u_uvScale")
            ? materialInstance->GetParameter<float>("u_uvScale")
            : 64.0f;
        if (!streamingState.layerUvScaleInitialized)
        {
            streamingState.layerUvScale = uvScale;
            streamingState.layerUvScaleInitialized = true;
            // 单向来源：这是 World 内层 UV 的唯一真值。feedback pass 的
            // materialInstance 不参与判定，renderSystem 每帧把该值推给它。
            std::cout << "[TerrainLayer] u_uvScale=" << uvScale
                      << " resolved from M_terrain instance ("
                      << plan.desc.assetPath << ")" << std::endl;
        }
        else if (vtRequested &&
                 std::abs(streamingState.layerUvScale - uvScale) > 1.0e-4f)
        {
            throw std::runtime_error(
                "All layer-VT terrains in a World must share the same "
                "u_uvScale so layer feedback matches the material: " +
                plan.desc.assetPath);
        }
        if (vtRequested)
        {
            // 作者显式请求 VT 时不能静默退回 mode 0；源图与 catalog 不一致
            // 会让每个页号都采到错误内容，必须在加载期失败。
            bool allSourceResolutionsMatch = true;
            for (const auto& entry : streamingState.layerSources)
            {
                if (!entry.second.catalogResolutionMatches)
                    allSourceResolutionsMatch = false;
            }
            if (!allSourceResolutionsMatch)
            {
                throw std::runtime_error(
                    "Terrain layer VT requires sourceResolution to match the "
                    "bound layer texture extent: " + plan.desc.assetPath);
            }
            streamingState.layerVTRequested = true;
            // L4e：登记 mode 1 实例，console 强制 mip 时逐帧写入。
            const auto registerLayerVtMaterial = [&](
                const std::shared_ptr<MaterialInstance>& material) {
                if (!material)
                    return;
                for (const std::weak_ptr<MaterialInstance>& existing :
                     streamingState.layerVTMaterials)
                {
                    if (existing.lock() == material)
                        return;
                }
                streamingState.layerVTMaterials.push_back(material);
            };
            registerLayerVtMaterial(materialInstance);
            registerLayerVtMaterial(detailMaterialInstance);
        }
        // L2 的 atlas 依赖实际 MI 绑定到的层数组，因此只能在首个地形 MI
        // 载入后创建；创建后本函数立刻绑定，不能留到对象遍历结束再回填。
        if (!streamingState.layerResources.atlasesCreated)
        {
            CreateTerrainLayerAtlasGpuResources(
                rendererBackend,
                streamingState);
        }
        if (!streamingState.pageResources.pageTable ||
            !streamingState.pageResources.layerIds ||
            !streamingState.pageResources.layerWeights)
        {
            throw std::runtime_error("Terrain page resources are not initialized");
        }
        if (!streamingState.layerResources.albedoAtlas ||
            !streamingState.layerResources.normalAtlas ||
            !streamingState.layerResources.pbrAtlas)
        {
            throw std::runtime_error(
                "Terrain layer atlas resources are not initialized");
        }
        // 基础 MI 与 detail MI 必须绑定同一组 VT 纹理；运行期页表、atlas、
        // 层数组和权重图都只存在一份，不能各持一份副本。
        const auto applySharedTerrainTextures = [&](MaterialInstance& target) {
            target.SetTextureKeepingIdentity(
                "layerTable", streamingState.layerResources.layerTable);
            target.SetTextureKeepingIdentity(
                "pageTable", streamingState.pageResources.pageTable);
            target.SetTextureKeepingIdentity(
                "layerIds", streamingState.pageResources.layerIds);
            target.SetTextureKeepingIdentity(
                "layerWeights", streamingState.pageResources.layerWeights);
            target.SetTextureKeepingIdentity("layerAlbedo", layerAlbedo);
            target.SetTextureKeepingIdentity("layerNormal", layerNormal);
            target.SetTextureKeepingIdentity("layerPbr", layerPbr);
            target.SetTextureKeepingIdentity("layerHeight", layerHeight);
            if (weightMaps)
            {
                target.SetTextureKeepingIdentity(
                    "layerWeightMaps", weightMaps);
            }
            // L3：每 mip 一张页表 + 三张 atlas；mode 0 不使用，但 descriptor
            // 仍必须是合法纹理，因此统一绑定 L2 创建的共享资源。
            for (uint32_t mip = 0;
                 mip <= TerrainLayerPageManager::MaxMipLevel;
                 ++mip)
            {
                if (!streamingState.layerResources.pageTables[mip])
                {
                    throw std::runtime_error(
                        "Terrain layer page table is not initialized for mip " +
                        std::to_string(mip));
                }
                target.SetTexture(
                    "layerPageTable" + std::to_string(mip),
                    streamingState.layerResources.pageTables[mip]);
            }
            target.SetTexture(
                "layerAlbedoAtlas",
                streamingState.layerResources.albedoAtlas);
            target.SetTexture(
                "layerNormalAtlas",
                streamingState.layerResources.normalAtlas);
            target.SetTexture(
                "layerPbrAtlas",
                streamingState.layerResources.pbrAtlas);
        };
        applySharedTerrainTextures(*materialInstance);
        if (detailMaterialInstance)
            applySharedTerrainTextures(*detailMaterialInstance);

        // 固定页表/atlas 尺寸；两个 MI 必须使用同一份值。
        const auto applySharedTerrainParameters = [&](MaterialInstance& target) {
            if (target.HasParameter("u_pageTableSize"))
                target.SetParameter(
                    "u_pageTableSize",
                    static_cast<float>(TerrainPageManager::PageTableSize));
            if (target.HasParameter("u_pageResolution"))
                target.SetParameter(
                    "u_pageResolution",
                    static_cast<float>(TerrainPageManager::PageResolution));
            if (target.HasParameter("u_layerPageResolution"))
            {
                target.SetParameter(
                    "u_layerPageResolution",
                    static_cast<float>(
                        TerrainLayerPageManager::PageResolution));
            }
            if (target.HasParameter("u_layerAtlasResolution"))
            {
                target.SetParameter(
                    "u_layerAtlasResolution",
                    static_cast<float>(
                        TerrainPageManager::AtlasResolution));
            }
            if (target.HasParameter("u_layerPagesPerAtlasSlice"))
            {
                target.SetParameter(
                    "u_layerPagesPerAtlasSlice",
                    static_cast<float>(
                        TerrainLayerPageManager::PagesPerAtlasSlice));
            }
            if (target.HasParameter("u_layerTilesPerAtlasSide"))
            {
                target.SetParameter(
                    "u_layerTilesPerAtlasSide",
                    static_cast<float>(
                        TerrainLayerPageManager::TilesPerAtlasSide));
            }
            if (target.HasParameter("u_layerMaxMip"))
            {
                target.SetParameter(
                    "u_layerMaxMip",
                    static_cast<float>(
                        TerrainLayerPageManager::MaxMipLevel));
            }
        };
        applySharedTerrainParameters(*materialInstance);
        if (detailMaterialInstance)
        {
            // u_uvScale、layer mode、force mip、global 页映射等决定 VT 采样
            // 语义，必须从基础 MI 复制；detail MI 只保留自己的 tessellation
            // 视觉参数（detail amplitude / radius 等）。
            const auto copyFloatParameter = [&](const char* parameterName) {
                if (!materialInstance->HasParameter(parameterName))
                    return;
                if (!detailMaterialInstance->HasParameter(parameterName))
                {
                    throw std::runtime_error(
                        "Terrain detail MI is missing VT parameter '" +
                        std::string(parameterName) + "': " +
                        plan.desc.detailMaterialInstancePath);
                }
                detailMaterialInstance->SetParameter(
                    parameterName,
                    materialInstance->GetParameter<float>(parameterName));
            };
            for (const char* parameterName : {
                     "u_uvScale",
                     "u_terrainLayerMode",
                     "u_terrainWeightMapMode",
                     "u_layerForceMip",
                     "u_globalSizeCm",
                     "u_frameTag"})
            {
                copyFloatParameter(parameterName);
            }
            if (materialInstance->HasParameter("u_globalOriginCm"))
            {
                if (!detailMaterialInstance->HasParameter("u_globalOriginCm"))
                {
                    throw std::runtime_error(
                        "Terrain detail MI is missing VT parameter "
                        "'u_globalOriginCm': " +
                        plan.desc.detailMaterialInstancePath);
                }
                detailMaterialInstance->SetParameter(
                    "u_globalOriginCm",
                    materialInstance->GetParameter<Eigen::Vector2f>(
                        "u_globalOriginCm"));
            }
            applySharedTerrainParameters(*detailMaterialInstance);
        }
    }

    RendererResourceCache& resourceCache = loadContext.resourceCache;
    RendererTerrainLoadResult result;
    // 运行时 LOD：生成 0..lodLevels-1 级网格并注册为 WorldMeshObject；
    // 每帧由 WorldSnapshotBuilder 按相机距离只挑一级输出。第一版几何驻留
    // 在加载期生效：窗口外的细 LOD（0..N-2）不生成，最粗一级永远生成，
    // 因此运行时相机移动只会改变“选择/兜底”，不会再补传细网格。
    // lodLevels=1 保持历史路径（无边界缝合带、无 LOD 组、名字/mesh key 不变）。
    // TR 预检保证 1..4；这里不做静默钳制，越界让 BuildChunks 明确失败。
    const uint32_t lodLevelCount =
        plan.desc.lodLevels == 0 ? 1u : plan.desc.lodLevels;
    const bool multiLod = lodLevelCount > 1;
    const std::vector<float> lodDistancesCm =
        TerrainLodDistancesCm(plan.desc);
    const size_t chunksPerSide = plan.desc.chunkQuads == 0
        ? 0
        : plan.desc.gridResolution / plan.desc.chunkQuads;
    const std::vector<float> residencyDistancesCm =
        multiLod ? TerrainChunkResidencyDistancesCm(plan.desc)
                 : std::vector<float>();
    const bool residencyActive =
        multiLod && chunkResidency.cameraKnown &&
        chunksPerSide != 0;

    // 组的距离口径必须与运行时选择完全一致：用 LOD0 顶点范围（边界缝合带
    // 与地表同高，不会改变 AABB）。先算一遍不含法线的 AABB，再用它决定哪些
    // 细 LOD 进入驻留窗口；窗口外的 chunk 连顶点/法线都不生成。
    std::vector<TerrainChunkBounds> chunkBounds;
    if (multiLod)
        chunkBounds = TerrainMeshBuilder::ComputeChunkBounds(plan);

    TerrainChunkPlacementStats placementStats;
    placementStats.placementId = TerrainPlacementIdentity(plan);
    placementStats.assetPath = plan.desc.assetPath;
    placementStats.chunksPerSide = static_cast<uint32_t>(chunksPerSide);
    placementStats.lodLevelCount = lodLevelCount;
    placementStats.residencyEnabled = residencyActive;
    placementStats.residencyDistancesCm = residencyDistancesCm;
    placementStats.levels.resize(lodLevelCount);

    std::vector<TerrainChunkBuildFilter> levelFilters(lodLevelCount);
    const uint32_t totalChunks =
        static_cast<uint32_t>(chunksPerSide) *
        static_cast<uint32_t>(chunksPerSide);
    for (uint32_t lodLevel = 0; lodLevel < lodLevelCount; ++lodLevel)
    {
        TerrainChunkLevelStats& levelStats = placementStats.levels[lodLevel];
        levelStats.lodLevel = lodLevel;
        // LOD0 走历史网格；更粗的级别带一圈边界缝合带。
        levelStats.stitched = lodLevel > 0;
        TerrainChunkBuildFilter& filter = levelFilters[lodLevel];
        if (!residencyActive)
            continue;
        filter.chunkMask.assign(totalChunks, 0);
        for (const TerrainChunkBounds& bounds : chunkBounds)
        {
            const size_t maskIndex =
                static_cast<size_t>(bounds.chunkZ) * chunksPerSide +
                bounds.chunkX;
            if (maskIndex >= filter.chunkMask.size())
                continue;
            if (TerrainChunkLevelIsResident(
                    chunkResidency,
                    residencyDistancesCm,
                    model,
                    bounds.boundsMin,
                    bounds.boundsMax,
                    lodLevel,
                    lodLevelCount))
            {
                filter.chunkMask[maskIndex] = 1;
            }
        }
    }

    uint64_t createdChunks = 0;
    for (const TerrainChunkBuildFilter& filter : levelFilters)
    {
        if (filter.Empty())
        {
            createdChunks += totalChunks;
            continue;
        }
        for (uint8_t accepted : filter.chunkMask)
            createdChunks += accepted != 0 ? 1u : 0u;
    }
    result.objectPlans.reserve(static_cast<size_t>(createdChunks));
    for (uint32_t lodLevel = 0; lodLevel < lodLevelCount; ++lodLevel)
    {
        const TerrainChunkBuildFilter* filter =
            residencyActive ? &levelFilters[lodLevel] : nullptr;
        const std::vector<TerrainMeshChunk> chunks =
            TerrainMeshBuilder::BuildChunks(
                plan,
                lodLevel,
                filter);
        TerrainChunkLevelStats& levelStats = placementStats.levels[lodLevel];
        levelStats.chunkCount = static_cast<uint32_t>(chunks.size());
        levelStats.skippedChunkCount =
            totalChunks - static_cast<uint32_t>(chunks.size());
        levelStats.skippedVertexCount =
            static_cast<uint64_t>(levelStats.skippedChunkCount) *
            TerrainChunkVertexCount(plan.desc, lodLevel, levelStats.stitched);
        levelStats.skippedIndexCount =
            static_cast<uint64_t>(levelStats.skippedChunkCount) *
            TerrainChunkIndexCount(plan.desc, lodLevel, levelStats.stitched);
        levelStats.skippedVertexBytes =
            levelStats.skippedVertexCount * sizeof(Vertex);
        levelStats.skippedIndexBytes =
            levelStats.skippedIndexCount * sizeof(uint32_t);
        for (const TerrainMeshChunk& chunk : chunks)
        {
            levelStats.vertexCount += chunk.vertices.size();
            levelStats.indexCount += chunk.indices.size();
            // key 包含 placementId 与 LOD：同一 TR 的多处摆放不能共享/覆盖
            // chunk 缓冲，同一 chunk 的不同级也必须是独立网格。
            const std::string chunkKey =
                TerrainChunkResourceKey(
                    plan,
                    chunk.chunkX,
                    chunk.chunkZ,
                    lodLevel);
            const std::shared_ptr<RenderableObject> cached =
                resourceCache.GetRenderableObject(chunkKey);
            if (cached == nullptr)
            {
                resourceCache.BindRenderableObject(
                    chunkKey,
                    std::make_shared<RenderableObject>(
                        chunk.vertices,
                        chunk.indices,
                        rendererBackend,
                        chunkKey,
                        false,
                        chunk.patchIndices));
            }

            MeshObjectBuildPlan objectPlan;
            objectPlan.objectName = TerrainChunkObjectName(
                objectName,
                chunk.chunkX,
                chunk.chunkZ,
                lodLevel,
                multiLod);
            objectPlan.sceneObjectIdentity = objectName;
            objectPlan.materialSlotIndex = 0;
            objectPlan.materialSlotName = "Terrain";
            objectPlan.debugName = objectPlan.objectName;
            objectPlan.model = model;
            objectPlan.scenePosition = position;
            objectPlan.sceneRotation = rotation;
            objectPlan.sceneScale = scale;
            objectPlan.localBoundsMin = chunk.boundsMin;
            objectPlan.localBoundsMax = chunk.boundsMax;
            objectPlan.meshKey = chunkKey;
            // 只有 LOD0 使用 detail MI；LOD>0 始终使用基础 MI，避免粗 LOD
            // 也进入 tessellation PATCH_LIST 管线。
            const bool useDetailMaterial =
                lodLevel == 0 && detailMaterialInstance != nullptr;
            objectPlan.materialKey = useDetailMaterial
                ? detailMaterialKey
                : materialKey;
            objectPlan.materialInstanceKey = useDetailMaterial
                ? detailMaterialInstance->GetName()
                : materialInstance->GetName();
            // feedback 只回读共享 VT 页表：没有 layers 的普通地形既没有虚拟页，
            // 也不该把自己的页坐标写进其他分层地形的 feedback。
            objectPlan.terrainFeedbackEligible =
                TerrainParticipatesInFeedback(plan, usesLayerTable);
            // 仅标记编辑器的地形身份；World 侧仍按 chunk mesh 构建和绘制。
            objectPlan.isTerrain = true;
            objectPlan.terrainAssetPath = plan.desc.assetPath;
            objectPlan.terrainPlacementId =
                TerrainPlacementIdentity(plan);
            if (multiLod)
            {
                objectPlan.lodGroupId =
                    TerrainPlacementIdentity(plan) + "|" +
                    std::to_string(chunk.chunkX) + "|" +
                    std::to_string(chunk.chunkZ);
                objectPlan.lodLevel = lodLevel;
                objectPlan.lodLevelCount = lodLevelCount;
                objectPlan.lodDistancesCm = lodDistancesCm;
                const size_t boundsIndex =
                    static_cast<size_t>(chunk.chunkZ) * chunksPerSide +
                    chunk.chunkX;
                if (boundsIndex >= chunkBounds.size())
                    throw std::runtime_error(
                        "Terrain LOD group bounds are missing for a chunk");
                objectPlan.lodGroupBoundsMin =
                    chunkBounds[boundsIndex].boundsMin;
                objectPlan.lodGroupBoundsMax =
                    chunkBounds[boundsIndex].boundsMax;
            }
            result.objectPlans.push_back(std::move(objectPlan));
        }
        levelStats.vertexBytes =
            levelStats.vertexCount * sizeof(Vertex);
        levelStats.indexBytes =
            levelStats.indexCount * sizeof(uint32_t);
    }

    // 加载期一次性发布：多次地形加载时按 placement 追加，运行时不改。
    TerrainChunkResidencyStats chunkStats =
        streamingState.SnapshotChunkStats();
    chunkStats.valid = true;
    chunkStats.placements.push_back(std::move(placementStats));
    streamingState.PublishChunkStats(std::move(chunkStats));
    return result;
}
} // namespace VL
