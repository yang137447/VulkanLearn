#include "render/resource/terrainCompositedService.h"
#include "core/runtimeEvidence.h"
#include "core/runtimeFileTrace.h"
#include "core/tracingIfstream.h"

#include "commonFunction.h"
#include "material.h"
#include "materialInstance.h"
#include "texture.h"
#include "textureAssetLoader.h"
#include "renderableObject.h"
#include "render/backend/rendererBackendVulkan.h"
#include "render/resource/rendererTerrainLoader.h"
#include "render/resource/terrainCompositedBakeGpu.h"
#include "render/resource/terrainCompositedProducerSnapshot.h"
#include "render/resource/terrainCompositedWorldBindings.h"
#include "render/resource/rendererResourceCache.h"
#include "render/frame/frameSubmitFault.h"
#include "render/resource/terrainCompositedProductionGpu.h"
#include "render/resource/rendererResourceLoadContext.h"
#include "material/materialAssetUtils.h"
#include "material/loader/materialInstanceResolver.h"
#include "material/materialInstanceStateBatch.h"
#include "shaderCompiler.h"
#include "render/resource/terrainHeightAtlas.h"
#include "render/terrain/terrainCompositedRuntime.h"
#include "render/terrain/terrainCompositedProductionPriority.h"
#include "render/terrain/terrainCompositedServiceRequestDiagnostics.h"
#include "shader/build/atomicFile.h"
#include "terrain/terrainCompositedDependencies.h"
#include "terrain/terrainCompositedAuthorRecipe.h"
#include "terrain/terrainCompositedFeedback.h"
#include "terrain/terrainCompositedProductionIdentity.h"
#include "terrain/terrainCompositedWeightOverlay.h"
#include "terrain/terrainHeightmap.h"
#include "terrain/terrainWeightMap.h"
#include "terrain/terrainPaintSha256.h"
#include "terrain/terrainRvtScheduler.h"
#include "terrain/terrainCompositedRefresh.h"
#include "terrain/terrainCompositedShaderPath.h"
#include "terrain/terrainCompositedQualityClock.h"
#include "terrain/terrainCompositedSourceSnapshots.h"
#include "terrain/terrainCompositedVerifiedInputs.h"
#include "terrain/terrainFaultInjection.h"
#include "terrain/terrainFaultOwnerRegistration.h"
#include "terrain/terrainCompositedPageSourceBatch.h"
#include "render/resource/terrainCompositedResourceSnapshot.h"
#include "render/frame/terrainCompositedReadiness.h"
#include "renderGraph.h"

#include "render/resource/terrainCompositedQualityGpu.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstring>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <map>
#include <mutex>
#include <set>
#include <sstream>
#include <stdexcept>
#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#ifdef MemoryBarrier
#undef MemoryBarrier
#endif
#endif


#include "render/resource/terrainCompositedServiceImpl.h"

