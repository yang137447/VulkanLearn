#include <gtest/gtest.h>

#include <algorithm>

#include <atomic>
#include <chrono>
#include <fstream>
#include <iterator>
#include <set>
#include <vector>
#include "gameExporter.h"
#include "shader/build/contentHash.h"
#include "shader/build/shaderBuildManifest.h"
#include "terrainPackageClosure.h"
#include "material/generator/materialParameterIncludeGenerator.h"

namespace
{
namespace fs = std::filesystem;
using Json = nlohmann::json;

void Write(const fs::path& path, const std::string& value)
{
    fs::create_directories(path.parent_path());
    std::ofstream output(path, std::ios::binary);
    output.write(value.data(), static_cast<std::streamsize>(value.size()));
    if (!output) throw std::runtime_error("Fixture write failed");
}

void WriteJson(const fs::path& path, const Json& value) { Write(path, value.dump()); }
Json ReadJson(const fs::path& path)
{
    std::ifstream input(path);
    return Json::parse(input);
}

void Put(std::string& bytes, size_t offset, uint32_t value, size_t length = 4)
{
    for (size_t index = 0; index < length; ++index) bytes.at(offset + index) = static_cast<char>(value >> (index * 8));
}

std::string Pe(const std::string& dependency = {}, bool delayed = false)
{
    std::string bytes(0x500, '\0');
    Put(bytes, 0, 0x5a4d, 2);
    Put(bytes, 0x3c, 0x80);
    Put(bytes, 0x80, 0x4550);
    Put(bytes, 0x84, 0x8664, 2);
    Put(bytes, 0x86, 1, 2);
    Put(bytes, 0x94, 0xf0, 2);
    Put(bytes, 0x98, 0x20b, 2);
    Put(bytes, 0x104, 16);
    Put(bytes, 0x194, 0x1000);
    Put(bytes, 0x198, 0x300);
    Put(bytes, 0x19c, 0x200);
    if (!dependency.empty())
    {
        Put(bytes, delayed ? 0x170 : 0x110, 0x1000);
        Put(bytes, delayed ? 0x174 : 0x114, delayed ? 64 : 40);
        if (delayed) Put(bytes, 0x200, 1);
        Put(bytes, delayed ? 0x204 : 0x20c, 0x1100);
        bytes.replace(0x300, dependency.size(), dependency);
    }
    return bytes;
}

class GameExportTest : public ::testing::Test
{
protected:
    void SetUp() override
    {
        static std::atomic<uint64_t> sequence{0};
        root = fs::temp_directory_path() / ("vl-game-export-" +
            VL::ContentHasher::HashString(std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()) +
                std::to_string(++sequence)).ToHex());
        options.projectRoot = root / "project";
        options.binaryDirectory = root / "binaries";
        options.outputDirectory = root / "game";
        options.initialScene = "Maps/SC_test/SC_test.json";
        resources = root / "assets";
        WriteJson(options.projectRoot / "config/config.json", {{"resourcePath", "../assets"},
            {"initScene", "Maps/SC_old/SC_old.json"}, {"projectPath", root.string()},
            {"scripts", {{"enabled", false}}}, {"ui", {{"assetRoot", "ui"}, {"developerUiEnabled", true}}}});
        WriteJson(options.projectRoot / "config/renderGraphConfig.json", {{"passes", Json::array()}});
        const auto frozenRoot = fs::path(__FILE__).parent_path().parent_path().parent_path() / "config";
        for (const auto* name : {"terrainRvtP0Frozen.json", "terrainRvtP0Frozen.json.sha256"})
            fs::copy_file(frozenRoot / name, options.projectRoot / "config" / name);
        Write(options.projectRoot / "ui/runtime-hud.rml", "<rml/>");
        Write(options.projectRoot / "shader/glsl/test.frag", "void main(){}");
        Write(options.projectRoot / "shader/spv/test.spv", "compiled");
        Write(options.binaryDirectory / "main.exe", Pe());
        WriteJson(resources / options.initialScene, {{"type", "scene"}, {"objects", Json::array()}});
    }

    void TearDown() override
    {
        std::error_code error;
        fs::remove_all(root, error);
    }

    void Scene(const Json& objects)
    {
        WriteJson(resources / options.initialScene, {{"type", "scene"}, {"objects", objects}});
    }

    fs::path TerrainPackage(
        VL::TerrainCompositedWeightSource mode = VL::TerrainCompositedWeightSource::CoverageAtlas)
    {
        const std::string manifestName = "TerrainCook/tile_00/manifest.json";
        const auto manifestPath = resources / manifestName;
        auto production = TerrainProduction(mode);
        fs::create_directories(manifestPath.parent_path());
        for (const auto& object : production.objects)
        {
            fs::create_directories((manifestPath.parent_path() / object.relativePath).parent_path());
            fs::copy_file(resources / object.relativePath, manifestPath.parent_path() / object.relativePath,
                fs::copy_options::overwrite_existing);
        }
        for (const auto& terrainSurface : production.surfaces)
            for (const auto& material : {terrainSurface.runtimeMaterialInstancePath,
                     terrainSurface.runtimeDetailMaterialInstancePath})
            {
                fs::create_directories((manifestPath.parent_path() / material).parent_path());
                fs::copy_file(resources / material, manifestPath.parent_path() / material,
                    fs::copy_options::overwrite_existing);
            }
        VL::TerrainCompositedVTBuildDesc surface;
        surface.surfaceId = production.surfaces.front().surfaceId;
        surface.width = surface.height = 1024;
        surface.dependencyHash = production.SurfaceContentHash(surface.surfaceId);
        surface.generatorVersion = production.generator.version;
        surface.encoding = production.generator.encoding;
        surface.channels = {VL::TerrainCompositedVTFormat::RGBA8Srgb,
            VL::TerrainCompositedVTFormat::RGBA8UNorm, VL::TerrainCompositedVTFormat::RGBA8UNorm};
        VL::TerrainCompositedVTPublication publication;
        publication.production = production;
        publication.surfaces = {surface};
        publication.packagedPages = {{surface.surfaceId, surface.Layout().MipCount() - 1, 0, 0}};
        VL::TerrainCompositedVTStore::Publish(manifestPath, publication,
            VL::TerrainCompositedVTStore::PageProducer([&](const auto&)
            {
                return std::vector<uint8_t>(static_cast<size_t>(surface.PageBytes()), 127);
            }));
        WriteJson(resources / "Generated/TR_composited.json", {
            {"name", "TR_composited"}, {"type", "terrain"}, {"pageTilesPerSide", 1},
            {"heightmapPath", "Generated/Geometry/height.png"}, {"size", {128, 128}},
            {"heightScale", 100}, {"heightOffset", 0}, {"gridResolution", 1}, {"chunkQuads", 1},
            {"materialInstancePath", "Generated/Author/MI_original.json"},
            {"compositedVT", {{"enabled", true}, {"packagePath", manifestName},
                {"virtualResolution", {1024, 1024}}}}});
        Write(resources / "Generated/Geometry/height.png", "geometry-height-fixture");
        Scene(Json::array({{{"type", "terrain"}, {"name", "Landscape_00"},
            {"terrainPath", "Generated/TR_composited.json"}}}));
        return manifestPath;
    }

    VL::TerrainCompositedProductionClosure TerrainProduction(VL::TerrainCompositedWeightSource mode)
    {
        VL::TerrainCompositedProductionClosure production;
        auto& generator = production.generator;
        generator.version = 3;
        generator.evaluator = "M_terrain";
        generator.filterRule = "mip0-center-coarse-area-local-normal-rvt-adjacency-padding-v3";
        generator.encoding = "gpu-rvt-adjacency-area4-rgba8-v3";
        generator.compilePolicy = "fixture-release";
        auto add = [&](const std::string& name, VL::TerrainCompositedProductionObjectKind kind,
                       const VL::TerrainCompositedProductionImageDesc& image, const std::string& bytes)
        {
            VL::TerrainCompositedProductionObject object;
            object.objectId = name;
            object.relativePath = "inputs/" + name;
            object.kind = kind;
            object.image = image;
            object.bytes = bytes.size();
            object.digest = VL::ContentHasher::HashString(bytes);
            if (name == "catalog")
                VL::PopulateTerrainCompositedCatalogDigests(object,
                    std::vector<uint8_t>(bytes.begin(), bytes.end()));
            Write(resources / object.relativePath, bytes);
            production.objects.push_back(object);
            return name;
        };
        auto image = [&](const std::string& name, const std::string& format,
                         uint32_t width, uint32_t height, uint32_t layers = 1)
        {
            VL::TerrainCompositedProductionImageDesc description;
            description.format = format;
            description.width = width;
            description.height = height;
            description.layers = layers;
            description.transfer = format == "rgba8-srgb"
                ? VL::TerrainCompositedProductionTransfer::Srgb
                : VL::TerrainCompositedProductionTransfer::Linear;
            description.filter = VL::TerrainCompositedProductionFilter::Linear;
            return add(name, VL::TerrainCompositedProductionObjectKind::Image, description,
                std::string(static_cast<size_t>(description.ByteSize()), '\0'));
        };
        const auto cachePath = options.projectRoot / "shader/spv/shader-build-cache.json";
        if (fs::is_regular_file(cachePath))
        {
            const auto cache = ReadJson(cachePath);
            generator.compilePolicy = cache.at("compilePolicy").get<std::string>();
            auto bytes = [](const fs::path& path)
            {
                std::ifstream input(path, std::ios::binary);
                if (!input) throw std::runtime_error("Missing shader fixture");
                return std::string(std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>());
            };
            for (const auto& artifact : cache.at("artifacts"))
            {
                if (artifact.at("normalizedKey").get<std::string>().find("path=terrain/terrainCompositedBakeWorld") ==
                    std::string::npos) continue;
                for (const auto& primary : artifact.at("primarySources"))
                {
                    const auto identity = primary.at("identity").get<std::string>();
                    const auto id = add("primary-" + std::to_string(production.objects.size()) + ".glsl",
                        VL::TerrainCompositedProductionObjectKind::ShaderSource, {},
                        bytes(options.projectRoot / "shader/glsl" / identity));
                    if (identity == "terrain/terrainCompositedBakeWorld.comp")
                        generator.computeSource = id;
                    else
                        generator.includeSources.push_back(id);
                }
                for (const auto& dependency : artifact.at("dependencies"))
                {
                    const auto path = dependency.at("path").get<std::string>();
                    if (path == "terrain/terrainCompositedBakeWorld.comp")
                        continue;
                    generator.includeSources.push_back(add(
                        "include-" + std::to_string(generator.includeSources.size()) + ".glsl",
                        VL::TerrainCompositedProductionObjectKind::ShaderSource,
                        {}, bytes(options.projectRoot / "shader/glsl" / path)));
                }
                for (const auto& output : artifact.at("outputs"))
                    generator.compiledArtifacts.push_back(add(
                        "bake-" + std::to_string(generator.compiledArtifacts.size()) + ".spv",
                        VL::TerrainCompositedProductionObjectKind::ShaderArtifact, {},
                        bytes(options.projectRoot / "shader" / output.at("path").get<std::string>())));
            }
        }
        else
        {
            generator.computeSource = add("bake.comp",
                VL::TerrainCompositedProductionObjectKind::ShaderSource, {}, "void main(){}");
            generator.includeSources.push_back(add("evaluation.glsl",
                VL::TerrainCompositedProductionObjectKind::ShaderSource, {}, "vec4 evaluate();"));
            generator.compiledArtifacts.push_back(add("bake.spv",
                VL::TerrainCompositedProductionObjectKind::ShaderArtifact, {}, "fixture-spv"));
        }
        VL::TerrainCompositedSurfaceProduction surface;
        surface.surfaceId = "terrain|Landscape_00";
        surface.runtimeMaterialInstancePath = "Generated/MI_runtime_terrain.json";
        surface.runtimeDetailMaterialInstancePath = "Generated/MI_runtime_detail_terrain.json";
        WriteJson(resources / surface.runtimeMaterialInstancePath,
            {{"type", "materialInstance"}, {"material", "shader/glsl/M_terrain.json"}});
        WriteJson(resources / surface.runtimeDetailMaterialInstancePath,
            {{"type", "materialInstance"}, {"material", "shader/glsl/M_terrain.json"}});
        WriteJson(options.projectRoot / "shader/glsl/M_terrain.json",
            {{"name", "M_terrain"}, {"type", "material"}, {"shadingModel", "DefaultLit"},
             {"renderStates", Json::object()}, {"macros", Json::object()},
             {"parameters", {{"u_runtimeTest", {{"type", "float"}, {"default", 1.0},
                {"description", "Runtime material semantic validation fixture"}}}}},
             {"textures", {{"layerAlbedo", {{"type", "sampler2DArray"},
                {"default", nullptr}}}}}});
        const auto terrainGenerated = MaterialParameterIncludeGenerator::BuildGeneratedIncludeContent(
            options.projectRoot / "shader/glsl/M_terrain.json");
        Write(terrainGenerated.outputPath, terrainGenerated.generatedBytes);
        if (fs::is_regular_file(cachePath))
        {
            auto cache = ReadJson(cachePath);
            cache["artifacts"]["terrain"] = ShaderArtifact("GraphicsPair",
                "kind=GraphicsPair|shaderName=terrain|renderMode=Opaque|shadingModel=SHADING_MODEL_DEFAULT_LIT|macros=|target=Vulkan1.4",
                {{"runtimeVertex", "spv/terrain.vert.spv"}, {"runtimeFragment", "spv/terrain.frag.spv"}},
                {"terrain.vert", "terrain.frag"}, {"terrain.vert", "terrain.frag"});
            WriteJson(cachePath, cache);
        }
        surface.weightSource = mode;
        surface.localToGlobal = {7, 3};
        surface.globalToSourceSlice[7] = 1;
        surface.globalToSourceSlice[3] = 0;
        auto& inputs = surface.inputs;
        inputs.layerCatalog = image("catalog", "rgba32-float", 256, 4);
        inputs.layerAlbedo = image("albedo", "rgba8-srgb", 4, 4, 2);
        inputs.layerNormal = image("normal", "rgba8-unorm", 4, 4, 2);
        inputs.layerOrm = image("orm", "rgba8-unorm", 4, 4, 2);
        inputs.surfaceLocalNormal = image("frame-normal", "rgba32-float", 2, 2);
        inputs.surfaceLocalTangent = image("frame-tangent", "rgba32-float", 2, 2);
        inputs.heightAtlas = image("height-atlas", "r16-unorm", 136, 136);
        inputs.heightLayerMap = image("height-map", "rgba32-float", 2, 1);
        inputs.globalColour = image("global-colour", "rgba8-unorm", 4, 4);
        inputs.globalMasks = image("global-masks", "rgba8-unorm", 4, 4);
        if (mode == VL::TerrainCompositedWeightSource::CoverageAtlas)
        {
            inputs.coveragePageTable = image("coverage-table", "rgba8-unorm", 1, 1);
            inputs.coverageIds = image("coverage-ids", "rgba8-unorm", 128, 128);
            inputs.coverageWeights = image("coverage-weights", "rgba8-unorm", 128, 128);
        }
        else
            inputs.localLayerWeights = image("local-weights", "r32-float", 4, 4, 2);
        production.surfaces.push_back(surface);
        production.adjacency = VL::TerrainCompositedAdjacencyGraph({surface.surfaceId}, {});
        production.Validate();
        return production;
    }

