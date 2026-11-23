#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <limits>
#include <string>

#include "terrain/terrainCompositedDependencies.h"
#include "terrain/terrainAssetLoader.h"

namespace
{
namespace fs = std::filesystem;
using Json = nlohmann::json;

class TerrainCompositedDependenciesTest : public testing::Test
{
protected:
    void SetUp() override
    {
        static std::atomic<uint64_t> sequence{0};
        directory = fs::temp_directory_path() /
            ("vl-composited-dependencies-" +
                std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()) +
                "-" + std::to_string(sequence++));
        roots = {directory / "resources", directory / "project"};
        fs::create_directories(roots.resourceRoot);
        fs::create_directories(roots.projectRoot);
        plan.desc.assetPath = "Maps/TR_tile.json";
        plan.desc.heightmapPath = "Generated/Author/height.bin";
        plan.desc.paintPath = "Maps/tile.vlpaint";
        plan.desc.materialInstancePath = "Maps/MI_tile.json";
        plan.desc.layers.push_back(VL::TerrainLayerDesc{});
        plan.placementId = "terrain|tile";
        terrain = {{"type", "terrain"},
            {"heightmapPath", plan.desc.heightmapPath},
            {"paintPath", plan.desc.paintPath},
            {"materialInstancePath", plan.desc.materialInstancePath},
            {"compositedVT", {{"packagePath", "Generated/Cooked/tile.json"}}}};
        WriteJson(plan.desc.assetPath, terrain);
        Write(plan.desc.heightmapPath, std::string("height\0bytes", 12));
        Write(plan.desc.paintPath, "paint bytes");
        WriteJson(plan.desc.materialInstancePath, {
            {"type", "materialInstance"}, {"material", "shader/glsl/M_tile.json"},
            {"textures", {{"layers", "Maps/Textures/T_layers.json"}}}});
        WriteJson("shader/glsl/M_tile.json", {
            {"type", "material"}, {"textures", {{"layers", {{"default", nullptr}}}}},
            {"shaderEvaluation", {{"vertex", "M_tile.vertex.glsl"},
                {"surface", "M_tile.surface.glsl"}}}}, true);
        Write("shader/glsl/M_tile.vertex.glsl", "vertex evaluation", true);
        Write("shader/glsl/M_tile.surface.glsl", "surface evaluation", true);
        WriteJson("Maps/Textures/T_layers.json", {
            {"type", "textureArray"}, {"name", "Layers"}, {"colorSpace", "linear"},
            {"sources", {"Source/albedo.dds", "Source/other.dds"}},
            {"displacementSources", {"Source/height.png", "Source/height2.png"}}});
        Write("Source/albedo.dds", "container bytes");
        Write("Source/other.dds", "second slice bytes");
        Write("Source/height.png", "height source bytes");
        Write("Source/height2.png", "second height source bytes");
    }

    void TearDown() override
    {
        std::error_code error;
        fs::remove_all(directory, error);
    }

    void Write(const std::string& name, const std::string& bytes, bool project = false)
    {
        const auto path = (project ? roots.projectRoot : roots.resourceRoot) / name;
        fs::create_directories(path.parent_path());
        std::ofstream output(path, std::ios::binary | std::ios::trunc);
        output.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
        ASSERT_TRUE(output.good()) << path;
    }

    void WriteJson(const std::string& name, const Json& value, bool project = false)
    {
        Write(name, value.dump(), project);
    }

    Json Build() const
    {
        return VL::BuildTerrainCompositedDependencies(
            roots, plan, placement, plan.desc.materialInstancePath, buildConfig);
    }

