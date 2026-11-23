#pragma once

#include "terrain/terrainCompositedVTStore.h"
#include "shader/build/atomicFile.h"

#include <cstring>
#include <fstream>

namespace TerrainProductionFixture
{
inline void WriteFixtureBytes(
    const std::filesystem::path& path, const std::vector<uint8_t>& bytes)
{
    std::filesystem::create_directories(path.parent_path());
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    output.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    output.close();
    if (!output)
        throw std::runtime_error("Failed to write owned Production test fixture: " + path.string());
}

inline void WriteFixtureText(const std::filesystem::path& path, const std::string& text)
{
    WriteFixtureBytes(path, std::vector<uint8_t>(text.begin(), text.end()));
}

inline VL::TerrainCompositedProductionClosure Closure(
    const std::filesystem::path& root, const std::vector<std::string>& ids,
    VL::TerrainCompositedWeightSource mode = VL::TerrainCompositedWeightSource::CoverageAtlas)
{
    using namespace VL;
    TerrainCompositedProductionClosure closure;
    const auto image = [&](const std::string& id, const std::string& format,
        uint32_t width, uint32_t height, uint32_t layers = 1, uint32_t mips = 1)
    {
        TerrainCompositedProductionObject object;
        object.objectId = id;
        object.image.format = format;
        object.image.transfer = format == "rgba8-srgb" || format == "bc7-srgb" ?
            TerrainCompositedProductionTransfer::Srgb : TerrainCompositedProductionTransfer::Linear;
        object.image.width = width;
        object.image.height = height;
        object.image.layers = layers;
        object.image.mipLevels = mips;
        object.image.filter = TerrainCompositedProductionFilter::Linear;
        object.bytes = object.image.ByteSize();
        std::vector<uint8_t> bytes(static_cast<size_t>(object.bytes), 0);
        if (format == "rgba32-float" || format == "r32-float")
        {
            for (size_t offset = 0; offset < bytes.size(); offset += 4)
            {
                const float value = id == "frame-normal" ? (offset % 16 == 4 ? 1.f : 0.f) :
                    id == "frame-tangent" ? (offset % 16 == 0 || offset % 16 == 12 ? 1.f : 0.f) : 1.f;
                std::memcpy(bytes.data() + offset, &value, 4);
            }
        }
        else
            std::fill(bytes.begin(), bytes.end(), ContentHasher::HashString(id).bytes[0]);
        object.digest = ContentHasher::HashBytes(bytes.data(), bytes.size());
        if (id == "catalog")
            PopulateTerrainCompositedCatalogDigests(object, bytes);
        object.relativePath = "objects/" + object.digest.ToHex() + ".bin";
        WriteFixtureBytes(root / object.relativePath, bytes);
        closure.objects.push_back(object);
        return id;
    };
    const auto shader = [&](const std::string& id, TerrainCompositedProductionObjectKind kind)
    {
        TerrainCompositedProductionObject object;
        object.objectId = id;
        object.kind = kind;
        const std::string text = id == "compute" ? "void main() {}" : "fixture-spv";
        object.bytes = text.size();
        object.digest = ContentHasher::HashString(text);
        object.relativePath = "objects/" + object.digest.ToHex() + ".bin";
        WriteFixtureText(root / object.relativePath, text);
        closure.objects.push_back(object);
        return id;
    };
    closure.generator = {TerrainCompositedGeneratorVersion, "M_terrain", TerrainCompositedFilterRule,
        TerrainCompositedEncoding, "test-release",
        shader("compute", TerrainCompositedProductionObjectKind::ShaderSource), {},
        {shader("artifact", TerrainCompositedProductionObjectKind::ShaderArtifact)}};
    TerrainCompositedProductionBindings inputs;
    if (mode == TerrainCompositedWeightSource::CoverageAtlas)
    {
        inputs.coveragePageTable = image("coverage-table", "rgba8-unorm", 1, 1);
        inputs.coverageIds = image("coverage-ids", "rgba8-unorm", 128, 128);
        inputs.coverageWeights = image("coverage-weights", "rgba8-unorm", 128, 128);
    }
    else
        inputs.localLayerWeights = image("local-weights", "r32-float", 4, 4);
    inputs.layerCatalog = image("catalog", "rgba32-float", 256, 4);
    inputs.layerAlbedo = image("albedo", "rgba8-srgb", 8, 8);
    inputs.layerNormal = image("normal", "rgba8-unorm", 8, 8);
    inputs.layerOrm = image("orm", "rgba8-unorm", 8, 8);
    inputs.surfaceLocalNormal = image("frame-normal", "rgba32-float", 3, 3);
    inputs.surfaceLocalTangent = image("frame-tangent", "rgba32-float", 3, 3);
    for (const auto& id : ids)
    {
        TerrainCompositedSurfaceProduction surface;
        surface.surfaceId = id;
        surface.runtimeMaterialInstancePath = "runtime/MI_terrain.json";
        surface.weightSource = mode;
        surface.localToGlobal = {7};
        surface.globalToSourceSlice[7] = 0;
        surface.inputs = inputs;
        surface.authorDependencies = {{"project", "shader/glsl/terrain/terrainCompositedBake.comp",
            "bytes", closure.Object("compute").digest.ToHex(), {"compute"}}};
        closure.surfaces.push_back(surface);
    }
    WriteFixtureText(root / "runtime/MI_terrain.json",
        R"({"type":"materialInstance","material":"shader/glsl/M_terrain.json","parameters":{}})");
    closure.adjacency = TerrainCompositedAdjacencyGraph(ids, {});
    closure.Validate();
    return closure;
}

inline VL::TerrainCompositedVTPublication Publication(
    const std::filesystem::path& root, std::vector<VL::TerrainCompositedVTBuildDesc> surfaces,
    const std::vector<VL::TerrainCompositedVTPageData>& pages)
{
    std::vector<std::string> ids;
    for (const auto& surface : surfaces)
        ids.push_back(surface.surfaceId);
    VL::TerrainCompositedVTPublication publication;
    publication.production = Closure(root, ids);
    for (auto& surface : surfaces)
        surface.dependencyHash = publication.production.SurfaceContentHash(surface.surfaceId);
    publication.surfaces = std::move(surfaces);
    for (const auto& page : pages)
        publication.packagedPages.push_back(page.key);
    return publication;
}
}