    void RepublishTerrain(const fs::path& manifest, const VL::TerrainCompositedProductionClosure& production)
    {
        const auto store = VL::TerrainCompositedVTStore::Open(manifest);
        VL::TerrainCompositedVTPublication publication;
        publication.production = production;
        publication.surfaces = store.Surfaces();
        for (auto& surface : publication.surfaces)
            surface.dependencyHash = production.SurfaceContentHash(surface.surfaceId);
        std::vector<VL::TerrainCompositedVTPageData> pages;
        for (const auto& entry : store.Pages())
        {
            publication.packagedPages.push_back(entry.first);
            pages.push_back({entry.first, store.ReadPage(entry.first)});
        }
        VL::TerrainCompositedVTStore::Publish(manifest, publication, pages);
    }

    VL::TerrainCompositedProductionClosure AddRawAuthor(
        const fs::path& manifest, bool absent = false)
    {
        auto production = VL::TerrainCompositedVTStore::Open(manifest).Production();
        const std::string path = absent ? "Generated/Author/optional.paint.meta.json" : "Generated/Author/TR_original.json";
        const auto projection = VL::ContentHasher::HashString(
            absent ? "terrain-optional-absent-v1" : "canonical-author-projection").ToHex();
        const std::string bytes = absent
            ? Json{{"root", "resource"}, {"path", path}, {"mode", "absent"}, {"digest", projection}}.dump()
            : "{ \"material\": \"shader/glsl/DO_NOT_COMPILE_RAW_AUTHOR.json\", \"author\": true }\n";
        VL::TerrainCompositedProductionObject raw;
        raw.objectId = absent ? "raw-absence" : "raw-author";
        raw.kind = VL::TerrainCompositedProductionObjectKind::RawAuthorSource;
        raw.bytes = bytes.size();
        raw.digest = VL::ContentHasher::HashString(bytes);
        raw.relativePath = "author-inputs/" + raw.digest.ToHex() + ".bin";
        production.objects.push_back(raw);
        production.surfaces.front().authorDependencies.push_back({
            "resource", path, absent ? "absent" : "terrain-json-without-package-path-v1", projection, {raw.objectId}});
        Write(manifest.parent_path() / raw.relativePath, bytes);
        production.Validate();
        return production;
    }

    fs::path TerrainWorldPackage(VL::TerrainCompositedWeightSource mode)
    {
        const auto manifest = TerrainPackage(mode);
        const auto store = VL::TerrainCompositedVTStore::Open(manifest);
        VL::TerrainCompositedVTPublication publication;
        publication.production = store.Production();
        auto second = publication.production.surfaces.front();
        second.surfaceId = "terrain|Landscape_01";
        second.runtimeMaterialInstancePath = "Generated/MI_runtime_tile01.json";
        second.runtimeDetailMaterialInstancePath = "Generated/MI_runtime_tile01_detail.json";
        for (const auto& name : {second.runtimeMaterialInstancePath, second.runtimeDetailMaterialInstancePath})
            WriteJson(manifest.parent_path() / name,
                {{"type", "materialInstance"}, {"material", "shader/glsl/M_terrain.json"}});
        second.localToWorld[12] = 100;
        publication.production.surfaces.push_back(second);
        publication.production.adjacency = VL::TerrainCompositedAdjacencyGraph(
            {publication.production.surfaces.front().surfaceId, second.surfaceId}, {});
        for (const auto& production : publication.production.surfaces)
        {
            auto surface = store.Surfaces().front();
            surface.surfaceId = production.surfaceId;
            surface.dependencyHash = publication.production.SurfaceContentHash(surface.surfaceId);
            publication.surfaces.push_back(surface);
            publication.packagedPages.push_back({surface.surfaceId, surface.Layout().MipCount() - 1, 0, 0});
        }
        VL::TerrainCompositedVTStore::Publish(manifest, publication,
            VL::TerrainCompositedVTStore::PageProducer([&](const auto&)
            {
                return std::vector<uint8_t>(static_cast<size_t>(publication.surfaces.front().PageBytes()), 127);
            }));
        auto terrain = ReadJson(resources / "Generated/TR_composited.json");
        terrain["compositedVT"]["productionSourcePath"] = "Generated/Author/not-installed.production.json";
        WriteJson(resources / "Generated/TR_composited.json", terrain);
        terrain["compositedVT"]["packagePath"] = "TerrainCook\\tile_00\\manifest.json";
        WriteJson(resources / "Generated/TR_shared.json", terrain);
        Scene(Json::array({
            {{"type", "terrain"}, {"name", "Landscape_00"}, {"terrainPath", "Generated/TR_composited.json"}},
            {{"type", "terrain"}, {"name", "Landscape_01"}, {"terrainPath", "Generated/TR_shared.json"}}}));
        return manifest;
    }

    void EnableManaged()
    {
        auto config = ReadJson(options.projectRoot / "config/config.json");
        config["scripts"] = {{"enabled", true}};
        WriteJson(options.projectRoot / "config/config.json", config);
        for (const auto& name : {"VulkanLearn.Runtime.dll", "VulkanLearn.Runtime.runtimeconfig.json", "Gameplay.dll", "hostfxr.dll"})
            Write(options.binaryDirectory / "managed" / name, "{}");
        options.dotnetRoot = root / "dotnet";
        Write(options.dotnetRoot / "shared/Microsoft.NETCore.App/11.0.20/coreclr.dll", "runtime");
        Write(options.dotnetRoot / "host/fxr/11.0.20/hostfxr.dll", "host");
    }

    Json ShaderArtifact(const std::string& kind, const std::string& normalizedKey,
        const std::vector<std::pair<std::string, std::string>>& outputs,
        const std::vector<std::string>& dependencies,
        const std::vector<std::string>& primarySources)
    {
        Json artifact = {{"kind", kind}, {"normalizedKey", normalizedKey},
            {"sourceFingerprint", normalizedKey}, {"abiFingerprint", normalizedKey},
            {"outputs", Json::object()}, {"dependencies", Json::array()},
            {"primarySources", Json::array()}};
        for (const auto& [role, relative] : outputs)
        {
            const auto path = options.projectRoot / "shader" / relative;
            Write(path, "spirv:" + relative);
            artifact["outputs"][role] = {{"path", relative},
                {"digest", VL::ContentHasher::HashFile(path).ToHex()}};
        }
        for (const auto& relative : dependencies)
        {
            const auto path = options.projectRoot / "shader/glsl" / relative;
            if (relative == "generate/M_glass_crackParamter.glsl")
            {
                const auto generated = MaterialParameterIncludeGenerator::BuildGeneratedIncludeContent(
                    options.projectRoot / "shader/glsl/M_glass_crack.json");
                Write(path, generated.generatedBytes);
            }
            else
                Write(path, "glsl:" + relative);
            artifact["dependencies"].push_back({{"path", relative},
                {"digest", VL::ContentHasher::HashFile(path).ToHex()},
                {"requestingSources", Json::array()}, {"minimumIncludeDepth", 1}});
        }
        for (const auto& identity : primarySources)
        {
            const auto path = options.projectRoot / "shader/glsl" / identity;
            if (!fs::exists(path)) Write(path, "primary:" + identity);
            artifact["primarySources"].push_back({{"identity", identity},
                {"digest", VL::ContentHasher::HashFile(path).ToHex()}});
        }
        return artifact;
    }

    void WriteGameShaderCache()
    {
        Json artifacts = Json::object();
        uint32_t index = 0;
        auto add = [&artifacts, &index](const Json& artifact)
        {
            artifacts[std::to_string(++index)] = artifact;
        };
        for (const auto* shader : {
                 "engine/skinnedDeform.comp",
                 "terrain/terrainPageBake.comp",
                 "terrain/terrainCompositedBakeWorld.comp",
                 "terrain/terrainCompositedBakeWorldPadding.comp",
                 "terrain/terrainCompositedQuality.comp",
                 "pass/autoExposureHistogram.comp",
                 "pass/autoExposureAdapt.comp",
                 "generator/brfdLut.comp",
                 "generator/skyToCubemap.comp",
                 "generator/skySHGenerate.comp",
                 "generator/prefilterEnvMap.comp",
                 "generator/equirectToCubemap.comp",
                 "hair/tressfxSkinning.comp",
                 "hair/tressfxSimCollision.comp",
                 "hair/tressfxSimIntegration.comp",
                 "hair/tressfxSimLengthWind.comp",
                 "hair/tressfxSimLocalShape.comp",
                 "hair/tressfxSimSdfClear.comp",
                 "hair/tressfxSimSdfCollide.comp",
                 "hair/tressfxSimSdfSplat.comp",
                 "hair/tressfxSimStrandLevel.comp",
                 "hair/tressfxSimVsp.comp"})
        {
            const std::string name = shader;
            const std::string stem = std::filesystem::path(name).stem().string();
            const std::string folder = std::filesystem::path(name).parent_path().generic_string();
            std::vector<std::string> sources{name};
            if (name == "terrain/terrainCompositedBakeWorld.comp")
                sources.push_back("materialFunction/terrainCompositedEvaluation.glsl");
            if (name.find("terrain/terrainCompositedBake") == 0)
                sources.push_back("terrain/terrainCompositedBakeRoute.glsl");
            if (name.find("terrain/terrainCompositedBakeWorld") == 0)
            {
                sources.push_back("terrain/terrainCompositedBakeWorldEvaluator.glsl");
                sources.push_back("terrain/terrainCompositedBakeWorldTypes.glsl");
            }
            add(ShaderArtifact("StageEntry",
                "kind=StageEntry|path=" + name + "|stage=compute|macros=|target=Vulkan1.4",
                {{"runtimeCompute", "spv/" + folder + "/" + stem + "_comp.spv"},
                 {"debugCompute", "spv/" + folder + "/" + stem + "_comp.debug"}},
                sources, {name}));
        }
        add(ShaderArtifact("GraphicsPair",
            "kind=GraphicsPair|shaderName=uiOverlay|renderMode=Opaque|shadingModel=SHADING_MODEL_UNLIT|macros=|target=Vulkan1.4",
            {{"runtimeVertex", "spv/uiOverlay.vert.spv"},
             {"runtimeFragment", "spv/uiOverlay.frag.spv"},
             {"debugVertex", "spv/uiOverlay.vert.debug"},
             {"debugFragment", "spv/uiOverlay.frag.debug"}},
            {"uiOverlay.vert", "uiOverlay.frag"}, {"uiOverlay.vert", "uiOverlay.frag"}));
        add(ShaderArtifact("GraphicsPair",
            "kind=GraphicsPair|shaderName=offlineDisplay|renderMode=Opaque|shadingModel=SHADING_MODEL_UNLIT|macros=|target=Vulkan1.4",
            {{"runtimeVertex", "spv/offlineDisplay_vert.spv"},
             {"runtimeFragment", "spv/offlineDisplay_frag.spv"},
             {"debugVertex", "spv/offlineDisplay_vert.debug"},
             {"debugFragment", "spv/offlineDisplay_frag.debug"}},
            {"offlineDisplay.vert", "offlineDisplay.frag"}, {"offlineDisplay.vert", "offlineDisplay.frag"}));
        add(ShaderArtifact("MaterialGraphicsPair",
            "kind=MaterialGraphicsPair|materialSource=M_glass_crack.json|pass=Base|features=writesEveryPixel=1|usesOpacityMask=0|modifiesMeshPosition=0|twoSided=0|vertexFactory=StaticMesh|compileTarget=Vulkan1.4|variant=shaderName=glass_crack|renderMode=Opaque|shadingModel=SHADING_MODEL_UNLIT|macros=|vertexEvaluation=M_glass_crack.vertex.glsl|surfaceEvaluation=M_glass_crack.surface.glsl|parameterInclude=generate/M_glass_crackParamter.glsl",
            {{"runtimeVertex", "spv/material/glass_crack.vert.spv"},
             {"runtimeFragment", "spv/material/glass_crack.frag.spv"},
             {"debugVertex", "spv/material/glass_crack.vert.debug"},
             {"debugFragment", "spv/material/glass_crack.frag.debug"}},
            {"M_glass_crack.vertex.glsl", "M_glass_crack.surface.glsl",
             "common/function.glsl", "generate/M_glass_crackParamter.glsl"},
            {"__composed__/glass_crack.vert", "__composed__/glass_crack.frag"}));
        WriteJson(options.projectRoot / "shader/spv/shader-build-cache.json",
            {{"schemaVersion", 1}, {"hashAlgorithm", "BLAKE3-256"},
             {"compilePolicy", "ShaderCompilePolicyV1;configuration=Release;optimization=performance;runtime=optimized;reflection=debug-info+ENABLE_DEBUG_VIEW;abi=ShaderAbiSignatureV2"},
             {"targetEnvironment", "Vulkan1.4"}, {"artifacts", artifacts}});
    }

    void EnableGameFlavor()
    {
        options.flavor = VL::GameExportFlavor::Game;
        Write(options.binaryDirectory / "main_game.exe", Pe());
        WriteJson(options.binaryDirectory / "vl_build_flavor.json",
            {{"flavor", "game"}, {"editorEnabled", false}});
        Write(options.projectRoot / "ui/runtime-hud.rcss", "body { color: #fff; }");
        WriteJson(options.projectRoot / "ui/localization.json", {{"en-US", Json::object()}});
        WriteJson(options.projectRoot / "ui/runtime-ui.manifest.json",
            {{"format", 1}, {"files", Json::array(
                {"runtime-hud.rml", "runtime-hud.rcss", "localization.json"})}});
        WriteJson(resources / "Common/Materials/MI_glass_crack.json",
            {{"type", "materialInstance"}, {"material", "shader/glsl/M_glass_crack.json"}});
        WriteJson(options.projectRoot / "shader/glsl/M_glass_crack.json",
            {{"name", "M_glass_crack"}, {"type", "material"},
             {"shadingModel", "Unlit"}, {"renderStates", Json::object()}, {"macros", Json::object()},
             {"parameters", Json::object()}, {"textures", Json::object()},
             {"shaderEvaluation", {{"vertex", "M_glass_crack.vertex.glsl"},
                                   {"surface", "M_glass_crack.surface.glsl"}}}});
        Write(options.projectRoot / "shader/glsl/M_glass_crack.vertex.glsl", "void main(){}");
        Write(options.projectRoot / "shader/glsl/M_glass_crack.surface.glsl", "vec3 surface(){}");
        options.mingwRuntimeDirectory = root / "mingw/bin";
        for (const auto* runtime :
            {"libstdc++-6.dll", "libgcc_s_seh-1.dll", "libwinpthread-1.dll"})
            Write(options.mingwRuntimeDirectory / runtime, "mingw runtime");
        WriteGameShaderCache();
    }

    fs::path root;
    fs::path resources;
    VL::GameExportOptions options;
};

TEST_F(GameExportTest, ExportRewritesPortableConfigAndRetainsRuntimeInputs)
{
    const auto result = VL::ExportGame(options);
    const auto config = ReadJson(options.outputDirectory / "config/config.json");
    EXPECT_EQ(config.at("resourcePath"), "resources");
    EXPECT_EQ(config.at("initScene"), options.initialScene);
    EXPECT_FALSE(config.contains("projectPath"));
    EXPECT_FALSE(config.at("ui").at("developerUiEnabled"));
    EXPECT_FALSE(config.at("ui").at("developerUiVisible"));
    EXPECT_FALSE(config.at("ui").at("hotReload"));
    EXPECT_TRUE(fs::is_regular_file(options.outputDirectory / "shader/glsl/test.frag"));
    EXPECT_TRUE(fs::is_regular_file(options.outputDirectory / "shader/spv/test.spv"));
    EXPECT_TRUE(fs::is_regular_file(options.outputDirectory / "ui/runtime-hud.rml"));
    EXPECT_EQ(result.manifest.at("initialScene"), options.initialScene);
}