    void UseTerrainMaterial()
    {
        WriteJson("shader/glsl/M_terrain.json", {
            {"type", "material"}, {"parameters", {
                {"u_uvScale", {{"type", "float"}, {"default", 4.0}}},
                {"u_detailStrength", {{"type", "float"}, {"default", 0.5}}},
                {"u_globalOriginCm", {{"type", "vec2"}, {"default", {0.0, 0.0}}}}}},
            {"textures", {{"layers", {{"default", nullptr}}},
                {"globalColour", {{"default", "Maps/Textures/T_global.json"}}}}},
            {"shaderEvaluation", {{"vertex", "M_tile.vertex.glsl"},
                {"surface", "M_tile.surface.glsl"}}}}, true);
        WriteJson(plan.desc.materialInstancePath, {
            {"type", "materialInstance"}, {"material", "shader/glsl/M_terrain.json"},
            {"textures", {{"layers", "Maps/Textures/T_layers.json"}}}});
        WriteJson("Maps/Textures/T_global.json", {
            {"type", "texture"}, {"name", "Global"}, {"colorSpace", "linear"},
            {"source", "Source/global.png"}});
        Write("Source/global.png", "retained dynamic global image");
    }

    Json ReadJson(const std::string& name, bool project = false) const
    {
        std::ifstream input((project ? roots.projectRoot : roots.resourceRoot) / name);
        return Json::parse(input);
    }

    static const Json& Entry(const Json& manifest, const std::string& path)
    {
        for (const auto& entry : manifest.at("dependencies"))
            if (entry.at("path") == path)
                return entry;
        throw std::runtime_error("Missing test manifest entry: " + path);
    }

    fs::path directory;
    VL::TerrainCompositedDependencyRoots roots;
    VL::TerrainBuildPlan plan;
    Json terrain;
    std::array<float, 16> placement{1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};
    Json buildConfig{{"generatorVersion", 1}, {"encoding", "rgba8"}};
};
}

TEST_F(TerrainCompositedDependenciesTest, CapturesExactSourceBytesAndRootRelativeReferences)
{
    const auto manifest = Build();
    EXPECT_EQ(Entry(manifest, "Source/albedo.dds").at("digest"),
        VL::ContentHasher::HashString("container bytes").ToHex());
    EXPECT_EQ(Entry(manifest, "Generated/Author/height.bin").at("digest"),
        VL::ContentHasher::HashString(std::string("height\0bytes", 12)).ToHex());
    EXPECT_EQ(Entry(manifest, "Source/height2.png").at("root"), "resource");
    EXPECT_EQ(Entry(manifest, "shader/glsl/M_tile.json").at("root"), "project");
    EXPECT_EQ(Entry(manifest, "shader/glsl/M_tile.surface.glsl").at("root"), "project");
    EXPECT_EQ(Entry(manifest, "Maps/tile.vlpaint.meta.json").at("mode"), "absent");
    EXPECT_NO_THROW(VL::ValidateTerrainCompositedAuthors(roots, manifest));
    EXPECT_NO_THROW(VL::ValidateTerrainCompositedManifestConfig(
        manifest, plan, placement, plan.desc.materialInstancePath, buildConfig));
}

TEST_F(TerrainCompositedDependenciesTest, ProducesStableSortedManifestIndependentOfPhysicalRoot)
{
    const auto first = Build();
    EXPECT_EQ(first, Build());
    const auto secondRoot = directory / "relocated";
    fs::copy(roots.resourceRoot, secondRoot, fs::copy_options::recursive);
    auto relocated = roots;
    relocated.resourceRoot = secondRoot;
    EXPECT_EQ(first, VL::BuildTerrainCompositedDependencies(
        relocated, plan, placement, plan.desc.materialInstancePath, buildConfig));
    std::string previous;
    for (const auto& entry : first.at("dependencies"))
    {
        const auto key = entry.at("root").get<std::string>() + ":" +
            entry.at("path").get<std::string>();
        EXPECT_LT(previous, key);
        previous = key;
    }
}

TEST_F(TerrainCompositedDependenciesTest, ExcludesCookPackagePathAndPayloadWithoutDroppingGeneratedAuthors)
{
    const auto before = Build();
    terrain["compositedVT"]["packagePath"] = "Generated/Cooked/other.json";
    WriteJson(plan.desc.assetPath, terrain);
    Write("Generated/Cooked/other.json", "new manifest");
    Write("Generated/Cooked/package.vlcvt", "new cooked payload");
    plan.desc.compositedVT.packagePath = "Generated/Cooked/other.json";
    EXPECT_EQ(before, Build());
    EXPECT_NO_THROW(VL::ValidateTerrainCompositedAuthors(roots, before));
    EXPECT_NO_THROW(VL::ValidateTerrainCompositedManifestConfig(
        before, plan, placement, plan.desc.materialInstancePath, buildConfig));
    Write(plan.desc.heightmapPath, "edited generated author height");
    EXPECT_NE(before.at("hash"), Build().at("hash"));
}