namespace VL
{
void TerrainCompositedService::Impl::Capture(TerrainCompositedService::Impl::Surface& surface, const std::shared_ptr<MaterialInstance>& material)
{
    auto& input = surface.input;
    const auto& plan = surface.plan;
    const auto& layerMap = streaming.pageManager.PlacementLayerIdMap(TerrainPlacementIdentity(plan));
    Require(layerMap.size() == plan.desc.layers.size() && !layerMap.empty(),
        "missing placement global layer mapping");
    std::set<uint32_t> sourceSets;
    for (size_t local = 0; local < layerMap.size(); ++local)
    {
        const auto global = layerMap[local];
        Require(global < streaming.layerDefinitions.size(), "global layer mapping out of range");
        sourceSets.insert(streaming.layerDefinitions[global].sourceSet);
        input.localToGlobalLayer[local] = global;
        const auto source = streaming.layerSources.find(streaming.layerDefinitions[global].sourceSet);
        uint32_t slice = 0;
        Require(source != streaming.layerSources.end() &&
            source->second.TryGetLayerSlice(global, slice), "missing source slice mapping");
        input.globalToSourceSlice[global] = slice;
    }
    Require(sourceSets.size() == 1, "S1 supports exactly one sourceSet per surface");
    const auto keep = [&](const std::shared_ptr<Texture>& texture)
    {
        surface.retained.push_back(texture);
        return Sample(backend, texture);
    };
    input.layerAlbedo = keep(material->GetTexture("layerAlbedo"));
    input.layerNormal = keep(material->GetTexture("layerNormal"));
    input.layerOrm = keep(material->GetTexture("layerPbr"));
    input.layerHeight = keep(material->GetTexture("layerHeight"));
    surface.sources = {material->GetTexture("layerAlbedo"), material->GetTexture("layerNormal"),
        material->GetTexture("layerPbr"), material->GetTexture("layerHeight")};
    input.layerCatalog = keep(streaming.layerResources.layerTable);
    input.uvScale = Parameter(*material, "u_uvScale", 64);
    input.localToWorld = Placement(surface.model);
    input.localLayerCount = static_cast<uint32_t>(layerMap.size());
    input.retainFragmentPostprocess = true;
    input.frozenHeightBlendFade = 1;
    input.retainedFragment.heightBlendStrength = Parameter(*material, "u_heightBlendStrength", 0);
    if (Parameter(*material, "u_terrainWeightMapMode", 0) > 0.5f)
    {
        input.weightSource = Baker::WeightSource::LocalLayerArray;
        CaptureImportedWeights(surface, *material);
        RefreshLocalWeights(surface, plan, false);
    }
    const float extent = float(plan.desc.gridResolution + 1);
    input.frameUvOffset = {0.5f / extent, 0.5f / extent};
    input.frameUvScale = {(extent - 1) / extent, (extent - 1) / extent};
    auto frames = Frames(plan);
    input.surfaceLocalNormal = keep(Upload(backend, frames.first, "VTBakeLocalNormal", vk::Filter::eLinear));
    input.surfaceLocalTangent = keep(Upload(backend, frames.second, "VTBakeLocalTangent", vk::Filter::eLinear));
    surface.inputBytes = frames.first.data.size() + frames.second.data.size();
    surface.coverage = {streaming.pageResources.pageTable,
        streaming.pageResources.layerIds, streaming.pageResources.layerWeights};
    input.coverageUvOffset = {float(plan.desc.pageOrigin.x()) / 32.f,
        float(plan.desc.pageOrigin.y()) / 32.f};
    input.coverageUvScale = {float(plan.desc.pageTilesPerSide) / 32.f,
        float(plan.desc.pageTilesPerSide) / 32.f};
    input.coveragePageTable = keep(surface.coverage[0]);
    input.coverageIds = keep(surface.coverage[1]);
    input.coverageWeights = keep(surface.coverage[2]);
    if (surface.importedWeights)
        surface.inputBytes += uint64_t(surface.importedWeights->width) *
            surface.importedWeights->height * surface.importedWeights->arrayLayers * sizeof(float);
    std::vector<uint8_t> mapping(256 * 2 * 16, 0);
    for (uint32_t global = 0; global < 256; ++global)
    {
        const auto slice = input.globalToSourceSlice[global];
        const std::array<float, 4> row{slice == Baker::InvalidSlice ? 0.f : float(slice),
            slice == Baker::InvalidSlice ? 0.f : 1.f, 0, 0};
        std::memcpy(mapping.data() + size_t(global) * 16, row.data(), 16);
        const auto globalId = input.localToGlobalLayer[global];
        const std::array<float, 4> localRow{globalId == Baker::InvalidSlice ? 0.f : float(globalId),
            globalId == Baker::InvalidSlice ? 0.f : 1.f, 0, 0};
        std::memcpy(mapping.data() + (256 + size_t(global)) * 16, localRow.data(), 16);
    }
    const auto sourceMap = Upload(backend, Image(256, 2, true, std::move(mapping)), "TerrainLayerSourceMap");
    surface.sourceMap = sourceMap;
    surface.retained.push_back(sourceMap);
    for (const auto& target : surface.materials)
        target->SetTextureKeepingIdentity("terrainLayerSourceMap", sourceMap);
    std::vector<TerrainHeightAtlasLayer> heightLayers;
    for (size_t local = 0; local < layerMap.size(); ++local)
        if (plan.desc.layers[local].hasHeight)
            heightLayers.push_back({layerMap[local], *sourceSets.begin(), input.globalToSourceSlice[layerMap[local]]});
    if (!heightLayers.empty())
    {
        CanonicalFieldHasher key("terrain-world-height-input-owner");
        key.AddUInt64("sourceTexture", reinterpret_cast<uintptr_t>(material->GetTexture("layerHeight").get()));
        for (const auto& layer : heightLayers)
        {
            key.AddUInt32("globalLayer", layer.globalLayer);
            key.AddUInt32("sourceSet", layer.sourceSet);
            key.AddUInt32("sourceSlice", layer.sourceSlice);
        }
        const auto identity = key.Finalize().ToHex();
        const auto existing = sharedHeightInputs.find(identity);
        if (existing == sharedHeightInputs.end())
            surface.height = sharedHeightInputs.emplace(identity, CreateTerrainHeightAtlas(backend,
                {{*sourceSets.begin(), material->GetTexture("layerHeight")}}, heightLayers,
                frozen->runtime.owner->Caps().inputGpuBytes)).first->second;
        else
            surface.height = existing->second;
    }
}

TerrainCompositedVTPublication TerrainCompositedService::Impl::CapturePublication()
{
    TerrainCompositedVTPublication result;
    auto& closure = result.production;
    closure.adjacency = adjacency;
    std::map<const Texture*, std::string> captured;
    std::map<std::string, std::string> capturedImageObjects;
    const auto archive = [&](TerrainCompositedProductionObject object,
        const std::vector<uint8_t>& bytes)
    {
        try
        {
            TerrainCompositedProductionGpu::WriteInputObject(
                worldManifest.parent_path(), object, bytes);
        }
        catch (const std::exception& error)
        {
            throw std::runtime_error("CapturePublication archive [" + object.objectId +
                " -> " + object.relativePath + "]: " + error.what());
        }
    };
    const auto capture = [&](const std::shared_ptr<Texture>& texture,
        bool repeat, vk::Filter filter) -> std::string
    {
        if (!texture)
            return {};
        const auto existing = captured.find(texture.get());
        if (existing != captured.end())
            return existing->second;
        TerrainCompositedProductionObject object;
        object.objectId = "image-" + std::to_string(captured.size());
        object.relativePath = "inputs/pending.bin";
        const auto extent = backend.GetImageExtent(texture->getImage());
        auto& image = object.image;
        image.width = extent.width;
        image.height = extent.height;
        image.layers = texture->GetArrayLayers();
        image.mipLevels = texture->getMipLevels();
        const std::map<vk::Format, std::string> formats{
            {vk::Format::eR8Unorm, "r8-unorm"}, {vk::Format::eR8G8Unorm, "rg8-unorm"},
            {vk::Format::eR8G8B8A8Unorm, "rgba8-unorm"},
            {vk::Format::eR8G8B8A8Srgb, "rgba8-srgb"}, {vk::Format::eR16Unorm, "r16-unorm"},
            {vk::Format::eR32Sfloat, "r32-float"}, {vk::Format::eR32G32B32A32Sfloat, "rgba32-float"},
            {vk::Format::eBc5UnormBlock, "bc5-unorm"},
            {vk::Format::eBc7UnormBlock, "bc7-unorm"}, {vk::Format::eBc7SrgbBlock, "bc7-srgb"}};
        image.format = formats.at(texture->GetFormat());
        image.transfer = image.format == "rgba8-srgb" || image.format == "bc7-srgb"
            ? TerrainCompositedProductionTransfer::Srgb : TerrainCompositedProductionTransfer::Linear;
        image.address = repeat ? TerrainCompositedProductionAddress::Repeat :
            TerrainCompositedProductionAddress::Clamp;
        image.filter = filter == vk::Filter::eLinear ?
            TerrainCompositedProductionFilter::Linear : TerrainCompositedProductionFilter::Nearest;
        CanonicalFieldHasher metadataHash("terrain-production-image-view");
        metadataHash.AddString("format", image.format);
        metadataHash.AddUInt32("width", image.width);
        metadataHash.AddUInt32("height", image.height);
        metadataHash.AddUInt32("layers", image.layers);
        metadataHash.AddUInt32("mips", image.mipLevels);
        metadataHash.AddUInt32("transfer", static_cast<uint32_t>(image.transfer));
        metadataHash.AddUInt32("address", static_cast<uint32_t>(image.address));
        metadataHash.AddUInt32("filter", static_cast<uint32_t>(image.filter));
        const auto viewIdentity = metadataHash.Finalize().ToHex();
        std::vector<uint8_t> catalog;
        object = TerrainCompositedProductionGpu::CaptureGpuImage(backend, object, texture,
            *frozen->runtime.owner, [&](const auto& metadata, const auto& bytes) {
                auto addressed = metadata;
                addressed.relativePath = "inputs/" + addressed.digest.ToHex() + "-" + viewIdentity + ".bin";
                archive(addressed, bytes);
                if (texture == streaming.layerResources.layerTable)
                    catalog = bytes;
            }, frozen->allocations);
        object.relativePath = "inputs/" + object.digest.ToHex() + "-" + viewIdentity + ".bin";
        const auto objectIdentity = object.digest.ToHex() + viewIdentity;
        const auto duplicate = capturedImageObjects.find(objectIdentity);
        if (duplicate != capturedImageObjects.end())
        {
            captured.emplace(texture.get(), duplicate->second);
            return duplicate->second;
        }
        if (!catalog.empty())
            PopulateTerrainCompositedCatalogDigests(object, catalog);
        captured.emplace(texture.get(), object.objectId);
        capturedImageObjects.emplace(objectIdentity, object.objectId);
        closure.objects.push_back(object);
        return object.objectId;
    };
    const auto archiveMaterial = [&](const Surface& surface, size_t index)
    {
        const auto& path = index == 0 ? surface.plan.desc.materialInstancePath :
            surface.plan.desc.detailMaterialInstancePath;
        const auto raw = ReadBinaryFile(CommonFunction::Path(path));
        auto document = nlohmann::json::parse(raw.begin(), raw.end());
        const auto resolved = MaterialInstanceResolver::Resolve(path, document);
        document.erase("textures");
        document["parameters"] = nlohmann::json::object();
        for (const auto& parameter : surface.materials[index]->CaptureNumericParameterValues())
            std::visit([&](const auto& value) {
                using Value = std::decay_t<decltype(value)>;
                if constexpr (std::is_same_v<Value, float>)
                    document["parameters"][parameter.first] = value;
                else
                {
                    auto array = nlohmann::json::array();
                    for (Eigen::Index component = 0; component < value.size(); ++component)
                        array.push_back(value[component]);
                    document["parameters"][parameter.first] = std::move(array);
                }
            }, parameter.second);
        for (auto parameter = document["parameters"].begin(); parameter != document["parameters"].end();)
            if (resolved.materialJson.at("parameters").contains(parameter.key()) &&
                parameter.value() == resolved.materialJson.at("parameters").at(parameter.key()).at("default"))
                parameter = document["parameters"].erase(parameter);
            else
                ++parameter;
        if (document["parameters"].empty())
            document.erase("parameters");
        MaterialInstanceResolver::ResolveWithDefinition(path, document, resolved.materialJson);
        const auto text = document.dump(2);
        const auto hash = ContentHasher::HashBytes(text.data(), text.size()).ToHex();
        const auto relative = "runtime/" + hash + ".json";
        std::filesystem::create_directories(worldManifest.parent_path() / "runtime");
        WriteTextFileAtomically(worldManifest.parent_path() / relative, text);
        return relative;
    };
    for (const auto& registered : surfaces)
    {
        auto& surface = *registered;
        const auto& input = surface.input;
        TerrainCompositedSurfaceProduction production;
        production.surfaceId = surface.desc.surfaceId;
        production.runtimeMaterialInstancePath = archiveMaterial(surface, 0);
        if (surface.materials.size() > 1)
            production.runtimeDetailMaterialInstancePath = archiveMaterial(surface, 1);
        production.weightSource = input.weightSource == Baker::WeightSource::LocalLayerArray
            ? TerrainCompositedWeightSource::LocalLayerArray : TerrainCompositedWeightSource::CoverageAtlas;
        const auto& local = streaming.pageManager.PlacementLayerIdMap(surface.desc.surfaceId);
        production.localToGlobal.assign(local.begin(), local.end());
        production.sourceSet = streaming.layerDefinitions.at(local.front()).sourceSet;
        production.globalToSourceSlice = input.globalToSourceSlice;
        std::copy(input.localToWorld.begin(), input.localToWorld.end(), production.localToWorld.begin());
        const auto pair = [](const std::array<float, 2>& value)
        { return std::array<double, 2>{value[0], value[1]}; };
        auto& sampling = production.sampling;
        sampling.surfaceUvOffset = pair(input.surfaceUvOffset);
        sampling.surfaceUvScale = pair(input.surfaceUvScale);
        sampling.coverageUvOffset = pair(input.coverageUvOffset);
        sampling.coverageUvScale = pair(input.coverageUvScale);
        sampling.frameUvOffset = pair(input.frameUvOffset);
        sampling.frameUvScale = pair(input.frameUvScale);
        production.parameters.uvScale = input.uvScale;
        production.parameters.heightBlendStrength = input.retainedFragment.heightBlendStrength;
        auto& images = production.inputs;
        images.coveragePageTable = capture(surface.coverage[0], false, vk::Filter::eNearest);
        images.coverageIds = capture(surface.coverage[1], false, vk::Filter::eNearest);
        images.coverageWeights = capture(surface.coverage[2], false, vk::Filter::eLinear);
        images.layerCatalog = capture(streaming.layerResources.layerTable, false, vk::Filter::eNearest);
        images.layerAlbedo = capture(surface.sources[0], true, vk::Filter::eLinear);
        images.layerNormal = capture(surface.sources[1], true, vk::Filter::eLinear);
        images.layerOrm = capture(surface.sources[2], true, vk::Filter::eLinear);
        images.layerHeight = capture(surface.sources[3], true, vk::Filter::eLinear);
        images.localLayerWeights = capture(surface.localWeights, false, vk::Filter::eLinear);
        for (const auto& texture : surface.retained)
        {
            if (texture->getImageView() == input.surfaceLocalNormal.descriptor.imageView)
                images.surfaceLocalNormal = capture(texture, false, vk::Filter::eLinear);
            if (texture->getImageView() == input.surfaceLocalTangent.descriptor.imageView)
                images.surfaceLocalTangent = capture(texture, false, vk::Filter::eLinear);
        }
        images.heightAtlas = capture(surface.height.heightAtlas, false, vk::Filter::eLinear);
        images.heightLayerMap = capture(surface.height.layerMap, false, vk::Filter::eNearest);
        images.globalColour = capture(surface.materials.front()->GetTexture("globalColour"), false, vk::Filter::eLinear);
        images.globalMasks = capture(surface.materials.front()->GetTexture("globalMasks"), false, vk::Filter::eLinear);
        closure.surfaces.push_back(TerrainCompositedProductionGpu::FreezeSurface(
            surface.desc.surfaceId, production.sourceSet, input, std::move(production.inputs),
            production.runtimeMaterialInstancePath, production.runtimeDetailMaterialInstancePath));
    }
    auto& compiler = pipelineFactory.GetShaderCompiler();
    const auto manifest = compiler.CaptureManifestSnapshot();
    auto& generator = closure.generator;
    generator.version = TerrainCompositedGeneratorVersion;
    generator.evaluator = "M_terrain";
    generator.filterRule = TerrainCompositedFilterRule;
    generator.encoding = TerrainCompositedEncoding;
    generator.compilePolicy = manifest.compilePolicy;
    std::map<std::string, std::string> capturedShaders;
    std::map<std::string, std::string> capturedShaderPaths;
    const auto shader = [&](const std::string& identity, const std::string& digest,
        TerrainCompositedProductionObjectKind kind)
    {
        const auto relative = TerrainCompositedShaderRelativePath(identity,
            kind == TerrainCompositedProductionObjectKind::ShaderSource);
        const auto authorPath = (std::filesystem::path("shader") / relative).generic_string();
        const auto previous = capturedShaders.find(digest);
        if (previous != capturedShaders.end())
        {
            capturedShaderPaths[identity] = previous->second;
            capturedShaderPaths[authorPath] = previous->second;
            return previous->second;
        }
        const auto bytes = ReadBinaryFile(compiler.GetShaderRoot() / relative);
        TerrainCompositedProductionObject object;
        object.objectId = "shader-" + std::to_string(closure.objects.size());
        object.kind = kind;
        object.bytes = bytes.size();
        object.digest = ContentHasher::HashBytes(bytes.data(), bytes.size());
        Require(object.digest.ToHex() == digest, "generator artifact/source changed during freeze");
        object.relativePath = "inputs/" + object.digest.ToHex() + ".bin";
        archive(object, bytes);
        closure.objects.push_back(object);
        capturedShaders.emplace(digest, object.objectId);
        capturedShaderPaths[identity] = object.objectId;
        capturedShaderPaths[authorPath] = object.objectId;
        return object.objectId;
    };
    const auto include = [&](const std::string& id)
    {
        if (id != generator.computeSource &&
            std::find(generator.includeSources.begin(), generator.includeSources.end(), id) ==
                generator.includeSources.end())
            generator.includeSources.push_back(id);
    };
    for (const auto& artifact : manifest.artifacts)
    {
        if (artifact.second.normalizedKey.find("terrain/terrainCompositedBakeWorld.comp") == std::string::npos &&
            artifact.second.normalizedKey.find("terrain/terrainCompositedBakeWorldPadding.comp") == std::string::npos)
            continue;
        for (const auto& source : artifact.second.primarySources)
        {
            const auto id = shader(source.identity, source.digest,
                TerrainCompositedProductionObjectKind::ShaderSource);
            if (generator.computeSource.empty() &&
                artifact.second.normalizedKey.find("terrain/terrainCompositedBakeWorld.comp") != std::string::npos)
                generator.computeSource = id;
            else
                include(id);
        }
        for (const auto& dependency : artifact.second.dependencies)
            include(shader(dependency.path, dependency.digest,
                TerrainCompositedProductionObjectKind::ShaderSource));
        for (const auto& output : artifact.second.outputs)
        {
            const auto id = shader(output.second.path, output.second.digest,
                TerrainCompositedProductionObjectKind::ShaderArtifact);
            if (std::find(generator.compiledArtifacts.begin(), generator.compiledArtifacts.end(), id) ==
                generator.compiledArtifacts.end())
                generator.compiledArtifacts.push_back(id);
        }
    }
    generator.includeSources.erase(std::remove(generator.includeSources.begin(),
        generator.includeSources.end(), generator.computeSource), generator.includeSources.end());
    for (auto& production : closure.surfaces)
    {
        const auto found = std::find_if(surfaces.begin(), surfaces.end(),
            [&](const auto& surface) { return surface->desc.surfaceId == production.surfaceId; });
        Require(found != surfaces.end() && !(*found)->authorManifest.is_null(),
            "Cook is missing the pre-capture author snapshot");
        ValidateTerrainCompositedAuthors(Roots(), (*found)->authorManifest);
        const auto& author = **found;
        std::map<std::string, std::set<std::string>> provenance;
        const auto add = [&](const std::string& path, const std::string& object)
        {
            if (!path.empty() && !object.empty())
                provenance[path].insert(object);
        };
        const auto& bindings = production.inputs;
        const std::vector<std::string> derived{
            bindings.coveragePageTable, bindings.coverageIds, bindings.coverageWeights,
            bindings.layerCatalog, bindings.localLayerWeights,
            bindings.surfaceLocalNormal, bindings.surfaceLocalTangent,
            bindings.heightAtlas, bindings.heightLayerMap};
        for (const auto& object : derived)
            add(author.plan.desc.assetPath, object);
        for (const auto& object : {bindings.surfaceLocalNormal, bindings.surfaceLocalTangent,
            bindings.coverageIds, bindings.coverageWeights})
            add(author.plan.desc.heightmapPath, object);
        for (const auto& path : {author.plan.desc.paintPath,
            author.plan.desc.paintPath.empty() ? std::string() : author.plan.desc.paintPath + ".meta.json"})
            for (const auto& object : {bindings.coverageIds, bindings.coverageWeights, bindings.localLayerWeights})
                add(path, object);
        std::set<std::string> materialObjects;
        for (const auto& material : author.materials)
            for (const auto& binding : material->CaptureStateSnapshot().textures)
            {
                const auto image = captured.find(binding.second.texture.get());
                if (image == captured.end() || !binding.second.textureAssetIdentity)
                    continue;
                const auto& asset = *binding.second.textureAssetIdentity;
                add(asset, image->second);
                for (const auto& source : LoadTextureAssetDesc(asset).sources)
                    add(source, image->second);
                materialObjects.insert(image->second);
            }
        materialObjects.insert(generator.computeSource);
        for (const auto& dependency : author.authorManifest.at("dependencies"))
        {
            const auto path = dependency.at("path").get<std::string>();
            const auto shaderObject = capturedShaderPaths.find(path);
            if (shaderObject != capturedShaderPaths.end())
                add(path, shaderObject->second);
            else if (path.rfind("shader/glsl/", 0) == 0 && dependency.at("mode") == "bytes")
            {
                const auto roots = Roots();
                const auto root = dependency.at("root") == "project" ? roots.projectRoot : roots.resourceRoot;
                const auto bytes = ReadBinaryFile(root / path);
                const auto digest = ContentHasher::HashBytes(bytes.data(), bytes.size());
                Require(digest.ToHex() == dependency.at("digest").get<std::string>(),
                    "author shader changed after the frozen dependency snapshot: " + path);
                TerrainCompositedProductionObject object;
                object.objectId = "author-shader-" + std::to_string(closure.objects.size());
                object.kind = TerrainCompositedProductionObjectKind::ShaderSource;
                object.bytes = bytes.size();
                object.digest = digest;
                object.relativePath = "inputs/" + digest.ToHex() + ".bin";
                archive(object, bytes);
                capturedShaderPaths.emplace(path, object.objectId);
                closure.objects.push_back(object);
                add(path, object.objectId);
            }
            if (dependency.at("mode") == "terrain-material-instance-static-json-v2" ||
                dependency.at("mode") == "terrain-material-static-json-v2")
                provenance[path].insert(materialObjects.begin(), materialObjects.end());
        }
        for (const auto& dependency : (*found)->authorManifest.at("dependencies"))
        {
            const auto path = dependency.at("path").get<std::string>();
            if (!provenance.count(path) || provenance.at(path).empty())
            {
                const auto roots = Roots();
                const auto root = dependency.at("root") == "project" ? roots.projectRoot : roots.resourceRoot;
                const auto mode = dependency.at("mode").get<std::string>();
                std::vector<uint8_t> bytes;
                if (mode == "absent")
                {
                    Require(!std::filesystem::exists(root / path), "optional author appeared during freeze: " + path);
                    const auto text = nlohmann::json{{"kind", "terrain-author-absent"},
                        {"dependency", dependency}}.dump();
                    bytes.assign(text.begin(), text.end());
                }
                else
                {
                    bytes = ReadBinaryFile(root / path);
                    const std::string raw(bytes.begin(), bytes.end());
                    Require(TerrainCompositedDependencyEntryHash(raw, mode, path).ToHex() ==
                        dependency.at("digest").get<std::string>(),
                        "author input changed after frozen dependency capture: " + path);
                }
                TerrainCompositedProductionObject object;
                object.kind = TerrainCompositedProductionObjectKind::RawAuthorSource;
                object.objectId = "author-raw-" + std::to_string(closure.objects.size());
                object.bytes = bytes.size();
                object.digest = ContentHasher::HashBytes(bytes.data(), bytes.size());
                object.relativePath = "author-inputs/" + object.digest.ToHex() + ".bin";
                const auto existing = std::find_if(closure.objects.begin(), closure.objects.end(),
                    [&](const auto& input) { return input.relativePath == object.relativePath; });
                if (existing == closure.objects.end())
                {
                    archive(object, bytes);
                    closure.objects.push_back(object);
                    add(path, object.objectId);
                }
                else
                    add(path, existing->objectId);
            }
            const auto mapped = provenance.find(path);
            production.authorDependencies.push_back({
                dependency.at("root").get<std::string>(),
                path,
                dependency.at("mode").get<std::string>(),
                dependency.at("digest").get<std::string>(),
                std::vector<std::string>(mapped->second.begin(), mapped->second.end())});
        }
    }
    closure.Validate();
    for (auto& surface : surfaces)
    {
        surface->desc.dependencyHash = closure.SurfaceContentHash(surface->desc.surfaceId);
        surface->input.contentIdentity = surface->desc.ContentHash().ToHex();
        result.surfaces.push_back(surface->desc);
        const auto layout = surface->desc.Layout();
        for (uint32_t mip = std::min(frozen->packagedMinMip, layout.MipCount() - 1);
            mip < layout.MipCount(); ++mip)
            for (uint32_t pageY = 0; pageY < layout.Mip(mip).pagesY; ++pageY)
                for (uint32_t pageX = 0; pageX < layout.Mip(mip).pagesX; ++pageX)
                    result.packagedPages.push_back({surface->desc.surfaceId, mip, pageX, pageY});
    }
    return result;
}

TerrainCompositedVTPageData TerrainCompositedService::Impl::Bake(const TerrainCompositedService::Impl::Surface& surface, const TerrainCompositedVTPageKey& key)
{
    Baker::PageRequest request;
    request.virtualWidth = surface.desc.width;
    request.virtualHeight = surface.desc.height;
    request.virtualMip = key.mip;
    request.pageX = key.x;
    request.pageY = key.y;
    request.areaSamplesPerAxis = key.mip == 0 ? 1 : 4;
    Baker::PageBytes result;
    std::string error;
    Baker::AdjacencySnapshot world;
    world.sourceSurfaceId = surface.desc.surfaceId;
    world.graph = adjacency;
    for (const auto& neighbor : surfaces)
    {
        auto input = neighbor->input;
        input.surfaceId = neighbor->desc.surfaceId;
        input.adjacency.reset();
        world.surfaces.push_back({neighbor->desc.surfaceId, std::move(input)});
    }
    const auto outputUsage = vk::BufferUsageFlagBits::eStorageBuffer |
        vk::BufferUsageFlagBits::eTransferSrc | vk::BufferUsageFlagBits::eTransferDst;
    const auto outputBytes = backend.QueryBufferMemoryRequirements(Baker::OutputBytes, outputUsage).size;
    const auto scratchExtent = Baker::RequiredScratchBytes(request.areaSamplesPerAxis);
    const uint64_t scratchBytes = scratchExtent ? backend.QueryBufferMemoryRequirements(
        scratchExtent, vk::BufferUsageFlagBits::eStorageBuffer |
            vk::BufferUsageFlagBits::eIndirectBuffer).size : 0;
    const auto readbackBytes = backend.QueryBufferMemoryRequirements(
        Baker::OutputBytes, vk::BufferUsageFlagBits::eTransferDst).size;
    auto lease = frozen->runtime.owner->TryReserve(
        {outputBytes + scratchBytes + readbackBytes, readbackBytes, 0, 1},
        TerrainRvtCostClass::Workspace, TerrainRvtResourcePhase::Candidate);
    Require(lease.has_value(), "Cook output/scratch exceeds frozen workspace");
    world.cookWorkspaceLease = std::make_shared<const TerrainRvtBudgetLease>(std::move(*lease));
    Require(baker->BakePageSync(world, request, result, error), "GPU Cook failed: " + error);
    TerrainCompositedVTPageData page{key, {}};
    page.bytes.reserve(surface.desc.PageBytes());
    for (const auto* channel : {&result.albedoSrgb, &result.normalLocalUnorm, &result.ormLinear})
        page.bytes.insert(page.bytes.end(), channel->begin(), channel->end());
    Require(page.bytes.size() == surface.desc.PageBytes() &&
        result.contentIdentity == surface.input.contentIdentity, "GPU Cook returned mismatched page");
    return page;
}
} // namespace VL