TEST_F(GameExportTest, GeneratedInitialSceneRetainsMapsDirectoryWithoutUnrelatedMaps)
{
    options.initialScene = "Generated/Validation/SC_generated.json";
    Scene(Json::array());
    const auto result = VL::ExportGame(options);
    EXPECT_TRUE(fs::is_directory(options.outputDirectory / "resources/Maps"));
    EXPECT_TRUE(fs::is_empty(options.outputDirectory / "resources/Maps"));
    EXPECT_TRUE(fs::is_regular_file(options.outputDirectory / "resources" / options.initialScene));
    EXPECT_EQ(result.manifest.at("initialScene"), options.initialScene);
}

TEST_F(GameExportTest, BlueprintSceneAndSpawnDependenciesAreCollectedWithoutUnrelatedMaps)
{
    EnableManaged();
    Scene(Json::array({{{"type", "blueprintInstance"}, {"blueprintPath", "Common/Blueprints/BP_box.json"}}}));
    WriteJson(resources / "Common/Blueprints/BP_box.json",
        {{"components", Json::array({{{"modelPath", "Maps/SC_test/Meshes/SM_box.json"}}})}});
    WriteJson(resources / "Maps/SC_test/Meshes/SM_box.json",
        {{"modelDataPath", "Maps/SC_test/Source/box.gltf"},
         {"materialSlots", Json::array({{{"materialInstancePath", "Common/Materials/MI_box.json"}}})}});
    Write(resources / "Maps/SC_test/Source/box.gltf", "{}");
    Write(resources / "Maps/SC_test/Source/mesh.bin", "external buffer");
    Write(resources / "Maps/SC_test/Source/textures/box.png", "external image");
    WriteJson(resources / "Common/Materials/MI_box.json", {{"material", "shader/glsl/M_box.json"}});
    WriteJson(options.projectRoot / "shader/glsl/M_box.json", {{"textures", Json::object()}});
    WriteJson(resources / "Maps/SC_other/SC_other.json", {{"type", "scene"}});
    VL::ExportGame(options);
    EXPECT_TRUE(fs::is_regular_file(options.outputDirectory / "resources/Common/Blueprints/BP_box.json"));
    EXPECT_TRUE(fs::is_regular_file(options.outputDirectory / "resources/Maps/SC_test/Source/mesh.bin"));
    EXPECT_TRUE(fs::is_regular_file(options.outputDirectory / "resources/Maps/SC_test/Source/textures/box.png"));
    EXPECT_FALSE(fs::exists(options.outputDirectory / "resources/Maps/SC_other"));
}

TEST_F(GameExportTest, SpawnAndDynamicAssetsFollowTheSameRecursiveClosure)
{
    EnableManaged();
    auto scene = ReadJson(resources / options.initialScene);
    scene["spawnAssets"] = {"Common/Blueprints/BP_spawn.json"};
    WriteJson(resources / options.initialScene, scene);
    WriteJson(resources / "Common/Blueprints/BP_spawn.json", {{"class", "Spawn"}});
    WriteJson(resources / "Maps/SC_next/SC_next.json",
        {{"type", "scene"}, {"objects", Json::array({{{"modelPath", "Common/SM_next.json"}}})}});
    WriteJson(resources / "Common/SM_next.json", {{"name", "Next"}});
    options.dynamicAssets = {"Maps/SC_next/SC_next.json"};
    VL::ExportGame(options);
    EXPECT_TRUE(fs::exists(options.outputDirectory / "resources/Common/Blueprints/BP_spawn.json"));
    EXPECT_TRUE(fs::exists(options.outputDirectory / "resources/Common/SM_next.json"));
}

TEST_F(GameExportTest, TressfxPlacementExportsProxyAndWeightsButNotSourceIdentity)
{
    Json object = {{"type", "mesh"}};
    object["tressfx"]["skinning"] = {
        {"source", "CharacterBody"}, {"rootBone", "Head"},
        {"boneWeights", "HairInputs/weights.tfxbone"}};
    object["tressfx"]["hairCollision"] = {
        {"mesh", {{"path", "HairInputs/proxy.obj"}}}};
    Scene(Json::array({object}));
    Write(resources / "HairInputs/weights.tfxbone", "weights");
    Write(resources / "HairInputs/proxy.obj", "proxy");
    EXPECT_NO_THROW(VL::ExportGame(options));
    EXPECT_TRUE(fs::exists(options.outputDirectory / "resources/HairInputs/weights.tfxbone"));
    EXPECT_TRUE(fs::exists(options.outputDirectory / "resources/HairInputs/proxy.obj"));
    EXPECT_FALSE(fs::exists(options.outputDirectory / "resources/CharacterBody"));
}

TEST_F(GameExportTest, GraphMaterialsAndShaderDefaultTexturesAreDependencies)
{
    WriteJson(options.projectRoot / "config/renderGraphConfig.json",
        {{"passes", Json::array({{{"materialInstancePath", "Common/MI_pass.json"}}})}});
    WriteJson(resources / "Common/MI_pass.json", {{"material", "shader/glsl/M_pass.json"}});
    WriteJson(options.projectRoot / "shader/glsl/M_pass.json",
        {{"textures", {{"Albedo", {{"default", "Common/T_albedo.json"}}}}}});
    WriteJson(resources / "Common/T_albedo.json", {{"source", "Common/Source/albedo.png"}});
    Write(resources / "Common/Source/albedo.png", "pixels");
    VL::ExportGame(options);
    EXPECT_TRUE(fs::exists(options.outputDirectory / "resources/Common/MI_pass.json"));
    EXPECT_TRUE(fs::exists(options.outputDirectory / "resources/Common/Source/albedo.png"));
}

TEST_F(GameExportTest, MissingDependencyDoesNotPublishAnOutputOrLeaveStaging)
{
    Scene(Json::array({{{"modelPath", "Common/SM_missing.json"}}}));
    EXPECT_THROW(VL::ExportGame(options), std::invalid_argument);
    EXPECT_FALSE(fs::exists(options.outputDirectory));
    for (const auto& entry : fs::directory_iterator(root))
        EXPECT_EQ(entry.path().filename().string().find("game.staging"), std::string::npos);
}