TEST_F(TerrainCompositedDependenciesTest, DetectsChangedSourceBytesWithUnchangedSizeAndTimestamp)
{
    const auto before = Build();
    const auto source = roots.resourceRoot / "Source/albedo.dds";
    const auto timestamp = fs::last_write_time(source);
    Write("Source/albedo.dds", "Container bytes");
    fs::last_write_time(source, timestamp);
    EXPECT_THROW(VL::ValidateTerrainCompositedAuthors(roots, before), std::runtime_error);
    EXPECT_NE(before.at("hash"), Build().at("hash"));
}

TEST_F(TerrainCompositedDependenciesTest, TracksTextureDescriptorBytesAndSliceOrder)
{
    const auto before = Build();
    const auto path = roots.resourceRoot / "Maps/Textures/T_layers.json";
    std::ifstream input(path);
    Json descriptor;
    input >> descriptor;
    input.close();
    std::swap(descriptor["sources"][0], descriptor["sources"][1]);
    WriteJson("Maps/Textures/T_layers.json", descriptor);
    EXPECT_NE(before.at("hash"), Build().at("hash"));
    EXPECT_THROW(VL::ValidateTerrainCompositedAuthors(roots, before), std::runtime_error);
    const auto reordered = Build();
    Write("Maps/Textures/T_layers.json", descriptor.dump(2) + "\n");
    EXPECT_NE(reordered.at("hash"), Build().at("hash"));
}

TEST_F(TerrainCompositedDependenciesTest, TracksMaterialEvaluationSourceBytes)
{
    const auto before = Build();
    Write("shader/glsl/M_tile.surface.glsl", "changed surface evaluation", true);
    EXPECT_NE(before.at("hash"), Build().at("hash"));
    EXPECT_THROW(VL::ValidateTerrainCompositedAuthors(roots, before), std::runtime_error);
}

TEST_F(TerrainCompositedDependenciesTest, TracksTransitiveQuotedAndStandardIncludesConservatively)
{
    Write("shader/glsl/M_tile.surface.glsl",
        "/* #include \"missing.glsl\" */\n#include \"materialFunction/first.glsl\"\n"
        "// #include \"also-missing.glsl\"\n", true);
    Write("shader/glsl/materialFunction/first.glsl",
        "# include <common/second.glsl>\n#if 0\n#include \"inactive.glsl\"\n#endif\n", true);
    Write("shader/glsl/common/second.glsl", "#include \"../materialFunction/first.glsl\"\n", true);
    Write("shader/glsl/materialFunction/inactive.glsl", "float inactive = 0;\n", true);
    const auto before = Build();
    EXPECT_EQ(Entry(before, "shader/glsl/common/second.glsl").at("mode"), "bytes");
    EXPECT_EQ(Entry(before, "shader/glsl/materialFunction/inactive.glsl").at("mode"), "bytes");
    Write("shader/glsl/common/second.glsl", "float newTerm = 2;\n", true);
    EXPECT_NE(before.at("hash"), Build().at("hash"));
    EXPECT_THROW(VL::ValidateTerrainCompositedAuthors(roots, before), std::runtime_error);
}

TEST_F(TerrainCompositedDependenciesTest, RejectsMissingEscapingAndNonliteralShaderIncludes)
{
    Write("shader/glsl/M_tile.surface.glsl", "#include \"missing.glsl\"\n", true);
    EXPECT_THROW(Build(), std::runtime_error);
    Write("shader/glsl/M_tile.surface.glsl", "#include \"../../outside.glsl\"\n", true);
    EXPECT_THROW(Build(), std::runtime_error);
    Write("shader/glsl/M_tile.surface.glsl", "#include INCLUDE_MACRO\n", true);
    EXPECT_THROW(Build(), std::runtime_error);
}

