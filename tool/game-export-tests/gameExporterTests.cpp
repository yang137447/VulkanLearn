#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <fstream>
#include <iterator>
#include <set>
#include "gameExporter.h"
#include "shader/build/contentHash.h"

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
            add(ShaderArtifact("StageEntry",
                "kind=StageEntry|path=" + name + "|stage=compute|macros=|target=Vulkan1.4",
                {{"runtimeCompute", "spv/" + folder + "/" + stem + "_comp.spv"},
                 {"debugCompute", "spv/" + folder + "/" + stem + "_comp.debug"}},
                {name}, {name}));
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
    EXPECT_FALSE(fs::exists(options.outputDirectory / "shader/glsl/generate"));
    EXPECT_TRUE(fs::exists(options.outputDirectory / "shader/spv/shader-build-cache.json"));
    EXPECT_NO_THROW(VL::VerifyGamePackage(options.outputDirectory));
}

TEST_F(GameExportTest, GameFlavorGameplayDefaultGlassCrackMaterialIsIncluded)
{
    EnableGameFlavor();
    VL::ExportGame(options);
    EXPECT_TRUE(fs::exists(options.outputDirectory / "resources/Common/Materials/MI_glass_crack.json"));
    EXPECT_TRUE(fs::exists(options.outputDirectory / "shader/glsl/M_glass_crack.json"));
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
} // namespace