TEST_F(GameExportTest, TerrainClosureResolvesInputBeyondMaxPath)
{
#ifdef _WIN32
    // VT 输入名是两个 64 位摘要，加深层资源根后真实超�?MAX_PATH（这�?> 300 字符）�?
    // Resolve 必须�?\\?\ 访问，不能把实际存在的输入判成缺失�?
    // 建树/清理只用 Win32，避�?libstdc++ �?fs 实现在超长路径上递归�?
    const auto base = root / "deep";
    fs::path full = base;
    std::string nested;
    for (int index = 0; index < 4; ++index)
    {
        const auto component = std::string(60, static_cast<char>('a' + index));
        full /= component;
        nested += component + "/";
    }
    const auto leaf = std::string(65, 'z') + ".bin";
    full /= leaf;
    nested += leaf;
    ASSERT_GT(full.wstring().size(), 260u);

    std::vector<std::wstring> directories;
    std::wstring current = VL::TerrainPackageClosure::ExtendedPath(base);
    ASSERT_TRUE(CreateDirectoryW(current.c_str(), nullptr) != 0 ||
        GetLastError() == ERROR_ALREADY_EXISTS);
    directories.push_back(current);
    const fs::path relativeNested(nested);
    for (auto iterator = relativeNested.begin(); iterator != relativeNested.end(); ++iterator)
    {
        if (std::next(iterator) == relativeNested.end())
            break;
        current += L"\\" + iterator->wstring();
        if (!CreateDirectoryW(current.c_str(), nullptr) && GetLastError() != ERROR_ALREADY_EXISTS)
            FAIL() << "cannot create nested directory";
        directories.push_back(current);
    }
    const auto extendedFile = VL::TerrainPackageClosure::ExtendedPath(full);
    const HANDLE handle = CreateFileW(extendedFile.c_str(), GENERIC_WRITE, 0, nullptr,
        CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    ASSERT_NE(handle, INVALID_HANDLE_VALUE);
    DWORD written = 0;
    ASSERT_TRUE(WriteFile(handle, "payload", 7, &written, nullptr));
    CloseHandle(handle);

    EXPECT_NO_THROW(VL::TerrainPackageClosure::Resolve(base, nested));
    EXPECT_THROW(VL::TerrainPackageClosure::Resolve(base, nested + "x"), std::runtime_error);

    DeleteFileW(extendedFile.c_str());
    for (auto iterator = directories.rbegin(); iterator != directories.rend(); ++iterator)
        RemoveDirectoryW(iterator->c_str());
#else
    GTEST_SKIP() << "MAX_PATH is a Windows-only constraint";
#endif
}

TEST_F(GameExportTest, TraversalDependencyIsRejectedBeforePublication)
{
    Scene(Json::array({{{"modelPath", "../secret.json"}}}));
    WriteJson(root / "secret.json", {{"private", true}});
    EXPECT_THROW(VL::ExportGame(options), std::invalid_argument);
    EXPECT_FALSE(fs::exists(options.outputDirectory));
}

TEST_F(GameExportTest, ExistingOutputIsNeverOverwritten)
{
    Write(options.outputDirectory / "keep.txt", "preserved");
    EXPECT_THROW(VL::ExportGame(options), std::invalid_argument);
    EXPECT_TRUE(fs::exists(options.outputDirectory / "keep.txt"));
    EXPECT_FALSE(fs::exists(options.outputDirectory / "main.exe"));
}

TEST_F(GameExportTest, OutputCannotRecursivelyContainPackagedInput)
{
    options.outputDirectory = resources / "Generated/game";
    EXPECT_THROW(VL::ExportGame(options), std::invalid_argument);
    EXPECT_FALSE(fs::exists(options.outputDirectory));
}

TEST_F(GameExportTest, CustomUiRootCannotRecursivelyContainExportStaging)
{
    auto config = ReadJson(options.projectRoot / "config/config.json");
    config["ui"]["assetRoot"] = "custom-ui";
    WriteJson(options.projectRoot / "config/config.json", config);
    Write(options.projectRoot / "custom-ui/runtime-hud.rml", "<rml/>");
    options.outputDirectory = options.projectRoot / "custom-ui/game";
    EXPECT_THROW(VL::ExportGame(options), std::invalid_argument);
    EXPECT_FALSE(fs::exists(options.outputDirectory));
}

TEST_F(GameExportTest, ManifestDigestsEveryPublishedPayloadFile)
{
    const auto result = VL::ExportGame(options);
    const auto manifest = ReadJson(options.outputDirectory / "game-export-manifest.json");
    EXPECT_EQ(manifest, result.manifest);
    EXPECT_EQ(manifest.at("hash"), "BLAKE3-256");
    EXPECT_EQ(manifest.at("files").size(), result.fileCount);
    uintmax_t total = 0;
    for (const auto& entry : manifest.at("files"))
    {
        const auto path = options.outputDirectory / entry.at("path").get<std::string>();
        EXPECT_EQ(entry.at("digest"), VL::ContentHasher::HashFile(path).ToHex());
        EXPECT_EQ(entry.at("size"), fs::file_size(path));
        total += fs::file_size(path);
    }
    EXPECT_EQ(total, result.totalBytes);
}

TEST_F(GameExportTest, ManagedExportIncludesActualPrivateRuntimeAndUsesRelativeSettings)
{
    EnableManaged();
    Write(options.dotnetRoot / "shared/Microsoft.NETCore.App/11.0.9/coreclr.dll", "older");
    Write(options.dotnetRoot / "shared/Microsoft.NETCore.App/11.0.20/coreclr.dll", "newer");
    Write(options.dotnetRoot / "host/fxr/11.0.20/hostfxr.dll", "host");
    VL::ExportGame(options);
    EXPECT_TRUE(fs::exists(options.outputDirectory / "managed/dotnet/shared/Microsoft.NETCore.App/11.0.20/coreclr.dll"));
    EXPECT_FALSE(fs::exists(options.outputDirectory / "managed/dotnet/shared/Microsoft.NETCore.App/11.0.9"));
    const auto scripts = ReadJson(options.outputDirectory / "config/config.json").at("scripts");
    EXPECT_TRUE(scripts.at("autoPlay"));
    EXPECT_EQ(scripts.at("runtimeDirectory"), "managed");
    EXPECT_EQ(scripts.at("assembly"), "managed/Gameplay.dll");
}

TEST_F(GameExportTest, BlueprintSceneCannotBeExportedWithDisabledScripts)
{
    Scene(Json::array({{{"type", "blueprintInstance"}, {"class", "Gameplay.Character"}}}));
    EXPECT_THROW(VL::ExportGame(options), std::invalid_argument);
    EXPECT_FALSE(fs::exists(options.outputDirectory));
}

TEST_F(GameExportTest, MissingPrivateRuntimeFailsInsteadOfDependingOnDeveloperSdk)
{
    auto config = ReadJson(options.projectRoot / "config/config.json");
    config["scripts"] = {{"enabled", true}};
    WriteJson(options.projectRoot / "config/config.json", config);
    for (const auto& name : {"VulkanLearn.Runtime.dll", "VulkanLearn.Runtime.runtimeconfig.json", "Gameplay.dll", "hostfxr.dll"})
        Write(options.binaryDirectory / "managed" / name, "{}");
    EXPECT_THROW(VL::ExportGame(options), std::runtime_error);
    EXPECT_FALSE(fs::exists(options.outputDirectory));
}

TEST_F(GameExportTest, DisabledCSharpBuildDoesNotInferScriptsFromStaleManagedFiles)
{
    auto config = ReadJson(options.projectRoot / "config/config.json");
    config.erase("scripts");
    WriteJson(options.projectRoot / "config/config.json", config);
    Write(options.binaryDirectory / "managed/Gameplay.dll", "stale development artifact");
    options.csharpEnabled = false;
    VL::ExportGame(options);
    EXPECT_FALSE(ReadJson(options.outputDirectory / "config/config.json").at("scripts").at("enabled"));
    EXPECT_FALSE(fs::exists(options.outputDirectory / "managed"));
}

TEST_F(GameExportTest, NativeDependencyClosureCopiesTransitiveLibraries)
{
    Write(options.binaryDirectory / "main.exe", Pe("vl_fixture_first.dll"));
    Write(options.binaryDirectory / "vl_fixture_first.dll", Pe("vl_fixture_second.dll"));
    Write(options.binaryDirectory / "vl_fixture_second.dll", Pe("vl_fixture_first.dll"));
    VL::ExportGame(options);
    EXPECT_TRUE(fs::exists(options.outputDirectory / "vl_fixture_first.dll"));
    EXPECT_TRUE(fs::exists(options.outputDirectory / "vl_fixture_second.dll"));
}

TEST_F(GameExportTest, MissingNativeDependencyAbortsPublication)
{
    Write(options.binaryDirectory / "main.exe", Pe("vl_fixture_missing.dll"));
    EXPECT_THROW(VL::ExportGame(options), std::runtime_error);
    EXPECT_FALSE(fs::exists(options.outputDirectory));
}

TEST_F(GameExportTest, NativeDelayImportsAreIncludedInDependencyClosure)
{
    Write(options.binaryDirectory / "main.exe", Pe("vl_fixture_delayed.dll", true));
    Write(options.binaryDirectory / "vl_fixture_delayed.dll", Pe());
    VL::ExportGame(options);
    EXPECT_TRUE(fs::exists(options.outputDirectory / "vl_fixture_delayed.dll"));
}

TEST_F(GameExportTest, TruncatedNativeImageCannotBePublished)
{
    Write(options.binaryDirectory / "main.exe", "MZ");
    EXPECT_THROW(VL::ExportGame(options), std::runtime_error);
    EXPECT_FALSE(fs::exists(options.outputDirectory));
}

TEST_F(GameExportTest, WrongArchitectureNativeImageCannotBePublished)
{
    auto arm = Pe();
    Put(arm, 0x84, 0xaa64, 2);
    Write(options.binaryDirectory / "main.exe", arm);
    EXPECT_THROW(VL::ExportGame(options), std::runtime_error);
    EXPECT_FALSE(fs::exists(options.outputDirectory));
}

TEST_F(GameExportTest, DevFlavorKeepsFullUiTreeAndRecordsFallback)
{
    Write(options.projectRoot / "ui/editor-workspace.rml", "<rml/>");
    Write(options.projectRoot / "ui/widgets/editor-widgets.rcss", "body {}");
    VL::ExportGame(options);
    EXPECT_TRUE(fs::exists(options.outputDirectory / "ui/editor-workspace.rml"));
    EXPECT_TRUE(fs::exists(options.outputDirectory / "ui/widgets/editor-widgets.rcss"));
    const auto manifest = ReadJson(options.outputDirectory / "game-export-manifest.json");
    EXPECT_TRUE(manifest.at("uiFallback"));
    EXPECT_EQ(manifest.at("flavor"), "dev");
}

TEST_F(GameExportTest, GameFlavorRequiresMainGameAndBuildMarker)
{
    options.flavor = VL::GameExportFlavor::Game;
    EXPECT_THROW(VL::ExportGame(options), std::runtime_error);
    EXPECT_FALSE(fs::exists(options.outputDirectory));
}

TEST_F(GameExportTest, GameFlavorRetainsApprovedTerrainFrozenBytesAndReceipt)
{
    EnableGameFlavor();
    const auto result = VL::ExportGame(options);
    EXPECT_EQ(VL::ContentHasher::HashFile(options.projectRoot / "config/terrainRvtP0Frozen.json"),
        VL::ContentHasher::HashFile(options.outputDirectory / "config/terrainRvtP0Frozen.json"));
    EXPECT_TRUE(fs::is_regular_file(options.outputDirectory / "config/terrainRvtP0Frozen.json.sha256"));
    EXPECT_EQ(result.manifest.at("terrainRvtFrozenSha256"),
        "43fba0dd79a60a4a8b740598c7975d743ace9d32215adb35d8cab213e088e0dd");
    EXPECT_NO_THROW(VL::VerifyGamePackage(options.outputDirectory));
}

TEST_F(GameExportTest, GameFlavorRejectsFrozenConfigAndSidecarRelaxedTogether)
{
    EnableGameFlavor();
    WriteJson(options.projectRoot / "config/terrainRvtP0Frozen.json",
        {{"frozen", true}, {"status", "frozen"}, {"totalGpuBytes", UINT64_MAX}});
    Write(options.projectRoot / "config/terrainRvtP0Frozen.json.sha256", std::string(64, '0'));
    EXPECT_THROW(VL::ExportGame(options), std::runtime_error);
    EXPECT_FALSE(fs::exists(options.outputDirectory));
}

TEST_F(GameExportTest, GameFlavorRejectsMissingFrozenConfig)
{
    EnableGameFlavor();
    fs::remove(options.projectRoot / "config/terrainRvtP0Frozen.json");
    EXPECT_THROW(VL::ExportGame(options), std::runtime_error);
    EXPECT_FALSE(fs::exists(options.outputDirectory));
}

TEST_F(GameExportTest, GameFlavorRejectsEditorBuildMarker)
{
    EnableGameFlavor();
    WriteJson(options.binaryDirectory / "vl_build_flavor.json",
        {{"flavor", "editor"}, {"editorEnabled", true}});
    EXPECT_THROW(VL::ExportGame(options), std::runtime_error);
    EXPECT_FALSE(fs::exists(options.outputDirectory));
}

TEST_F(GameExportTest, GameFlavorCopiesOnlyRuntimeUiManifestClosure)
{
    EnableGameFlavor();
    Write(options.projectRoot / "ui/editor-workspace.rml", "<rml/>");
    Write(options.projectRoot / "ui/editor-workspace.rcss", "body {}");
    Write(options.projectRoot / "ui/widgets/editor-widgets.rcss", "body {}");
    Write(options.projectRoot / "ui/icons/play.png", "icon");
    VL::ExportGame(options);
    EXPECT_TRUE(fs::exists(options.outputDirectory / "ui/runtime-hud.rml"));
    EXPECT_TRUE(fs::exists(options.outputDirectory / "ui/runtime-hud.rcss"));
    EXPECT_TRUE(fs::exists(options.outputDirectory / "ui/localization.json"));
    EXPECT_TRUE(fs::exists(options.outputDirectory / "ui/runtime-ui.manifest.json"));
    EXPECT_FALSE(fs::exists(options.outputDirectory / "ui/editor-workspace.rml"));
    EXPECT_FALSE(fs::exists(options.outputDirectory / "ui/widgets"));
    EXPECT_FALSE(fs::exists(options.outputDirectory / "ui/icons"));
    const auto manifest = ReadJson(options.outputDirectory / "game-export-manifest.json");
    EXPECT_FALSE(manifest.at("uiFallback"));
    EXPECT_EQ(manifest.at("flavor"), "game");
    EXPECT_NO_THROW(VL::VerifyGamePackage(options.outputDirectory));
}

TEST_F(GameExportTest, GameFlavorFailsWhenRuntimeUiClosureIsIncomplete)
{
    EnableGameFlavor();
    Write(options.projectRoot / "ui/runtime-hud.rcss", "body { background-image: url(missing.png); }");
    EXPECT_THROW(VL::ExportGame(options), std::runtime_error);
    EXPECT_FALSE(fs::exists(options.outputDirectory));
}

TEST_F(GameExportTest, GameFlavorStripsEditorGridBeforeAssetScan)
{
    EnableGameFlavor();
    WriteJson(options.projectRoot / "config/renderGraphConfig.json",
        {{"resources", Json::array({{{"name", "selectionMask"}},
                                    {{"name", "sceneColor"}}})},
         {"passes", Json::array({{{"name", "editorGrid"}, {"type", "editorGrid"},
                                  {"materialInstancePath", "Common/Materials/Pass/MI_editorGrid.json"}}})}});
    WriteJson(resources / "Common/Materials/Pass/MI_editorGrid.json",
        {{"type", "materialInstance"}, {"material", "shader/glsl/pass/M_editorGrid.json"}});
    WriteJson(options.projectRoot / "shader/glsl/pass/M_editorGrid.json",
        {{"name", "M_editorGrid"}, {"type", "material"}});
    VL::ExportGame(options);
    const auto graph = ReadJson(options.outputDirectory / "config/renderGraphConfig.json");
    EXPECT_TRUE(graph.at("passes").empty());
    EXPECT_FALSE(fs::exists(options.outputDirectory / "resources/Common/Materials/Pass/MI_editorGrid.json"));
    EXPECT_FALSE(fs::exists(options.outputDirectory / "shader/glsl/pass/M_editorGrid.json"));
    EXPECT_NO_THROW(VL::VerifyGamePackage(options.outputDirectory));
}

TEST_F(GameExportTest, GameFlavorRetainsDebugSpirvAndMaterialEvaluations)
{
    EnableGameFlavor();
    Scene(Json::array({{{"type", "glassPane"}, {"name", "glass"}}}));
    VL::ExportGame(options);
    const auto manifest = ReadJson(options.outputDirectory / "shader/runtime-shader.manifest.json");
    bool sawDebugVertex = false;
    bool sawDebugFragment = false;
    for (const auto& [id, artifact] : manifest.at("artifacts").items())
    {
        static_cast<void>(id);
        for (const auto& [role, output] : artifact.at("outputs").items())
        {
            if (role == "debugVertex") sawDebugVertex = true;
            if (role == "debugFragment") sawDebugFragment = true;
            EXPECT_TRUE(fs::is_regular_file(options.outputDirectory / "shader" / output.at("path").get<std::string>()));
            EXPECT_EQ(output.at("digest"), VL::ContentHasher::HashFile(
                options.outputDirectory / "shader" / output.at("path").get<std::string>()).ToHex());
        }
    }
    EXPECT_TRUE(sawDebugVertex);
    EXPECT_TRUE(sawDebugFragment);
    EXPECT_TRUE(fs::exists(options.outputDirectory / "shader/glsl/M_glass_crack.json"));
    EXPECT_TRUE(fs::exists(options.outputDirectory / "shader/glsl/M_glass_crack.vertex.glsl"));
    EXPECT_TRUE(fs::exists(options.outputDirectory / "shader/glsl/M_glass_crack.surface.glsl"));
    EXPECT_TRUE(fs::exists(options.outputDirectory / "shader/glsl/generate/M_glass_crackParamter.glsl"));
    EXPECT_TRUE(fs::exists(options.outputDirectory / "shader/spv/shader-build-cache.json"));
    EXPECT_NO_THROW(VL::VerifyGamePackage(options.outputDirectory));
}

TEST_F(GameExportTest, GameFlavorShadowPassRetainsLocalShadowShader)
{
    EnableGameFlavor();
    WriteJson(options.projectRoot / "config/renderGraphConfig.json",
        {{"passes", Json::array({{{"name", "shadow"}, {"type", "shadow"}}})}});
    auto cache = ReadJson(options.projectRoot / "shader/spv/shader-build-cache.json");
    for (const std::string shaderName : {"shadow", "shadowLocal"})
        cache["artifacts"][shaderName] = ShaderArtifact("GraphicsPair",
            "kind=GraphicsPair|shaderName=" + shaderName +
                "|renderMode=Opaque|shadingModel=SHADING_MODEL_DEFAULT_LIT|macros=|target=Vulkan1.4",
            {{"runtimeVertex", "spv/" + shaderName + ".vert.spv"},
             {"runtimeFragment", "spv/" + shaderName + ".frag.spv"}},
            {shaderName + ".vert", shaderName + ".frag"},
            {shaderName + ".vert", shaderName + ".frag"});
    WriteJson(options.projectRoot / "shader/spv/shader-build-cache.json", cache);
    VL::ExportGame(options);
    EXPECT_TRUE(fs::is_regular_file(options.outputDirectory / "shader/glsl/shadowLocal.vert"));
    EXPECT_TRUE(fs::is_regular_file(options.outputDirectory / "shader/glsl/shadowLocal.frag"));
    EXPECT_TRUE(fs::is_regular_file(options.outputDirectory / "shader/spv/shadowLocal.vert.spv"));
    EXPECT_NO_THROW(VL::VerifyGamePackage(options.outputDirectory));
}

TEST_F(GameExportTest, GameFlavorGameplayDefaultGlassCrackMaterialIsIncluded)
{
    EnableGameFlavor();
    Scene(Json::array({{{"type", "glassPane"}, {"name", "glass"}}}));
    VL::ExportGame(options);
    EXPECT_TRUE(fs::exists(options.outputDirectory / "resources/Common/Materials/MI_glass_crack.json"));
    EXPECT_TRUE(fs::exists(options.outputDirectory / "shader/glsl/M_glass_crack.json"));
}

TEST_F(GameExportTest, GameFlavorWithoutGlassDoesNotRequireItsDefaultMaterialOrShader)
{
    EnableGameFlavor();
    fs::remove(resources / "Common/Materials/MI_glass_crack.json");
    VL::ExportGame(options);
    EXPECT_FALSE(fs::exists(options.outputDirectory / "resources/Common/Materials/MI_glass_crack.json"));
    EXPECT_FALSE(fs::exists(options.outputDirectory / "shader/glsl/M_glass_crack.json"));
    EXPECT_NO_THROW(VL::VerifyGamePackage(options.outputDirectory));
}

TEST_F(GameExportTest, GameFlavorQualityComputeClosurePreventsInstalledMissingEntry)
{
    EnableGameFlavor();
    const auto original = ReadJson(options.projectRoot / "shader/spv/shader-build-cache.json");
    auto changed = original;
    std::string qualityId;
    for (const auto& item : original["artifacts"].items())
        if (item.value()["normalizedKey"].get<std::string>().find(
            "path=terrain/terrainCompositedQuality.comp|") != std::string::npos)
            qualityId = item.key();
    ASSERT_FALSE(qualityId.empty());
    changed["artifacts"].erase(qualityId);
    WriteJson(options.projectRoot / "shader/spv/shader-build-cache.json", changed);
    EXPECT_THROW(VL::ExportGame(options), std::runtime_error);
    EXPECT_FALSE(fs::exists(options.outputDirectory));
    changed = original;
    changed["artifacts"][qualityId]["outputs"].erase("debugCompute");
    WriteJson(options.projectRoot / "shader/spv/shader-build-cache.json", changed);
    EXPECT_THROW(VL::ExportGame(options), std::runtime_error);
    WriteJson(options.projectRoot / "shader/spv/shader-build-cache.json", original);
    VL::ExportGame(options);
    const auto manifest = ReadJson(options.outputDirectory / "shader/runtime-shader.manifest.json");
    ASSERT_TRUE(manifest["artifacts"].contains(qualityId));
    EXPECT_TRUE(fs::is_regular_file(options.outputDirectory /
        "shader/glsl/terrain/terrainCompositedQuality.comp"));
    for (const auto& item : original["artifacts"][qualityId]["outputs"])
        EXPECT_EQ(VL::ContentHasher::HashFile(options.outputDirectory / "shader" /
            item["path"].get<std::string>()).ToHex(), item["digest"].get<std::string>());
    EXPECT_NO_THROW(VL::VerifyGamePackage(options.outputDirectory));
    fs::remove(options.outputDirectory / "shader/glsl/terrain/terrainCompositedQuality.comp");
    EXPECT_THROW(VL::VerifyGamePackage(options.outputDirectory), std::invalid_argument);
}

TEST_F(GameExportTest, GameFlavorPreinstallsGeneratedIncludeEvenWithoutCacheDependency)
{
    EnableGameFlavor();
    TerrainPackage();
    const auto include = fs::path("shader/glsl/generate/M_terrainParamter.glsl");
    const auto expected = VL::ContentHasher::HashFile(options.projectRoot / include);
    VL::ExportGame(options);
    EXPECT_EQ(VL::ContentHasher::HashFile(options.outputDirectory / include), expected);
    EXPECT_NO_THROW(VL::VerifyGamePackage(options.outputDirectory));
    const auto candidate = MaterialParameterIncludeGenerator::BuildGeneratedIncludeContent(
        options.outputDirectory / "shader/glsl/M_terrain.json");
    EXPECT_EQ(VL::ContentHasher::HashString(candidate.generatedBytes), expected);
    fs::remove(options.outputDirectory / include);
    EXPECT_THROW(VL::VerifyGamePackage(options.outputDirectory), std::invalid_argument);
    Write(options.projectRoot / include, "stale generated include not recorded by shader dependencies");
    options.outputDirectory = root / "stale-game";
    EXPECT_THROW(VL::ExportGame(options), std::runtime_error);
    EXPECT_FALSE(fs::exists(options.outputDirectory));
}

TEST_F(GameExportTest, GameFlavorRetainsImplicitRuntimePipelineClosure)
{
    EnableGameFlavor();
    auto cache = ReadJson(options.projectRoot / "shader/spv/shader-build-cache.json");
    for (const std::string shaderPath : {
        "pass/volumetricFog.comp", "pass/lightCulling.comp", "particle/particleSimulate.comp"})
        cache["artifacts"][shaderPath] = ShaderArtifact("StageEntry",
            "kind=StageEntry|path=" + shaderPath + "|stage=compute|macros=|target=Vulkan1.4",
            {{"runtimeCompute", "spv/" + shaderPath + ".spv"}}, {shaderPath}, {shaderPath});
    for (const std::string shaderName : {"pass/fogComposite", "particle/particle", "decal/decal"})
        cache["artifacts"][shaderName] = ShaderArtifact("GraphicsPair",
            "kind=GraphicsPair|shaderName=" + shaderName +
                "|renderMode=Opaque|shadingModel=SHADING_MODEL_DEFAULT_LIT|macros=|target=Vulkan1.4",
            {{"runtimeVertex", "spv/" + shaderName + ".vert.spv"},
             {"runtimeFragment", "spv/" + shaderName + ".frag.spv"}},
            {shaderName + ".vert", shaderName + ".frag"},
            {shaderName + ".vert", shaderName + ".frag"});
    WriteJson(options.projectRoot / "shader/spv/shader-build-cache.json", cache);
    VL::ExportGame(options);
    EXPECT_TRUE(fs::is_regular_file(options.outputDirectory / "shader/glsl/pass/volumetricFog.comp"));
    EXPECT_TRUE(fs::is_regular_file(options.outputDirectory / "shader/glsl/pass/lightCulling.comp"));
    EXPECT_TRUE(fs::is_regular_file(options.outputDirectory / "shader/glsl/particle/particleSimulate.comp"));
    EXPECT_TRUE(fs::is_regular_file(options.outputDirectory / "shader/glsl/particle/particle.vert"));
    EXPECT_TRUE(fs::is_regular_file(options.outputDirectory / "shader/glsl/decal/decal.frag"));
    EXPECT_NO_THROW(VL::VerifyGamePackage(options.outputDirectory));
}

TEST_F(GameExportTest, GameFlavorRetainsRuntimeTerrainProducerSourcesIncludesAndArtifacts)
{
    EnableGameFlavor();
    auto cache = ReadJson(options.projectRoot / "shader/spv/shader-build-cache.json");
    for (auto& artifact : cache["artifacts"])
        if (artifact.at("normalizedKey").get<std::string>().find("path=terrain/terrainCompositedBakeWorld.comp|") !=
            std::string::npos)
            artifact = ShaderArtifact("StageEntry",
                "kind=StageEntry|path=terrain/terrainCompositedBakeWorld.comp|stage=compute|macros=|target=Vulkan1.4",
                {{"runtimeCompute", "spv/terrain/terrainCompositedBakeWorld_comp.spv"},
                 {"debugCompute", "spv/terrain/terrainCompositedBakeWorld_comp.debug"}},
                {"terrain/terrainCompositedBakeWorld.comp", "materialFunction/terrainCompositedEvaluation.glsl",
                 "terrain/terrainCompositedBakeRoute.glsl", "terrain/terrainCompositedBakeWorldTypes.glsl",
                 "terrain/terrainCompositedBakeWorldEvaluator.glsl"},
                {"terrain/terrainCompositedBakeWorld.comp"});
    WriteJson(options.projectRoot / "shader/spv/shader-build-cache.json", cache);
    VL::ExportGame(options);
    for (const auto* name : {
             "shader/glsl/terrain/terrainCompositedBakeWorld.comp",
             "shader/glsl/terrain/terrainCompositedBakeWorldPadding.comp",
             "shader/glsl/terrain/terrainCompositedBakeWorldEvaluator.glsl",
             "shader/glsl/terrain/terrainCompositedBakeWorldTypes.glsl",
             "shader/glsl/terrain/terrainCompositedBakeRoute.glsl",
             "shader/spv/terrain/terrainCompositedBakeWorld_comp.spv",
             "shader/spv/terrain/terrainCompositedBakeWorldPadding_comp.spv",
             "shader/glsl/materialFunction/terrainCompositedEvaluation.glsl",
             "shader/spv/terrain/terrainCompositedBakeWorld_comp.debug"})
        EXPECT_TRUE(fs::is_regular_file(options.outputDirectory / name));
    EXPECT_NO_THROW(VL::VerifyGamePackage(options.outputDirectory));
}

TEST_F(GameExportTest, GameFlavorRejectsMissingRuntimeTerrainProducerArtifact)
{
    EnableGameFlavor();
    auto cache = ReadJson(options.projectRoot / "shader/spv/shader-build-cache.json");
    auto& artifacts = cache["artifacts"];
    for (auto iterator = artifacts.begin(); iterator != artifacts.end();)
        if (iterator->at("normalizedKey").get<std::string>().find("path=terrain/terrainCompositedBakeWorld.comp|") !=
            std::string::npos)
            iterator = artifacts.erase(iterator);
        else
            ++iterator;
    WriteJson(options.projectRoot / "shader/spv/shader-build-cache.json", cache);
    EXPECT_THROW(VL::ExportGame(options), std::runtime_error);
    EXPECT_FALSE(fs::exists(options.outputDirectory));
}

TEST_F(GameExportTest, GameFlavorDoesNotRequireOrShipRetiredTerrainComputeStages)
{
    EnableGameFlavor();
    auto cache = ReadJson(options.projectRoot / "shader/spv/shader-build-cache.json");
    for (const auto* name : {"terrainCompositedBake", "terrainCompositedBakeAddress",
             "terrainCompositedBakeBoundsInit", "terrainCompositedBakeBoundsPrepare", "terrainCompositedBakeEncode"})
    {
        const std::string path = "terrain/" + std::string(name) + ".comp";
        cache["artifacts"][name] = ShaderArtifact("StageEntry",
            "kind=StageEntry|path=" + path + "|stage=compute|macros=|target=Vulkan1.4",
            {{"runtimeCompute", "spv/terrain/" + std::string(name) + ".spv"}}, {path}, {path});
    }
    WriteJson(options.projectRoot / "shader/spv/shader-build-cache.json", cache);
    TerrainPackage();
    VL::ExportGame(options);
    for (const auto* name : {"terrainCompositedBake", "terrainCompositedBakeAddress",
             "terrainCompositedBakeBoundsInit", "terrainCompositedBakeBoundsPrepare", "terrainCompositedBakeEncode"})
    {
        EXPECT_FALSE(fs::exists(options.outputDirectory / "shader/glsl/terrain" / (std::string(name) + ".comp")));
        EXPECT_FALSE(fs::exists(options.outputDirectory / "shader/spv/terrain" / (std::string(name) + ".spv")));
    }
    EXPECT_NO_THROW(VL::VerifyGamePackage(options.outputDirectory));
}

TEST_F(GameExportTest, GameFlavorRejectsEachMissingGen3ProducerStage)
{
    EnableGameFlavor();
    const auto original = ReadJson(options.projectRoot / "shader/spv/shader-build-cache.json");
    for (const auto* path : {"terrain/terrainCompositedBakeWorld.comp",
             "terrain/terrainCompositedBakeWorldPadding.comp"})
    {
        auto changed = original;
        auto& artifacts = changed["artifacts"];
        for (auto iterator = artifacts.begin(); iterator != artifacts.end();)
            if (iterator->at("normalizedKey").get<std::string>().find(
                    std::string("path=") + path + "|") != std::string::npos)
                iterator = artifacts.erase(iterator);
            else
                ++iterator;
        WriteJson(options.projectRoot / "shader/spv/shader-build-cache.json", changed);
        EXPECT_THROW(VL::ExportGame(options), std::runtime_error) << path;
        EXPECT_FALSE(fs::exists(options.outputDirectory));
    }
}

TEST_F(GameExportTest, TerrainGen3RouteAndEveryCompiledStageMustMatchGeneratorClosure)
{
    EnableGameFlavor();
    const auto manifest = TerrainPackage();
    const auto original = VL::TerrainCompositedVTStore::Open(manifest).Production();
    for (const auto& id : original.generator.compiledArtifacts)
    {
        auto changed = original;
        auto& artifacts = changed.generator.compiledArtifacts;
        artifacts.erase(std::remove(artifacts.begin(), artifacts.end(), id), artifacts.end());
        RepublishTerrain(manifest, changed);
        EXPECT_THROW(VL::ExportGame(options), std::runtime_error) << id;
        EXPECT_FALSE(fs::exists(options.outputDirectory));
    }
    for (const auto* name : {"terrain/terrainCompositedBakeRoute.glsl",
             "terrain/terrainCompositedBakeWorldEvaluator.glsl", "terrain/terrainCompositedBakeWorldTypes.glsl"})
    {
        auto changed = original;
        const auto digest = VL::ContentHasher::HashFile(options.projectRoot / "shader/glsl" / name);
        auto& includes = changed.generator.includeSources;
        includes.erase(std::remove_if(includes.begin(), includes.end(),
            [&](const auto& id) { return changed.Object(id).digest == digest; }), includes.end());
        RepublishTerrain(manifest, changed);
        EXPECT_THROW(VL::ExportGame(options), std::runtime_error) << name;
        EXPECT_FALSE(fs::exists(options.outputDirectory));
    }
}

TEST_F(GameExportTest, TerrainWorldCompilerSnapshotUsesGlslIdentitiesAndDistinctPipelineOutputs)
{
    EnableGameFlavor();
    const auto cachePath = options.projectRoot / "shader/spv/shader-build-cache.json";
    const auto cache = ReadJson(cachePath);
    VL::ShaderBuildManifestStore compilerStore;
    compilerStore.Initialize(options.projectRoot / "shader", cache.at("compilePolicy").get<std::string>());
    const auto snapshot = compilerStore.CaptureSnapshot();
    std::set<std::string> stages;
    std::set<std::string> outputs;
    for (const auto& entry : snapshot.artifacts)
    {
        const auto& record = entry.second;
        if (record.normalizedKey.find("path=terrain/terrainCompositedBakeWorld") == std::string::npos)
            continue;
        ASSERT_EQ(record.primarySources.size(), 1);
        const auto& primary = record.primarySources.front();
        EXPECT_EQ(primary.identity.rfind("terrain/", 0), 0);
        EXPECT_TRUE(fs::is_regular_file(compilerStore.GetGlslRoot() / primary.identity));
        EXPECT_FALSE(fs::exists(compilerStore.GetShaderRoot() / primary.identity));
        EXPECT_EQ(VL::ContentHasher::HashFile(compilerStore.GetGlslRoot() / primary.identity).ToHex(), primary.digest);
        stages.insert(primary.identity);
        for (const auto& dependency : record.dependencies)
            EXPECT_EQ(VL::ContentHasher::HashFile(compilerStore.GetGlslRoot() / dependency.path).ToHex(),
                dependency.digest);
        for (const auto& output : record.outputs)
        {
            EXPECT_EQ(output.second.path.rfind("spv/", 0), 0);
            EXPECT_TRUE(outputs.insert(output.second.path).second);
            EXPECT_EQ(VL::ContentHasher::HashFile(compilerStore.GetShaderRoot() / output.second.path).ToHex(),
                output.second.digest);
        }
    }
    EXPECT_EQ(stages, (std::set<std::string>{
        "terrain/terrainCompositedBakeWorld.comp", "terrain/terrainCompositedBakeWorldPadding.comp"}));
    EXPECT_EQ(outputs.size(), 4);
    TerrainPackage();
    EXPECT_NO_THROW(VL::ExportGame(options));
    EXPECT_NO_THROW(VL::VerifyGamePackage(options.outputDirectory));
}

TEST_F(GameExportTest, TerrainWorldPaddingPrimaryAndOutputDriftAreRejectedIndependently)
{
    EnableGameFlavor();
    TerrainPackage();
    const auto cachePath = options.projectRoot / "shader/spv/shader-build-cache.json";
    const auto original = ReadJson(cachePath);
    for (const auto* fault : {"primary-identity", "primary-digest", "runtime-output", "debug-output"})
    {
        auto cache = original;
        for (auto& record : cache["artifacts"])
        {
            if (record.at("normalizedKey").get<std::string>().find(
                    "path=terrain/terrainCompositedBakeWorldPadding.comp|") == std::string::npos)
                continue;
            if (std::string(fault) == "primary-identity")
                record["primarySources"][0]["identity"] = "glsl/terrain/terrainCompositedBakeWorldPadding.comp";
            else if (std::string(fault) == "primary-digest")
                record["primarySources"][0]["digest"] = VL::ContentHasher::HashString("changed-padding-primary").ToHex();
            else
                record["outputs"][std::string(fault) == "runtime-output" ? "runtimeCompute" : "debugCompute"]["digest"] =
                    VL::ContentHasher::HashString("changed-padding-output").ToHex();
        }
        WriteJson(cachePath, cache);
        EXPECT_THROW(VL::ExportGame(options), std::runtime_error) << fault;
        EXPECT_FALSE(fs::exists(options.outputDirectory));
    }
}

TEST_F(GameExportTest, GameFlavorFailsWithoutShaderBuildCache)
{
    EnableGameFlavor();
    fs::remove(options.projectRoot / "shader/spv/shader-build-cache.json");
    EXPECT_THROW(VL::ExportGame(options), std::runtime_error);
    EXPECT_FALSE(fs::exists(options.outputDirectory));
}

TEST_F(GameExportTest, GameFlavorRejectsDebugCompilePolicy)
{
    EnableGameFlavor();
    auto cache = ReadJson(options.projectRoot / "shader/spv/shader-build-cache.json");
    cache["compilePolicy"] =
        "ShaderCompilePolicyV1;configuration=Debug;optimization=none;runtime=debug;reflection=debug-info+ENABLE_DEBUG_VIEW;abi=ShaderAbiSignatureV2";
    WriteJson(options.projectRoot / "shader/spv/shader-build-cache.json", cache);
    EXPECT_THROW(VL::ExportGame(options), std::runtime_error);
    EXPECT_FALSE(fs::exists(options.outputDirectory));
}

TEST_F(GameExportTest, VerifyGamePackageRejectsEditorContentLeak)
{
    EnableGameFlavor();
    VL::ExportGame(options);
    Write(options.outputDirectory / "ui/editor-workspace.rml", "<rml/>");
    EXPECT_THROW(VL::VerifyGamePackage(options.outputDirectory), std::runtime_error);
}

TEST_F(GameExportTest, TerrainProductionEnumeratesBothWeightModesAndGeneratorClosure)
{
    for (const auto mode : {VL::TerrainCompositedWeightSource::CoverageAtlas,
             VL::TerrainCompositedWeightSource::LocalLayerArray})
    {
        const auto production = TerrainProduction(mode);
        const auto files = VL::TerrainPackageClosure::ProductionFiles(resources, production);
        EXPECT_EQ(files.size(), production.objects.size());
        std::set<fs::path> paths(files.begin(), files.end());
        EXPECT_EQ(paths.size(), files.size());
        for (const auto& object : production.objects)
            EXPECT_EQ(paths.count(resources / object.relativePath), 1);
        EXPECT_EQ(paths.count(resources / production.Object(production.generator.computeSource).relativePath), 1);
        EXPECT_EQ(paths.count(resources / production.Object(production.generator.includeSources.front()).relativePath), 1);
        EXPECT_EQ(paths.count(resources / production.Object(production.generator.compiledArtifacts.front()).relativePath), 1);
    }
}

TEST_F(GameExportTest, TerrainProductionRejectsMissingGeneratorAndWeightInputs)
{
    for (const auto mode : {VL::TerrainCompositedWeightSource::CoverageAtlas,
             VL::TerrainCompositedWeightSource::LocalLayerArray})
    {
        const auto production = TerrainProduction(mode);
        for (const auto& object : production.objects)
        {
            auto changed = production;
            changed.objects.erase(std::remove_if(changed.objects.begin(), changed.objects.end(),
                [&](const auto& candidate) { return candidate.objectId == object.objectId; }), changed.objects.end());
            EXPECT_THROW(VL::TerrainPackageClosure::ProductionFiles(resources, changed), std::runtime_error);
        }
        const auto source = resources / production.Object(production.generator.computeSource).relativePath;
        fs::remove(source);
        EXPECT_THROW(VL::TerrainPackageClosure::ProductionFiles(resources, production), std::runtime_error);
    }
}

TEST_F(GameExportTest, TerrainProductionRejectsTraversalAndInvalidLocalGlobalMapping)
{
    const auto production = TerrainProduction(VL::TerrainCompositedWeightSource::LocalLayerArray);
    auto changed = production;
    changed.objects.front().relativePath = "../escape";
    EXPECT_THROW(VL::TerrainPackageClosure::ProductionFiles(resources, changed), std::runtime_error);
    changed = production;
    changed.surfaces.front().globalToSourceSlice[7] = 2;
    EXPECT_THROW(VL::TerrainPackageClosure::ProductionFiles(resources, changed), std::runtime_error);
}

TEST_F(GameExportTest, TerrainCompositedExportsManifestAndContentAddressedBinary)
{
    EnableGameFlavor();
    const auto manifest = TerrainPackage();
    const auto document = ReadJson(manifest);
    const auto relative = fs::path("resources/TerrainCook/tile_00") / document.at("package").get<std::string>();
    const auto result = VL::ExportGame(options);
    EXPECT_EQ(VL::ContentHasher::HashFile(options.outputDirectory / relative).ToHex(),
        document.at("packageHash").get<std::string>());
    EXPECT_NO_THROW(VL::VerifyGamePackage(options.outputDirectory));
    bool listed = false;
    for (const auto& file : result.manifest.at("files"))
        if (file.at("path") == relative.generic_string()) listed = true;
    EXPECT_TRUE(listed);
}

TEST_F(GameExportTest, TerrainRuntimeStripPreservesGeometryAndUsesOnlyArchivedMaterials)
{
    EnableGameFlavor();
    const auto manifest = TerrainPackage();
    auto terrain = ReadJson(resources / "Generated/TR_composited.json");
    terrain["paintPath"] = "Generated/Author/missing.vlpaint";
    terrain["layers"] = Json::array({{{"name", "test"}, {"heightRange", {0, 1}},
        {"slopeRange", {0, 90}}}});
    terrain["compositedVT"]["productionSourcePath"] = "Generated/Author/missing.recipe.json";
    WriteJson(resources / "Generated/TR_composited.json", terrain);
    const auto heightHash = VL::ContentHasher::HashFile(resources / terrain["heightmapPath"].get<std::string>());
    VL::ExportGame(options);
    const auto runtime = ReadJson(options.outputDirectory / "resources/Generated/TR_composited.json");
    EXPECT_FALSE(runtime.contains("paintPath"));
    EXPECT_FALSE(runtime["compositedVT"].contains("productionSourcePath"));
    EXPECT_EQ(runtime["layers"], terrain["layers"]);
    EXPECT_EQ(runtime["size"], terrain["size"]);
    EXPECT_EQ(runtime["gridResolution"], terrain["gridResolution"]);
    const auto production = VL::TerrainCompositedVTStore::Open(manifest).Production();
    EXPECT_EQ(runtime["materialInstancePath"],
        (manifest.lexically_relative(resources).parent_path() /
            production.surfaces.front().runtimeMaterialInstancePath).generic_string());
    EXPECT_EQ(runtime["detailMaterialInstancePath"],
        (manifest.lexically_relative(resources).parent_path() /
            production.surfaces.front().runtimeDetailMaterialInstancePath).generic_string());
    EXPECT_EQ(VL::ContentHasher::HashFile(options.outputDirectory / "resources" /
        runtime["heightmapPath"].get<std::string>()), heightHash);
    EXPECT_FALSE(fs::exists(options.outputDirectory / "resources/Generated/Author"));
    EXPECT_NO_THROW(VL::VerifyGamePackage(options.outputDirectory));
}

TEST_F(GameExportTest, TerrainSharedAssetGetsPlacementMaterialsWithoutChangingImplicitLayerKeys)
{
    EnableGameFlavor();
    const auto manifest = TerrainWorldPackage(VL::TerrainCompositedWeightSource::CoverageAtlas);
    auto scene = ReadJson(resources / options.initialScene);
    scene["objects"][1]["terrainPath"] = scene["objects"][0]["terrainPath"];
    WriteJson(resources / options.initialScene, scene);
    auto terrain = ReadJson(resources / "Generated/TR_composited.json");
    terrain["layers"] = Json::array({{{"name", "test"}, {"heightRange", {0, 1}},
        {"slopeRange", {0, 90}}}});
    WriteJson(resources / "Generated/TR_composited.json", terrain);
    VL::ExportGame(options);
    const auto installed = ReadJson(options.outputDirectory / "resources" / options.initialScene);
    const auto firstPath = installed["objects"][0]["terrainPath"].get<std::string>();
    const auto secondPath = installed["objects"][1]["terrainPath"].get<std::string>();
    EXPECT_NE(firstPath, secondPath);
    const auto first = ReadJson(options.outputDirectory / "resources" / firstPath);
    const auto second = ReadJson(options.outputDirectory / "resources" / secondPath);
    EXPECT_NE(first["materialInstancePath"], second["materialInstancePath"]);
    EXPECT_EQ(second["layers"][0]["key"], "Generated/TR_composited.json#test");
    EXPECT_FALSE(first["layers"][0].contains("key"));
    EXPECT_EQ(first["heightmapPath"], second["heightmapPath"]);
    EXPECT_EQ(first["size"], second["size"]);
    EXPECT_EQ(VL::TerrainCompositedVTStore::Open(manifest).Surfaces().size(), 2u);
    EXPECT_NO_THROW(VL::VerifyGamePackage(options.outputDirectory));
}

TEST_F(GameExportTest, TerrainRuntimeStripRejectsUnpublishedPlacementAndInvalidNativeTr)
{
    EnableGameFlavor();
    TerrainPackage();
    const auto scene = ReadJson(resources / options.initialScene);
    auto changed = scene;
    changed["objects"][0]["name"] = "Unpublished";
    WriteJson(resources / options.initialScene, changed);
    EXPECT_THROW(VL::ExportGame(options), std::runtime_error);
    WriteJson(resources / options.initialScene, scene);
    auto terrain = ReadJson(resources / "Generated/TR_composited.json");
    terrain["heightmapPath"] = "";
    WriteJson(resources / "Generated/TR_composited.json", terrain);
    EXPECT_THROW(VL::ExportGame(options), std::invalid_argument);
    EXPECT_FALSE(fs::exists(options.outputDirectory));
}

TEST_F(GameExportTest, TerrainClosurePayloadUsesManifestRelativePathWithRootedReader)
{
    const auto manifest = TerrainPackage();
    const auto closure = VL::TerrainPackageClosure::Open(resources,
        manifest.lexically_relative(resources).generic_string());
    const auto document = ReadJson(manifest);
    const auto expected = VL::TerrainPackageClosure::Resolve(manifest.parent_path(),
        document.at("package").get<std::string>());
    EXPECT_EQ(std::count(closure.files.begin(), closure.files.end(), expected), 1);
    for (const auto& file : closure.files)
    {
        const auto relative = file.lexically_relative(fs::canonical(resources));
        EXPECT_FALSE(relative.empty());
        EXPECT_FALSE(relative.is_absolute());
        EXPECT_NO_THROW(VL::TerrainPackageClosure::Relative(relative.generic_string()));
    }
}

TEST_F(GameExportTest, TerrainCompositedRelocatesLongCookPathsWithoutChangingPayload)
{
    EnableGameFlavor();
    const auto manifest = TerrainPackage();
    const auto original = ReadJson(manifest);
    // 导出器在 staging 路径超过 259 字符时把包重定位到短目录。staging = output + ".staging.N"�?
    // �?output 更长；按本机临时目录实际长度补齐，使最深文件的 output 路径达到 260�?
    // 而源路径（assets �?game/resources �?8 字符）仍低于 260，无需系统开启长路径�?
    size_t deepest = 0;
    for (const auto& entry : fs::recursive_directory_iterator(manifest.parent_path()))
        deepest = std::max(deepest, entry.path().lexically_relative(manifest.parent_path()).wstring().size());
    const size_t base = (options.outputDirectory / "resources" / "TerrainCook").wstring().size() + 2 + deepest;
    const size_t padding = base < 260 ? std::max<size_t>(1, 260 - base) : 1;
    const auto longerDirectory = resources / "TerrainCook" / std::string(padding, 'p');
    fs::rename(manifest.parent_path(), longerDirectory);
    const auto manifestName = (longerDirectory / "manifest.json").lexically_relative(resources).generic_string();
    auto terrain = ReadJson(resources / "Generated/TR_composited.json");
    terrain["compositedVT"]["packagePath"] = manifestName;
    WriteJson(resources / "Generated/TR_composited.json", terrain);
    VL::ExportGame(options);
    const auto exportedTerrain = ReadJson(options.outputDirectory / "resources/Generated/TR_composited.json");
    const auto relocated = exportedTerrain.at("compositedVT").at("packagePath").get<std::string>();
    EXPECT_NE(relocated, manifestName);
    const auto exportedManifest = options.outputDirectory / "resources" / relocated;
    EXPECT_EQ(ReadJson(exportedManifest), original);
    EXPECT_EQ(VL::ContentHasher::HashFile(exportedManifest.parent_path() /
        original.at("package").get<std::string>()).ToHex(), original.at("packageHash"));
    EXPECT_NO_THROW(VL::VerifyGamePackage(options.outputDirectory));
}

TEST_F(GameExportTest, TerrainCompositedProductionV2MetadataSurvivesExport)
{
    EnableGameFlavor();
    const auto manifest = TerrainPackage();
    const auto production = ReadJson(manifest);
    VL::ExportGame(options);
    const auto packaged = ReadJson(options.outputDirectory / "resources" /
        manifest.lexically_relative(resources));
    EXPECT_EQ(packaged, production);
    EXPECT_EQ(packaged.at("version"), 2);
    EXPECT_EQ(packaged.at("filterRule"), "mip0-center-coarse-area-local-normal-rvt-adjacency-padding-v3");
    EXPECT_EQ(packaged.at("surfaces")[0].at("generatorVersion"), 3);
    EXPECT_EQ(packaged.at("surfaces")[0].at("encoding"), "gpu-rvt-adjacency-area4-rgba8-v3");
    EXPECT_NO_THROW(VL::VerifyGamePackage(options.outputDirectory));
}

TEST_F(GameExportTest, TerrainCompositedSparsePackageDoesNotRequireFinePages)
{
    EnableGameFlavor();
    const auto manifest = TerrainPackage();
    const auto generic = ReadJson(manifest);
    ASSERT_EQ(generic.at("pages").size(), 1);
    ASSERT_EQ(generic.at("roots").size(), 1);
    ASSERT_EQ(generic.at("surfaces")[0].at("width"), 1024);
    ASSERT_EQ(generic.at("filterRule"), "mip0-center-coarse-area-local-normal-rvt-adjacency-padding-v3");
    EXPECT_NO_THROW(VL::ExportGame(options));
    EXPECT_NO_THROW(VL::VerifyGamePackage(options.outputDirectory));
}

TEST_F(GameExportTest, TerrainCompositedRejectsV1WithoutFallback)
{
    EnableGameFlavor();
    const auto path = TerrainPackage();
    auto legacy = ReadJson(path);
    legacy["version"] = 1;
    legacy.erase("production");
    legacy.erase("roots");
    WriteJson(path, legacy);
    EXPECT_THROW(VL::ExportGame(options), std::runtime_error);
    EXPECT_FALSE(fs::exists(options.outputDirectory));
}

TEST_F(GameExportTest, TerrainCompositedVerifyRejectsV1AfterInventoryRehash)
{
    EnableGameFlavor();
    const auto source = TerrainPackage();
    VL::ExportGame(options);
    const auto relative = fs::path("resources") / source.lexically_relative(resources);
    const auto packaged = options.outputDirectory / relative;
    auto legacy = ReadJson(packaged);
    legacy["version"] = 1;
    legacy.erase("production");
    legacy.erase("roots");
    WriteJson(packaged, legacy);
    auto inventory = ReadJson(options.outputDirectory / "game-export-manifest.json");
    bool updated = false;
    for (auto& file : inventory["files"])
    {
        if (file.at("path") != relative.generic_string()) continue;
        file["size"] = fs::file_size(packaged);
        file["digest"] = VL::ContentHasher::HashFile(packaged).ToHex();
        updated = true;
    }
    ASSERT_TRUE(updated);
    WriteJson(options.outputDirectory / "game-export-manifest.json", inventory);
    EXPECT_THROW(VL::VerifyGamePackage(options.outputDirectory), std::runtime_error);
}

TEST_F(GameExportTest, TerrainCompositedRejectsUnsupportedVersionBeforePublishing)
{
    const auto path = TerrainPackage();
    auto manifest = ReadJson(path);
    manifest["version"] = 99;
    WriteJson(path, manifest);
    EXPECT_THROW(VL::ExportGame(options), std::runtime_error);
    EXPECT_FALSE(fs::exists(options.outputDirectory));
}

TEST_F(GameExportTest, TerrainCompositedRejectsTraversalAndAbsolutePackageReferences)
{
    const auto path = TerrainPackage();
    for (const auto& name : {"../outside.vlcvt", "/outside.vlcvt", "C:/outside.vlcvt", "sub/page.vlcvt"})
    {
        auto manifest = ReadJson(path);
        manifest["package"] = name;
        WriteJson(path, manifest);
        EXPECT_THROW(VL::ExportGame(options), std::runtime_error);
        EXPECT_FALSE(fs::exists(options.outputDirectory));
    }
}

TEST_F(GameExportTest, TerrainCompositedRejectsManifestTraversal)
{
    TerrainPackage();
    auto terrain = ReadJson(resources / "Generated/TR_composited.json");
    terrain["compositedVT"]["packagePath"] = "../assets/TerrainCook/tile_00/manifest.json";
    WriteJson(resources / "Generated/TR_composited.json", terrain);
    EXPECT_THROW(VL::ExportGame(options), std::invalid_argument);
    EXPECT_FALSE(fs::exists(options.outputDirectory));
}

TEST_F(GameExportTest, TerrainCompositedAcceptsWindowsRelativeManifestSeparators)
{
    TerrainPackage();
    auto terrain = ReadJson(resources / "Generated/TR_composited.json");
    terrain["compositedVT"]["packagePath"] = "TerrainCook\\tile_00\\manifest.json";
    WriteJson(resources / "Generated/TR_composited.json", terrain);
    EXPECT_NO_THROW(VL::ExportGame(options));
    EXPECT_TRUE(fs::exists(options.outputDirectory / "resources/TerrainCook/tile_00/manifest.json"));
}

TEST_F(GameExportTest, TerrainCompositedRejectsMissingOrTruncatedBinary)
{
    const auto path = TerrainPackage();
    const auto manifest = ReadJson(path);
    const auto binary = path.parent_path() / manifest.at("package").get<std::string>();
    fs::remove(binary);
    EXPECT_THROW(VL::ExportGame(options), std::runtime_error);
    Write(binary, "truncated");
    EXPECT_THROW(VL::ExportGame(options), std::runtime_error);
    EXPECT_FALSE(fs::exists(options.outputDirectory));
}

TEST_F(GameExportTest, TerrainCompositedRejectsSameSizeCorruption)
{
    const auto path = TerrainPackage();
    const auto manifest = ReadJson(path);
    Write(path.parent_path() / manifest.at("package").get<std::string>(),
        std::string(manifest.at("packageBytes").get<size_t>(), 'x'));
    EXPECT_THROW(VL::ExportGame(options), std::runtime_error);
    EXPECT_FALSE(fs::exists(options.outputDirectory));
}

TEST_F(GameExportTest, TerrainCompositedRejectsBadPageIntegrityAndMissingTail)
{
    const auto path = TerrainPackage();
    const auto original = ReadJson(path);
    auto manifest = original;
    manifest["pages"][0]["hash"] = VL::ContentHasher::HashString("incorrect page").ToHex();
    WriteJson(path, manifest);
    EXPECT_THROW(VL::ExportGame(options), std::runtime_error);
    manifest = original;
    manifest["pages"] = Json::array();
    WriteJson(path, manifest);
    EXPECT_THROW(VL::ExportGame(options), std::runtime_error);
    EXPECT_FALSE(fs::exists(options.outputDirectory));
}

TEST_F(GameExportTest, TerrainCompositedRejectsOffsetsAndContentIdentityMismatch)
{
    const auto path = TerrainPackage();
    const auto original = ReadJson(path);
    auto manifest = original;
    manifest["pages"][0]["offset"] = 0;
    WriteJson(path, manifest);
    EXPECT_THROW(VL::ExportGame(options), std::runtime_error);
    manifest = original;
    manifest["surfaces"][0]["width"] = 2;
    WriteJson(path, manifest);
    EXPECT_THROW(VL::ExportGame(options), std::runtime_error);
}

TEST_F(GameExportTest, TerrainCompositedRejectsDuplicateManifestFields)
{
    const auto path = TerrainPackage();
    auto serialized = ReadJson(path).dump();
    serialized.insert(1, "\"version\":99,");
    Write(path, serialized);
    EXPECT_THROW(VL::ExportGame(options), std::runtime_error);
    EXPECT_FALSE(fs::exists(options.outputDirectory));
}

TEST_F(GameExportTest, TerrainCompositedRejectsManifestSymlink)
{
    const auto path = TerrainPackage();
    const auto original = path.parent_path() / "original.json";
    fs::rename(path, original);
    std::error_code error;
    fs::create_symlink(original, path, error);
    if (error) GTEST_SKIP() << "Symlink creation unavailable: " << error.message();
    EXPECT_THROW(VL::ExportGame(options), std::runtime_error);
    EXPECT_FALSE(fs::exists(options.outputDirectory));
}

TEST_F(GameExportTest, TerrainCompositedRequiresCookManifestWhenEnabled)
{
    const auto path = TerrainPackage();
    fs::remove(path);
    EXPECT_THROW(VL::ExportGame(options), std::runtime_error);
    EXPECT_FALSE(fs::exists(options.outputDirectory));
}

TEST_F(GameExportTest, TerrainCompositedDisabledIsRejectedBeforeExport)
{
    WriteJson(resources / "Generated/TR_disabled.json", {{"type", "terrain"},
        {"compositedVT", {{"enabled", false},
            {"packagePath", "Generated/not-cooked/manifest.json"}}}});
    Scene(Json::array({{{"type", "terrain"}, {"terrainPath", "Generated/TR_disabled.json"}}}));
    EXPECT_THROW(VL::ExportGame(options), std::runtime_error);
    EXPECT_FALSE(fs::exists(options.outputDirectory));
}

TEST_F(GameExportTest, TerrainMissingCompositedConfigurationIsRejectedBeforeExport)
{
    WriteJson(resources / "Generated/TR_removed.json", {{"type", "terrain"}});
    Scene(Json::array({{{"type", "terrain"}, {"terrainPath", "Generated/TR_removed.json"}}}));
    EXPECT_THROW(VL::ExportGame(options), std::runtime_error);
    EXPECT_FALSE(fs::exists(options.outputDirectory));
}

TEST_F(GameExportTest, TerrainCompositedVerifyRejectsRetiredConfigurationAfterInventoryRehash)
{
    EnableGameFlavor();
    TerrainPackage();
    VL::ExportGame(options);
    const fs::path relative = "resources/Generated/TR_composited.json";
    const auto packaged = options.outputDirectory / relative;
    const auto original = ReadJson(packaged);
    for (const bool missing : {false, true})
    {
        auto changed = original;
        if (missing)
            changed.erase("compositedVT");
        else
            changed["compositedVT"]["enabled"] = false;
        WriteJson(packaged, changed);
        auto inventory = ReadJson(options.outputDirectory / "game-export-manifest.json");
        bool updated = false;
        for (auto& file : inventory["files"])
            if (file.at("path") == relative.generic_string())
            {
                file["size"] = fs::file_size(packaged);
                file["digest"] = VL::ContentHasher::HashFile(packaged).ToHex();
                updated = true;
            }
        ASSERT_TRUE(updated);
        WriteJson(options.outputDirectory / "game-export-manifest.json", inventory);
        if (missing)
            EXPECT_THROW(VL::VerifyGamePackage(options.outputDirectory), std::runtime_error);
        else
            EXPECT_THROW(VL::VerifyGamePackage(options.outputDirectory), std::invalid_argument);
    }
}

TEST_F(GameExportTest, TerrainCompositedVerifyRejectsBinaryOmittedFromExportInventory)
{
    EnableGameFlavor();
    const auto path = TerrainPackage();
    const auto binary = ReadJson(path).at("package").get<std::string>();
    VL::ExportGame(options);
    auto inventory = ReadJson(options.outputDirectory / "game-export-manifest.json");
    auto& files = inventory["files"];
    for (auto iterator = files.begin(); iterator != files.end();)
    {
        if (fs::path(iterator->at("path").get<std::string>()).filename() == binary)
            iterator = files.erase(iterator);
        else
            ++iterator;
    }
    WriteJson(options.outputDirectory / "game-export-manifest.json", inventory);
    EXPECT_THROW(VL::VerifyGamePackage(options.outputDirectory), std::runtime_error);
}

TEST_F(GameExportTest, TerrainCompositedVerifyChecksSchemaAfterExportDigestsMatch)
{
    EnableGameFlavor();
    const auto path = TerrainPackage();
    VL::ExportGame(options);
    const auto relative = fs::path("resources") / path.lexically_relative(resources);
    const auto packaged = options.outputDirectory / relative;
    auto manifest = ReadJson(packaged);
    manifest["version"] = 99;
    WriteJson(packaged, manifest);
    auto inventory = ReadJson(options.outputDirectory / "game-export-manifest.json");
    for (auto& file : inventory["files"])
    {
        if (file.at("path") != relative.generic_string()) continue;
        file["size"] = fs::file_size(packaged);
        file["digest"] = VL::ContentHasher::HashFile(packaged).ToHex();
    }
    WriteJson(options.outputDirectory / "game-export-manifest.json", inventory);
    EXPECT_THROW(VL::VerifyGamePackage(options.outputDirectory), std::runtime_error);
}

TEST_F(GameExportTest, TerrainCompositedExportsProductionObjectsAndAuthoritativeDynamicMaterial)
{
    EnableGameFlavor();
    const auto manifest = TerrainPackage();
    const auto store = VL::TerrainCompositedVTStore::Open(manifest);
    VL::ExportGame(options);
    for (const auto& object : store.Production().objects)
    {
        const auto source = manifest.parent_path() / object.relativePath;
        EXPECT_TRUE(fs::is_regular_file(options.outputDirectory / "resources" / source.lexically_relative(resources)));
    }
    EXPECT_TRUE(fs::is_regular_file(options.outputDirectory / "resources" /
        manifest.parent_path().lexically_relative(resources) /
        store.Production().surfaces.front().runtimeMaterialInstancePath));
    EXPECT_NO_THROW(VL::VerifyGamePackage(options.outputDirectory));
}

TEST_F(GameExportTest, TerrainWorldMaterialsResolveFromManifestParentInsteadOfRootDecoys)
{
    EnableGameFlavor();
    const auto manifest = TerrainPackage();
    const auto store = VL::TerrainCompositedVTStore::Open(manifest);
    const auto& surface = store.Production().surfaces.front();
    for (const auto& name : {surface.runtimeMaterialInstancePath, surface.runtimeDetailMaterialInstancePath})
    {
        WriteJson(resources / name, {{"type", "materialInstance"},
            {"material", "shader/glsl/M_wrong_root_decoy.json"}, {"description", "root-decoy"}});
        auto authoritative = ReadJson(manifest.parent_path() / name);
        authoritative["configHelp"] = "manifest-parent:" + name;
        WriteJson(manifest.parent_path() / name, authoritative);
    }
    const auto closure = VL::TerrainPackageClosure::Open(resources, manifest.lexically_relative(resources).generic_string());
    VL::ExportGame(options);
    for (const auto& name : {surface.runtimeMaterialInstancePath, surface.runtimeDetailMaterialInstancePath})
    {
        const auto relative = manifest.parent_path().lexically_relative(resources) / name;
        EXPECT_EQ(closure.materialInstances.count(relative.generic_string()), 1);
        const auto exported = options.outputDirectory / "resources" / relative;
        EXPECT_EQ(ReadJson(exported).at("configHelp"), "manifest-parent:" + name);
        EXPECT_EQ(VL::ContentHasher::HashFile(exported),
            VL::ContentHasher::HashFile(manifest.parent_path() / name));
        EXPECT_FALSE(fs::exists(options.outputDirectory / "resources" / name));
    }
    EXPECT_NO_THROW(VL::VerifyGamePackage(options.outputDirectory));
}

TEST_F(GameExportTest, TerrainRuntimeMaterialNativeValidatorRejectsInvalidSparseOverridesBeforeAnyExportCopy)
{
    EnableGameFlavor();
    const auto manifest = TerrainPackage();
    const auto store = VL::TerrainCompositedVTStore::Open(manifest);
    const auto& surface = store.Production().surfaces.front();
    for (const auto& name : {surface.runtimeMaterialInstancePath, surface.runtimeDetailMaterialInstancePath})
    {
        const auto path = manifest.parent_path() / name;
        const auto original = ReadJson(path);
        for (const auto* fault : {"empty-textures", "empty-parameters", "unknown-parameter", "repeated-default",
                 "wrong-parameter-type"})
        {
            auto changed = original;
            if (std::string(fault) == "empty-textures") changed["textures"] = Json::object();
            else if (std::string(fault) == "empty-parameters") changed["parameters"] = Json::object();
            else if (std::string(fault) == "unknown-parameter") changed["parameters"] = {{"u_unknown", 2.0}};
            else if (std::string(fault) == "repeated-default") changed["parameters"] = {{"u_runtimeTest", 1.0}};
            else changed["parameters"] = {{"u_runtimeTest", "invalid-float"}};
            WriteJson(path, changed);
            EXPECT_THROW(VL::ExportGame(options), std::runtime_error) << name << " " << fault;
            EXPECT_FALSE(fs::exists(options.outputDirectory));
        }
        WriteJson(path, original);
    }
}

TEST_F(GameExportTest, TerrainRuntimeMaterialNativeValidatorChecksInstalledOverridesAfterInventoryRehash)
{
    EnableGameFlavor();
    const auto manifest = TerrainPackage();
    const auto store = VL::TerrainCompositedVTStore::Open(manifest);
    VL::ExportGame(options);
    const auto originalInventory = ReadJson(options.outputDirectory / "game-export-manifest.json");
    for (const auto& name : {store.Production().surfaces.front().runtimeMaterialInstancePath,
             store.Production().surfaces.front().runtimeDetailMaterialInstancePath})
    {
        const auto path = options.outputDirectory / "resources" / manifest.parent_path().lexically_relative(resources) / name;
        const auto original = ReadJson(path);
        auto changed = original;
        changed["textures"] = Json::object();
        WriteJson(path, changed);
        auto inventory = originalInventory;
        for (auto& file : inventory["files"])
            if (file.at("path") == path.lexically_relative(options.outputDirectory).generic_string())
            {
                file["size"] = fs::file_size(path);
                file["digest"] = VL::ContentHasher::HashFile(path).ToHex();
            }
        WriteJson(options.outputDirectory / "game-export-manifest.json", inventory);
        EXPECT_THROW(VL::VerifyGamePackage(options.outputDirectory), std::runtime_error) << name;
        WriteJson(path, original);
        WriteJson(options.outputDirectory / "game-export-manifest.json", originalInventory);
    }
    EXPECT_NO_THROW(VL::VerifyGamePackage(options.outputDirectory));
}

TEST_F(GameExportTest, TerrainWorldMissingManifestRelativeMaterialNeverFallsBackToRoot)
{
    const auto manifest = TerrainPackage();
    const auto store = VL::TerrainCompositedVTStore::Open(manifest);
    const auto& surface = store.Production().surfaces.front();
    for (const auto& name : {surface.runtimeMaterialInstancePath, surface.runtimeDetailMaterialInstancePath})
    {
        ASSERT_TRUE(fs::is_regular_file(resources / name));
        const auto path = manifest.parent_path() / name;
        const auto backup = fs::path(path.string() + ".hold");
        fs::rename(path, backup);
        EXPECT_THROW(VL::TerrainPackageClosure::Open(resources,
            manifest.lexically_relative(resources).generic_string()), std::runtime_error);
        EXPECT_THROW(VL::ExportGame(options), std::runtime_error);
        EXPECT_FALSE(fs::exists(options.outputDirectory));
        fs::rename(backup, path);
    }
}

TEST_F(GameExportTest, TerrainCompositedLocalWeightsRemainLosslessAndMapped)
{
    EnableGameFlavor();
    const auto manifest = TerrainPackage(VL::TerrainCompositedWeightSource::LocalLayerArray);
    VL::ExportGame(options);
    const auto path = options.outputDirectory / "resources" / manifest.lexically_relative(resources);
    const auto store = VL::TerrainCompositedVTStore::Open(path);
    const auto& surface = store.Production().surfaces.front();
    EXPECT_EQ(surface.weightSource, VL::TerrainCompositedWeightSource::LocalLayerArray);
    EXPECT_EQ(surface.localToGlobal, (std::vector<uint32_t>{7, 3}));
    EXPECT_EQ(surface.globalToSourceSlice[7], 1);
    EXPECT_EQ(store.ReadInput(surface.inputs.localLayerWeights).size(), 4 * 4 * 2 * sizeof(float));
    EXPECT_TRUE(surface.inputs.coverageWeights.empty());
    EXPECT_NO_THROW(store.ValidateAll());
    EXPECT_NO_THROW(VL::VerifyGamePackage(options.outputDirectory));
}

TEST_F(GameExportTest, TerrainCompositedWorldManifestAndInputsAreCopiedOnceForBothModes)
{
    EnableGameFlavor();
    for (const auto mode : {VL::TerrainCompositedWeightSource::CoverageAtlas,
             VL::TerrainCompositedWeightSource::LocalLayerArray})
    {
        options.outputDirectory = root / (mode == VL::TerrainCompositedWeightSource::CoverageAtlas
            ? "coverage-game" : "local-game");
        const auto manifest = TerrainWorldPackage(mode);
        const auto store = VL::TerrainCompositedVTStore::Open(manifest);
        const auto result = VL::ExportGame(options);
        const auto first = ReadJson(options.outputDirectory / "resources/Generated/TR_composited.json");
        const auto second = ReadJson(options.outputDirectory / "resources/Generated/TR_shared.json");
        EXPECT_EQ(first.at("compositedVT").at("packagePath"), second.at("compositedVT").at("packagePath"));
        EXPECT_FALSE(first.at("compositedVT").contains("productionSourcePath"));
        EXPECT_FALSE(second.at("compositedVT").contains("productionSourcePath"));
        EXPECT_FALSE(fs::exists(options.outputDirectory / "resources/Generated/Author"));
        size_t manifests = 0;
        size_t payloads = 0;
        size_t objects = 0;
        std::set<std::string> paths;
        for (const auto& file : result.manifest.at("files"))
        {
            const auto name = file.at("path").get<std::string>();
            EXPECT_TRUE(paths.insert(name).second);
            const fs::path path(name);
            manifests += path.filename() == "manifest.json";
            payloads += path.extension() == ".vlcvt";
            objects += name.find("/inputs/") != std::string::npos;
        }
        EXPECT_EQ(manifests, 1);
        EXPECT_EQ(payloads, 1);
        EXPECT_EQ(objects, store.Production().objects.size());
        for (const auto& surface : store.Production().surfaces)
            for (const auto& name : {surface.runtimeMaterialInstancePath, surface.runtimeDetailMaterialInstancePath})
                EXPECT_TRUE(fs::is_regular_file(options.outputDirectory / "resources" /
                    manifest.parent_path().lexically_relative(resources) / name));
        EXPECT_NO_THROW(VL::VerifyGamePackage(options.outputDirectory));
    }
}

TEST_F(GameExportTest, TerrainCompositedAuthorRecipeIsNeitherReadNorPackaged)
{
    EnableGameFlavor();
    TerrainPackage();
    auto terrain = ReadJson(resources / "Generated/TR_composited.json");
    const std::string recipe = "Generated/Author/recipe.production.json";
    terrain["compositedVT"]["productionSourcePath"] = recipe;
    WriteJson(resources / "Generated/TR_composited.json", terrain);
    Write(resources / recipe, "not-json-author-data-must-not-be-read");
    VL::ExportGame(options);
    EXPECT_FALSE(fs::exists(options.outputDirectory / "resources" / recipe));
    EXPECT_FALSE(ReadJson(options.outputDirectory / "resources/Generated/TR_composited.json")
        .at("compositedVT").contains("productionSourcePath"));
    EXPECT_NO_THROW(VL::VerifyGamePackage(options.outputDirectory));
}

TEST_F(GameExportTest, TerrainWorldAuthorDependenciesRoundtripWithoutReadingOrExportingAuthorFiles)
{
    EnableGameFlavor();
    const auto manifest = TerrainPackage();
    auto production = VL::TerrainCompositedVTStore::Open(manifest).Production();
    const auto object = production.surfaces.front().inputs.coverageWeights;
    production.surfaces.front().authorDependencies = {
        {"resource", "Generated/Author/original-paint.vlpaint", "bytes",
            VL::ContentHasher::HashString("frozen legacy paint bytes").ToHex(), {object}},
        {"resource", "Generated/Author/original-paint.vlpaint.meta.json", "absent",
            VL::ContentHasher::HashString("terrain-optional-absent-v1").ToHex(), {object}}};
    RepublishTerrain(manifest, production);
    const auto expected = ReadJson(manifest).at("production").at("surfaces")[0].at("authorDependencies");
    ASSERT_EQ(expected.size(), 2);
    ASSERT_FALSE(fs::exists(resources / "Generated/Author"));
    VL::ExportGame(options);
    const auto packaged = options.outputDirectory / "resources" / manifest.lexically_relative(resources);
    EXPECT_EQ(ReadJson(packaged).at("production").at("surfaces")[0].at("authorDependencies"), expected);
    EXPECT_FALSE(fs::exists(options.outputDirectory / "resources/Generated/Author"));
    EXPECT_NO_THROW(VL::VerifyGamePackage(options.outputDirectory));
}

TEST_F(GameExportTest, TerrainRawAuthorBytesArePackagedWithoutReadingOrCompilingOriginalAuthor)
{
    EnableGameFlavor();
    const auto manifest = TerrainPackage();
    const auto before = VL::TerrainCompositedVTStore::Open(manifest).Production();
    const auto production = AddRawAuthor(manifest);
    const auto& raw = production.objects.back();
    EXPECT_NE(raw.digest.ToHex(), production.surfaces.front().authorDependencies.back().digest);
    EXPECT_NE(before.ContentHash(), production.ContentHash());
    EXPECT_EQ(before.SurfaceContentHash(before.surfaces.front().surfaceId),
        production.SurfaceContentHash(production.surfaces.front().surfaceId));
    const auto json = VL::TerrainCompositedProductionToJson(production);
    EXPECT_EQ(json.at("objects").back().at("kind"), 3);
    EXPECT_TRUE(json.at("objects").back().at("image").is_null());
    RepublishTerrain(manifest, production);
    ASSERT_FALSE(fs::exists(resources / production.surfaces.front().authorDependencies.back().path));
    VL::ExportGame(options);
    const auto packaged = options.outputDirectory / "resources" / manifest.lexically_relative(resources);
    const auto installed = VL::TerrainCompositedVTStore::Open(packaged);
    EXPECT_NO_THROW(installed.ValidateAll());
    EXPECT_EQ(installed.ReadInput(raw.objectId), VL::TerrainCompositedVTStore::Open(manifest).ReadInput(raw.objectId));
    EXPECT_FALSE(fs::exists(options.outputDirectory / "resources/Generated/Author"));
    EXPECT_FALSE(fs::exists(options.outputDirectory / "shader/glsl/DO_NOT_COMPILE_RAW_AUTHOR.json"));
    EXPECT_NO_THROW(VL::VerifyGamePackage(options.outputDirectory));
}

TEST_F(GameExportTest, TerrainRawAuthorAbsenceRecordIsActualNonemptyPackagedBytes)
{
    EnableGameFlavor();
    const auto manifest = TerrainPackage();
    const auto production = AddRawAuthor(manifest, true);
    RepublishTerrain(manifest, production);
    VL::ExportGame(options);
    const auto installed = VL::TerrainCompositedVTStore::Open(
        options.outputDirectory / "resources" / manifest.lexically_relative(resources));
    const auto bytes = installed.ReadInput("raw-absence");
    ASSERT_FALSE(bytes.empty());
    const auto absence = Json::parse(bytes);
    EXPECT_EQ(absence.at("mode"), "absent");
    EXPECT_EQ(absence.at("digest"), production.surfaces.front().authorDependencies.back().digest);
    EXPECT_NE(production.objects.back().digest.ToHex(), absence.at("digest"));
    EXPECT_FALSE(fs::exists(options.outputDirectory / "resources/Generated/Author"));
    EXPECT_NO_THROW(VL::VerifyGamePackage(options.outputDirectory));
}

TEST_F(GameExportTest, TerrainRawAuthorMissingOrCorruptBytesRejectExportAndInstallation)
{
    EnableGameFlavor();
    const auto manifest = TerrainPackage();
    const auto production = AddRawAuthor(manifest);
    RepublishTerrain(manifest, production);
    const auto path = manifest.parent_path() / production.objects.back().relativePath;
    const auto saved = fs::path(path.string() + ".hold");
    fs::rename(path, saved);
    EXPECT_THROW(VL::ExportGame(options), std::runtime_error);
    fs::rename(saved, path);
    std::ifstream input(path, std::ios::binary);
    const std::string original{std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
    input.close();
    auto damaged = original;
    damaged.front() ^= 1;
    Write(path, damaged);
    EXPECT_THROW(VL::ExportGame(options), std::runtime_error);
    Write(path, original);
    VL::ExportGame(options);
    const auto installedPath = options.outputDirectory / "resources" / path.lexically_relative(resources);
    Write(installedPath, damaged);
    auto inventory = ReadJson(options.outputDirectory / "game-export-manifest.json");
    for (auto& file : inventory["files"])
        if (file.at("path") == installedPath.lexically_relative(options.outputDirectory).generic_string())
            file["digest"] = VL::ContentHasher::HashFile(installedPath).ToHex();
    WriteJson(options.outputDirectory / "game-export-manifest.json", inventory);
    EXPECT_THROW(VL::VerifyGamePackage(options.outputDirectory), std::runtime_error);
    EXPECT_THROW(VL::TerrainPackageClosure::Open(options.outputDirectory / "resources",
        manifest.lexically_relative(resources).generic_string()), std::runtime_error);
    fs::remove(installedPath);
    EXPECT_THROW(VL::VerifyGamePackage(options.outputDirectory), std::invalid_argument);
}

TEST_F(GameExportTest, TerrainRawAuthorAliasesAndImageMetadataAreRejected)
{
    const auto manifest = TerrainPackage();
    const auto production = AddRawAuthor(manifest);
    auto changed = production;
    changed.objects.back().relativePath = changed.objects.front().relativePath;
    EXPECT_THROW(VL::TerrainCompositedProductionToJson(changed), std::runtime_error);
    changed = production;
    auto alias = changed.objects.back();
    alias.objectId = "raw-case-alias";
    alias.relativePath.replace(0, 13, "AUTHOR-INPUTS");
    changed.objects.push_back(alias);
    EXPECT_THROW(VL::TerrainCompositedProductionToJson(changed), std::runtime_error);
    auto json = VL::TerrainCompositedProductionToJson(production);
    json["objects"].back()["image"] = Json::object();
    EXPECT_THROW(VL::TerrainCompositedProductionFromJson(json), std::runtime_error);
    changed = production;
    changed.objects.back().relativePath = "manifest.json";
    EXPECT_THROW(RepublishTerrain(manifest, changed), std::runtime_error);
}

TEST_F(GameExportTest, TerrainRawAuthorCannotBecomeGeneratorSourceArtifactOrImageBinding)
{
    const auto manifest = TerrainPackage();
    const auto production = AddRawAuthor(manifest);
    const auto id = production.objects.back().objectId;
    auto changed = production;
    changed.generator.computeSource = id;
    EXPECT_THROW(changed.Validate(), std::runtime_error);
    changed = production;
    changed.generator.includeSources.push_back(id);
    EXPECT_THROW(changed.Validate(), std::runtime_error);
    changed = production;
    changed.generator.compiledArtifacts.push_back(id);
    EXPECT_THROW(changed.Validate(), std::runtime_error);
    changed = production;
    changed.surfaces.front().inputs.layerAlbedo = id;
    EXPECT_THROW(changed.Validate(), std::runtime_error);
}

TEST_F(GameExportTest, TerrainCompositedRejectsProducerShaderIdentityDrift)
{
    EnableGameFlavor();
    for (const auto& role : {"policy", "main", "include", "spv"})
    {
        const auto manifest = TerrainPackage();
        auto production = VL::TerrainCompositedVTStore::Open(manifest).Production();
        const auto& generator = production.generator;
        if (std::string(role) == "policy")
            production.generator.compilePolicy = "other-release";
        else
        {
            const auto id = std::string(role) == "main" ? generator.computeSource
                : std::string(role) == "include" ? generator.includeSources.front()
                : generator.compiledArtifacts.front();
            for (auto& object : production.objects)
            {
                if (object.objectId != id) continue;
                const std::string bytes(static_cast<size_t>(object.bytes), 'x');
                Write(manifest.parent_path() / object.relativePath, bytes);
                object.bytes = bytes.size();
                object.digest = VL::ContentHasher::HashString(bytes);
            }
        }
        RepublishTerrain(manifest, production);
        EXPECT_NO_THROW(VL::TerrainCompositedVTStore::Open(manifest).ValidateAll());
        EXPECT_THROW(VL::ExportGame(options), std::runtime_error) << role;
        EXPECT_FALSE(fs::exists(options.outputDirectory));
    }
}

TEST_F(GameExportTest, TerrainCompositedDynamicMaterialsDoNotChangeBaseIdentity)
{
    const auto production = TerrainProduction(VL::TerrainCompositedWeightSource::LocalLayerArray);
    const auto identity = production.SurfaceContentHash(production.surfaces.front().surfaceId);
    auto changed = production;
    changed.surfaces.front().runtimeMaterialInstancePath = "Generated/MI_other.json";
    changed.surfaces.front().runtimeDetailMaterialInstancePath = "Generated/MI_other_detail.json";
    EXPECT_EQ(changed.SurfaceContentHash(changed.surfaces.front().surfaceId), identity);
}

TEST_F(GameExportTest, TerrainCompositedRejectsEveryMissingProductionInput)
{
    for (const auto mode : {VL::TerrainCompositedWeightSource::CoverageAtlas,
             VL::TerrainCompositedWeightSource::LocalLayerArray})
    {
        const auto manifest = TerrainPackage(mode);
        const auto store = VL::TerrainCompositedVTStore::Open(manifest);
        for (const auto& object : store.Production().objects)
        {
            const auto path = manifest.parent_path() / object.relativePath;
            const auto backup = path.string() + ".hold";
            fs::rename(path, backup);
            EXPECT_THROW(VL::ExportGame(options), std::runtime_error) << object.objectId;
            fs::rename(backup, path);
        }
    }
}

TEST_F(GameExportTest, TerrainCompositedRejectsCorruptProductionBytesUsingSharedValidator)
{
    for (const auto mode : {VL::TerrainCompositedWeightSource::CoverageAtlas,
             VL::TerrainCompositedWeightSource::LocalLayerArray})
    {
        const auto manifest = TerrainPackage(mode);
        const auto store = VL::TerrainCompositedVTStore::Open(manifest);
        for (const auto& object : store.Production().objects)
        {
            const auto path = manifest.parent_path() / object.relativePath;
            std::ifstream stream(path, std::ios::binary);
            const std::string original{std::istreambuf_iterator<char>(stream), std::istreambuf_iterator<char>()};
            stream.close();
            auto changed = original;
            changed.front() ^= 1;
            Write(path, changed);
            EXPECT_THROW(VL::ExportGame(options), std::runtime_error) << object.objectId;
            Write(path, original);
        }
    }
}

TEST_F(GameExportTest, TerrainCompositedRejectsMissingDynamicMaterial)
{
    const auto manifest = TerrainPackage();
    const auto store = VL::TerrainCompositedVTStore::Open(manifest);
    const auto& surface = store.Production().surfaces.front();
    for (const auto& name : {surface.runtimeMaterialInstancePath, surface.runtimeDetailMaterialInstancePath})
    {
        const auto path = manifest.parent_path() / name;
        const auto backup = path.string() + ".hold";
        fs::rename(path, backup);
        EXPECT_THROW(VL::ExportGame(options), std::runtime_error);
        fs::rename(backup, path);
    }
}

TEST_F(GameExportTest, TerrainCompositedRejectsProductionMetadataTampering)
{
    const auto manifest = TerrainPackage();
    auto document = ReadJson(manifest);
    document["production"] = Json::object();
    WriteJson(manifest, document);
    EXPECT_THROW(VL::ExportGame(options), std::runtime_error);
}

TEST_F(GameExportTest, TerrainCompositedVerifyRejectsProductionInputsOrMaterialOmittedFromInventory)
{
    EnableGameFlavor();
    const auto manifest = TerrainPackage();
    const auto store = VL::TerrainCompositedVTStore::Open(manifest);
    auto sources = VL::TerrainPackageClosure::ProductionFiles(manifest.parent_path(), store.Production());
    sources.push_back(manifest.parent_path() / store.Production().surfaces.front().runtimeMaterialInstancePath);
    sources.push_back(manifest.parent_path() / store.Production().surfaces.front().runtimeDetailMaterialInstancePath);
    VL::ExportGame(options);
    const auto original = ReadJson(options.outputDirectory / "game-export-manifest.json");
    for (const auto& source : sources)
    {
        const auto relative = (fs::path("resources") / source.lexically_relative(resources)).generic_string();
        auto inventory = original;
        auto& files = inventory["files"];
        for (auto iterator = files.begin(); iterator != files.end();)
            if (iterator->at("path") == relative)
                iterator = files.erase(iterator);
            else
                ++iterator;
        WriteJson(options.outputDirectory / "game-export-manifest.json", inventory);
        EXPECT_THROW(VL::VerifyGamePackage(options.outputDirectory), std::runtime_error);
    }
}

} // namespace