TEST_F(TerrainCompositedDependenciesTest, TracksResourceRootShadowingOfProjectMaterial)
{
    const auto before = Build();
    WriteJson("shader/glsl/M_tile.json", {{"type", "material"}, {"textures", Json::object()}});
    EXPECT_THROW(VL::ValidateTerrainCompositedAuthors(roots, before), std::runtime_error);
    EXPECT_NE(before.at("hash"), Build().at("hash"));
}

TEST_F(TerrainCompositedDependenciesTest, SupportsMaterialsWithoutShaderEvaluation)
{
    WriteJson("shader/glsl/M_tile.json",
        {{"type", "material"}, {"textures", Json::object()}}, true);
    EXPECT_NO_THROW(Build());
}

TEST_F(TerrainCompositedDependenciesTest, TerrainDynamicNumericSaveKeepsStaticIdentity)
{
    UseTerrainMaterial();
    const auto before = Build();
    EXPECT_EQ(Entry(before, plan.desc.materialInstancePath).at("mode"),
        "terrain-material-instance-static-json-v2");
    auto material = ReadJson(plan.desc.materialInstancePath);
    material["parameters"] = {{"u_terrainTessRadiusM", 15.0}, {"u_detailStrength", 2.0},
        {"u_globalOriginCm", {25.0, 75.0}}, {"u_layerDisplacementScale", 0.5},
        {"u_globalFadeEndM", 300.0}, {"u_macroStrength", 0.2}};
    WriteJson(plan.desc.materialInstancePath, material);
    EXPECT_EQ(before, Build());
    EXPECT_NO_THROW(VL::ValidateTerrainCompositedAuthors(roots, before));
}

TEST_F(TerrainCompositedDependenciesTest, TerrainUvHeightBlendWeightModeAndMacrosRemainStatic)
{
    UseTerrainMaterial();
    const auto before = Build();
    const auto original = ReadJson(plan.desc.materialInstancePath);
    for (const char* name : {"u_uvScale", "u_heightBlendStrength", "u_terrainWeightMapMode"})
    {
        auto material = original;
        material["parameters"][name] = 2.0;
        WriteJson(plan.desc.materialInstancePath, material);
        EXPECT_NE(before.at("hash"), Build().at("hash")) << name;
        EXPECT_THROW(VL::ValidateTerrainCompositedAuthors(roots, before), std::runtime_error);
    }
    auto material = original;
    material["macros"] = {{"VL_TERRAIN_TESSELLATION", 1}};
    WriteJson(plan.desc.materialInstancePath, material);
    EXPECT_NE(before.at("hash"), Build().at("hash"));
}

TEST_F(TerrainCompositedDependenciesTest, TerrainGlobalMapsAreRetainedConsumersOutsideStaticClosure)
{
    UseTerrainMaterial();
    const auto before = Build();
    for (const auto& entry : before.at("dependencies"))
    {
        EXPECT_NE(entry.at("path"), "Maps/Textures/T_global.json");
        EXPECT_NE(entry.at("path"), "Source/global.png");
    }
    Write("Source/global.png", "updated dynamic global image");
    auto material = ReadJson(plan.desc.materialInstancePath);
    material["textures"]["globalColour"] = "Maps/Textures/T_global.json";
    material["textures"]["globalMasks"] = "Maps/Textures/T_global.json";
    WriteJson(plan.desc.materialInstancePath, material);
    EXPECT_EQ(before, Build());
    EXPECT_NO_THROW(VL::ValidateTerrainCompositedAuthors(roots, before));
}

TEST_F(TerrainCompositedDependenciesTest, TerrainStaticArrayHeightAndWeightsStillInvalidateCook)
{
    UseTerrainMaterial();
    auto material = ReadJson(plan.desc.materialInstancePath);
    for (const char* name : {"layerAlbedo", "layerNormal", "layerPbr", "layerHeight", "layerWeightMaps"})
        material["textures"][name] = "Maps/Textures/T_layers.json";
    WriteJson(plan.desc.materialInstancePath, material);
    const auto before = Build();
    Write("Source/height2.png", "changed height source");
    EXPECT_NE(before.at("hash"), Build().at("hash"));
    EXPECT_THROW(VL::ValidateTerrainCompositedAuthors(roots, before), std::runtime_error);
    const auto heightChanged = Build();
    auto descriptor = ReadJson("Maps/Textures/T_layers.json");
    descriptor["flipY"] = false;
    WriteJson("Maps/Textures/T_layers.json", descriptor);
    EXPECT_NE(heightChanged.at("hash"), Build().at("hash"));
}

TEST_F(TerrainCompositedDependenciesTest, TerrainDynamicDefaultsDoNotRecookButStaticDefaultsDo)
{
    UseTerrainMaterial();
    const auto before = Build();
    auto definition = ReadJson("shader/glsl/M_terrain.json", true);
    EXPECT_EQ(Entry(before, "shader/glsl/M_terrain.json").at("mode"),
        "terrain-material-static-json-v2");
    definition["parameters"]["u_detailStrength"]["default"] = 3.0;
    definition["parameters"]["u_globalOriginCm"]["default"] = {10.0, 20.0};
    definition["textures"]["globalColour"]["default"] = nullptr;
    WriteJson("shader/glsl/M_terrain.json", definition, true);
    EXPECT_EQ(before, Build());
    EXPECT_NO_THROW(VL::ValidateTerrainCompositedAuthors(roots, before));
    definition["parameters"]["u_uvScale"]["default"] = 5.0;
    WriteJson("shader/glsl/M_terrain.json", definition, true);
    EXPECT_NE(before.at("hash"), Build().at("hash"));
}

TEST_F(TerrainCompositedDependenciesTest, UnknownParametersAndOtherMaterialsRemainConservative)
{
    const auto custom = Build();
    auto material = ReadJson(plan.desc.materialInstancePath);
    material["parameters"]["u_detailStrength"] = 1.0;
    WriteJson(plan.desc.materialInstancePath, material);
    EXPECT_NE(custom.at("hash"), Build().at("hash"));
    EXPECT_EQ(Entry(Build(), plan.desc.materialInstancePath).at("mode"), "bytes");
    UseTerrainMaterial();
    const auto terrainBefore = Build();
    material = ReadJson(plan.desc.materialInstancePath);
    material["parameters"]["u_newUnknownStaticInput"] = 1.0;
    WriteJson(plan.desc.materialInstancePath, material);
    EXPECT_NE(terrainBefore.at("hash"), Build().at("hash"));
}

TEST_F(TerrainCompositedDependenciesTest, TracksOptionalSidecarAppearanceModificationAndRemoval)
{
    const auto absent = Build();
    Write("Maps/tile.vlpaint.meta.json", "sidecar-v1");
    EXPECT_THROW(VL::ValidateTerrainCompositedAuthors(roots, absent), std::runtime_error);
    const auto present = Build();
    EXPECT_EQ(Entry(present, "Maps/tile.vlpaint.meta.json").at("mode"), "bytes");
    Write("Maps/tile.vlpaint.meta.json", "sidecar-v2");
    EXPECT_THROW(VL::ValidateTerrainCompositedAuthors(roots, present), std::runtime_error);
    fs::remove(roots.resourceRoot / "Maps/tile.vlpaint.meta.json");
    EXPECT_THROW(VL::ValidateTerrainCompositedAuthors(roots, present), std::runtime_error);
}

TEST_F(TerrainCompositedDependenciesTest, IncludesPlacementLayersAndBuildConfigurationInIdentity)
{
    const auto original = Build();
    placement[12] = 100;
    EXPECT_NE(original.at("hash"), Build().at("hash"));
    EXPECT_THROW(VL::ValidateTerrainCompositedManifestConfig(
        original, plan, placement, plan.desc.materialInstancePath, buildConfig), std::runtime_error);
    placement[12] = 0;
    plan.desc.layers[0].uvScale = 2;
    EXPECT_NE(original.at("hash"), Build().at("hash"));
    plan.desc.layers[0].uvScale = 1;
    buildConfig["generatorVersion"] = 2;
    EXPECT_NE(original.at("hash"), Build().at("hash"));
}

TEST_F(TerrainCompositedDependenciesTest, ConfigurationValidationWorksWithoutAnyAuthorFiles)
{
    const auto manifest = Build();
    const auto savedResources = directory / "unmounted-resources";
    const auto savedProject = directory / "unmounted-project";
    fs::rename(roots.resourceRoot, savedResources);
    fs::rename(roots.projectRoot, savedProject);
    EXPECT_NO_THROW(VL::ValidateTerrainCompositedManifestConfig(
        manifest, plan, placement, plan.desc.materialInstancePath, buildConfig));
    EXPECT_EQ(VL::TerrainCompositedDependencyHash(manifest).ToHex(), manifest.at("hash"));
    EXPECT_THROW(VL::ValidateTerrainCompositedAuthors(roots, manifest), std::runtime_error);
}

TEST_F(TerrainCompositedDependenciesTest, RejectsTamperedManifestMetadataAndDependencyDigests)
{
    auto manifest = Build();
    manifest["config"]["size"][0] = 25;
    EXPECT_THROW(VL::TerrainCompositedDependencyHash(manifest), std::runtime_error);
    manifest = Build();
    manifest["dependencies"][0]["digest"] = std::string(64, '0');
    EXPECT_THROW(VL::ValidateTerrainCompositedManifestConfig(
        manifest, plan, placement, plan.desc.materialInstancePath, buildConfig), std::runtime_error);
}

TEST_F(TerrainCompositedDependenciesTest, MissingDeclaredSourceThrowsWithItsPath)
{
    fs::remove(roots.resourceRoot / "Source/height2.png");
    try
    {
        Build();
        FAIL() << "Missing dependency accepted";
    }
    catch (const std::runtime_error& error)
    {
        EXPECT_NE(std::string(error.what()).find("Source/height2.png"), std::string::npos);
    }
}

TEST_F(TerrainCompositedDependenciesTest, RejectsRootTraversalAndAbsoluteSources)
{
    auto descriptor = Json{{"type", "texture"}, {"name", "Invalid"},
        {"colorSpace", "linear"}, {"source", "../outside.bin"}};
    WriteJson("Maps/Textures/T_layers.json", descriptor);
    Write("outside.bin", "unrelated");
    EXPECT_THROW(Build(), std::runtime_error);
    descriptor["source"] = (directory / "outside.bin").generic_string();
    WriteJson("Maps/Textures/T_layers.json", descriptor);
    EXPECT_THROW(Build(), std::runtime_error);
}

TEST_F(TerrainCompositedDependenciesTest, RejectsCanonicalSymlinkEscape)
{
    const auto outside = directory / "outside";
    fs::create_directories(outside);
    std::error_code error;
    fs::create_directory_symlink(outside, roots.resourceRoot / "escape", error);
    if (error)
        GTEST_SKIP() << "Symlink creation unavailable: " << error.message();
    terrain["heightmapPath"] = "escape/height.bin";
    WriteJson(plan.desc.assetPath, terrain);
    EXPECT_THROW(Build(), std::runtime_error);
}

TEST_F(TerrainCompositedDependenciesTest, RecursesMaterialReferencesAndDetectsCycles)
{
    WriteJson("Maps/MI_parent.json", {{"type", "materialInstance"},
        {"material", "shader/glsl/M_tile.json"}});
    WriteJson(plan.desc.materialInstancePath, {{"type", "materialInstance"},
        {"material", "Maps/MI_parent.json"}});
    const auto manifest = Build();
    EXPECT_EQ(Entry(manifest, "Maps/MI_parent.json").at("root"), "resource");
    WriteJson("Maps/MI_parent.json", {{"type", "materialInstance"},
        {"material", plan.desc.materialInstancePath}});
    EXPECT_THROW(Build(), std::runtime_error);
}

TEST_F(TerrainCompositedDependenciesTest, RejectsCookOutputAsAuthorTextureSource)
{
    terrain["compositedVT"]["packagePath"] = "Source/albedo.dds";
    WriteJson(plan.desc.assetPath, terrain);
    EXPECT_THROW(Build(), std::runtime_error);
}

TEST_F(TerrainCompositedDependenciesTest, RejectsNonFiniteConfiguration)
{
    placement[0] = std::numeric_limits<float>::infinity();
    EXPECT_THROW(Build(), std::runtime_error);
    placement[0] = 1;
    buildConfig["number"] = std::numeric_limits<double>::quiet_NaN();
    EXPECT_THROW(Build(), std::runtime_error);
}

TEST(TerrainCompositedRealAssets, CollectsPegasusAndInvalidatesEditedNestedShaderSnapshot)
{
    const char* fixture = std::getenv("VL_TERRAIN_DEPENDENCY_REAL_FIXTURE");
    if (!fixture || !*fixture)
        GTEST_SKIP() << "Set VL_TERRAIN_DEPENDENCY_REAL_FIXTURE to a saved resource-root TR path";
    const auto projectRoot = fs::current_path();
    std::ifstream configFile(projectRoot / "config/config.json");
    ASSERT_TRUE(configFile.good());
    const auto config = Json::parse(configFile);
    const auto resourceRoot = fs::canonical(projectRoot /
        config.at("resourcePath").get<std::string>());
    auto plan = VL::TerrainAssetLoader::Prepare(fixture);
    const VL::TerrainCompositedDependencyRoots roots{resourceRoot, projectRoot};
    const std::array<float, 16> placement{
        1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};
    const Json buildConfig{{"generatorVersion", 1}, {"encoding", "rgba8-real-fixture"}};
    const auto manifest = VL::BuildTerrainCompositedDependencies(
        roots, plan, placement, plan.desc.materialInstancePath, buildConfig);
    std::ifstream materialFile(resourceRoot / plan.desc.materialInstancePath);
    ASSERT_TRUE(materialFile.good());
    const auto material = Json::parse(materialFile);
    EXPECT_EQ(material.at("material"), "shader/glsl/M_terrain.json");
    const std::string nested = "shader/glsl/materialFunction/mf_terrainLayerBlend.glsl";
    bool found = false;
    for (const auto& entry : manifest.at("dependencies"))
        found |= entry.at("root") == "project" && entry.at("path") == nested;
    ASSERT_TRUE(found);
    EXPECT_NO_THROW(VL::ValidateTerrainCompositedAuthors(roots, manifest));
    std::cout << "REAL_FIXTURE path=" << fixture << " material=" << material.at("material")
        << " dependencies=" << manifest.at("dependencies").size()
        << " hash=" << manifest.at("hash").get<std::string>() << '\n';
    RecordProperty("fixture", fixture);
    RecordProperty("dependencyCount", manifest.at("dependencies").size());
    RecordProperty("dependencyHash", manifest.at("hash").get<std::string>());
    const auto snapshot = fs::temp_directory_path() /
        ("vl-composited-real-shader-snapshot-" +
            std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    struct SnapshotOwner
    {
        fs::path path;
        ~SnapshotOwner()
        {
            std::error_code error;
            fs::remove_all(path, error);
        }
    } owner{snapshot};
    for (const auto& entry : manifest.at("dependencies"))
        if (entry.at("root") == "project" && entry.at("mode") != "absent")
        {
            const auto relative = entry.at("path").get<std::string>();
            fs::create_directories((snapshot / relative).parent_path());
            fs::copy_file(projectRoot / relative, snapshot / relative);
        }
    const VL::TerrainCompositedDependencyRoots snapshotRoots{resourceRoot, snapshot};
    EXPECT_EQ(manifest, VL::BuildTerrainCompositedDependencies(
        snapshotRoots, plan, placement, plan.desc.materialInstancePath, buildConfig));
    {
        std::ofstream changed(snapshot / nested, std::ios::binary | std::ios::app);
        changed << "\n";
        ASSERT_TRUE(changed.good());
    }
    EXPECT_THROW(VL::ValidateTerrainCompositedAuthors(snapshotRoots, manifest), std::runtime_error);
    const auto edited = VL::BuildTerrainCompositedDependencies(
        snapshotRoots, plan, placement, plan.desc.materialInstancePath, buildConfig);
    EXPECT_NE(manifest.at("hash"), edited.at("hash"));
    std::cout << "REAL_FIXTURE nestedShaderEditHash=" << edited.at("hash").get<std::string>() << '\n';
    RecordProperty("nestedShaderEditHash", edited.at("hash").get<std::string>());
    std::ofstream evidence(projectRoot / "build/terrain-composited-real-dependencies.json");
    evidence << manifest.dump(2) << '\n';
    ASSERT_TRUE(evidence.good());
}
