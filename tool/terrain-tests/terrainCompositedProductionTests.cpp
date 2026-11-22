#include <gtest/gtest.h>
#include "terrainCompositedProductionFixture.h"
#include "terrain/terrainCompositedProductionIdentity.h"
#include "terrain/terrainCompositedShaderPath.h"

#include <atomic>
#include <chrono>
#include <limits>

namespace
{
using namespace VL;

TEST(TerrainCompositedShaderPathTest, SeparatesSourceAndArtifactRootsAndRejectsTraversal)
{
    EXPECT_EQ(TerrainCompositedShaderRelativePath("terrain/terrainCompositedBakeWorld.comp", true),
        std::filesystem::path("glsl/terrain/terrainCompositedBakeWorld.comp"));
    EXPECT_EQ(TerrainCompositedShaderRelativePath("materialFunction/terrainCoverage.glsl", true),
        std::filesystem::path("glsl/materialFunction/terrainCoverage.glsl"));
    EXPECT_EQ(TerrainCompositedShaderRelativePath("spv/terrain/world_comp.spv", false),
        std::filesystem::path("spv/terrain/world_comp.spv"));
    EXPECT_THROW(TerrainCompositedShaderRelativePath("../terrain/file.comp", true), std::invalid_argument);
    EXPECT_THROW(TerrainCompositedShaderRelativePath("glsl/terrain/file.comp", true), std::invalid_argument);
    EXPECT_THROW(TerrainCompositedShaderRelativePath("terrain/file.spv", false), std::invalid_argument);
    EXPECT_THROW(TerrainCompositedShaderRelativePath("C:/file.comp", true), std::invalid_argument);
}

class TerrainCompositedProductionTest : public testing::Test
{
protected:
    void SetUp() override
    {
        static std::atomic<uint64_t> sequence{0};
        root = std::filesystem::temp_directory_path() / ("vl-production-" +
            std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()) +
            "-" + std::to_string(sequence++));
        closure = TerrainProductionFixture::Closure(root, {"a"});
    }
    void TearDown() override
    {
        std::error_code ignored;
        std::filesystem::remove_all(root, ignored);
    }
    TerrainCompositedProductionObject& Mutable(const std::string& id)
    {
        for (auto& object : closure.objects)
            if (object.objectId == id)
                return object;
        throw std::runtime_error("missing test object");
    }
    void ChangeDisplacement()
    {
        auto& catalog = Mutable("catalog");
        auto bytes = ReadBinaryFile(root / catalog.relativePath);
        const float displacement = 0.2f;
        std::memcpy(bytes.data() + ((3 * 256 + 7) * 4 + 3) * 4, &displacement, 4);
        catalog.digest = ContentHasher::HashBytes(bytes.data(), bytes.size());
        PopulateTerrainCompositedCatalogDigests(catalog, bytes);
        catalog.relativePath = "objects/" + catalog.digest.ToHex() + ".bin";
        TerrainProductionFixture::WriteFixtureBytes(root / catalog.relativePath, bytes);
    }
    void HeightBlend()
    {
        TerrainCompositedProductionObject object;
        object.objectId = "height";
        object.relativePath = "objects/height.bin";
        object.image = {"r16-unorm", 8, 8, 1, 4};
        object.bytes = object.image.ByteSize();
        std::vector<uint8_t> bytes(static_cast<size_t>(object.bytes), 127);
        object.digest = ContentHasher::HashBytes(bytes.data(), bytes.size());
        TerrainProductionFixture::WriteFixtureBytes(root / object.relativePath, bytes);
        closure.objects.push_back(object);
        closure.surfaces[0].inputs.layerHeight = "height";
        closure.surfaces[0].parameters.heightBlendStrength = 1;
    }
    std::filesystem::path root;
    TerrainCompositedProductionClosure closure;
};

TEST_F(TerrainCompositedProductionTest, RawAuthorSourceRoundTripsWithoutChangingBaseIdentity)
{
    const auto original = closure.SurfaceContentHash("a");
    TerrainCompositedProductionObject object;
    object.objectId = "raw-author";
    object.kind = TerrainCompositedProductionObjectKind::RawAuthorSource;
    const std::string raw = "{\"type\":\"terrain\",\"formatting\":\"raw\"}\n";
    object.bytes = raw.size();
    object.digest = ContentHasher::HashString(raw);
    object.relativePath = "author-inputs/" + object.digest.ToHex() + ".bin";
    closure.objects.push_back(object);
    closure.surfaces.front().authorDependencies.push_back(
        {"resource", "Author/TR_actual.json", "bytes", object.digest.ToHex(), {object.objectId}});
    EXPECT_NO_THROW(closure.Validate());
    const auto parsed = TerrainCompositedProductionFromJson(TerrainCompositedProductionToJson(closure));
    EXPECT_EQ(parsed.ContentHash(), closure.ContentHash());
    EXPECT_EQ(parsed.SurfaceContentHash("a"), original);
    auto bad = closure;
    bad.objects.back().image.width = 1;
    EXPECT_THROW(bad.Validate(), std::exception);
    auto document = TerrainCompositedProductionToJson(closure);
    document["objects"].back()["image"] = nlohmann::json::object();
    EXPECT_THROW(TerrainCompositedProductionFromJson(document), std::exception);
    document = TerrainCompositedProductionToJson(closure);
    document["objects"].back()["kind"] = 4;
    EXPECT_THROW(TerrainCompositedProductionFromJson(document), std::exception);
}
}

TEST_F(TerrainCompositedProductionTest, StrictJsonRoundTripAndOrderIndependentIdentity)
{
    const auto original = closure.ContentHash();
    const auto parsed = TerrainCompositedProductionFromJson(TerrainCompositedProductionToJson(closure));
    EXPECT_EQ(parsed.ContentHash(), original);
    std::reverse(closure.objects.begin(), closure.objects.end());
    EXPECT_EQ(closure.ContentHash(), original);
}

TEST_F(TerrainCompositedProductionTest, RejectsUnknownFieldsBadEnumsAndUnverifiedProjection)
{
    const auto original = TerrainCompositedProductionToJson(closure);
    auto document = original;
    document["surfaces"][0]["surprise"] = 1;
    EXPECT_THROW(TerrainCompositedProductionFromJson(document), std::exception);
    document = original;
    document["objects"][0]["image"]["filter"] = 7;
    EXPECT_THROW(TerrainCompositedProductionFromJson(document), std::exception);
    auto& catalog = Mutable("catalog");
    catalog.baseCatalogDigest = ContentHasher::HashString("forged projection");
    const auto bytes = ReadBinaryFile(root / catalog.relativePath);
    EXPECT_THROW(ValidateTerrainCompositedProductionPayload(catalog, bytes), std::exception);
}

TEST_F(TerrainCompositedProductionTest, ZeroHeightBlendExcludesTesDisplacementFromBaseIdentity)
{
    const auto base = closure.SurfaceContentHash("a");
    const auto full = closure.ContentHash();
    ChangeDisplacement();
    EXPECT_NE(closure.ContentHash(), full);
    EXPECT_EQ(closure.SurfaceContentHash("a"), base);
}

TEST_F(TerrainCompositedProductionTest, HeightBlendIncludesDisplacementAndSourceMipIdentity)
{
    HeightBlend();
    const auto before = closure.SurfaceContentHash("a");
    ChangeDisplacement();
    EXPECT_NE(closure.SurfaceContentHash("a"), before);
    const auto afterDisplacement = closure.SurfaceContentHash("a");
    auto& height = Mutable("height");
    auto heightBytes = ReadBinaryFile(root / height.relativePath);
    heightBytes[heightBytes.size() - 1] ^= 1;
    height.digest = ContentHasher::HashBytes(heightBytes.data(), heightBytes.size());
    TerrainProductionFixture::WriteFixtureBytes(root / height.relativePath, heightBytes);
    EXPECT_NE(closure.SurfaceContentHash("a"), afterDisplacement);
}

TEST_F(TerrainCompositedProductionTest, DynamicMapsAndBothRuntimeMaterialPathsDoNotInvalidateBase)
{
    auto object = closure.Object("albedo");
    object.objectId = "global";
    object.relativePath = "objects/global.bin";
    closure.objects.push_back(object);
    closure.surfaces[0].inputs.globalColour = "global";
    const auto before = closure.SurfaceContentHash("a");
    Mutable("global").digest = ContentHasher::HashString("changed global colour");
    closure.surfaces[0].runtimeMaterialInstancePath = "runtime/MI_changed_dynamic.json";
    closure.surfaces[0].runtimeDetailMaterialInstancePath = "runtime/MI_detail_tes.json";
    EXPECT_EQ(closure.SurfaceContentHash("a"), before);
}

TEST_F(TerrainCompositedProductionTest, DisconnectedSurfaceStaticInputDoesNotInvalidateBase)
{
    auto second = closure.surfaces[0];
    second.surfaceId = "b";
    closure.surfaces.push_back(second);
    closure.adjacency = TerrainCompositedAdjacencyGraph({"a", "b"}, {});
    const auto before = closure.SurfaceContentHash("a");
    closure.surfaces[1].parameters.uvScale = 12;
    EXPECT_EQ(closure.SurfaceContentHash("a"), before);
    EXPECT_THROW(closure.SurfaceContentHash("missing"), std::exception);
}

TEST_F(TerrainCompositedProductionTest, FrozenLocalWeightsPreserveMoreThanFourNonSequentialLayers)
{
    closure = TerrainProductionFixture::Closure(root / "local", {"a"}, TerrainCompositedWeightSource::LocalLayerArray);
    closure.surfaces[0].localToGlobal = {201, 17, 4, 98, 65, 2};
    closure.surfaces[0].globalToSourceSlice.fill(UINT32_MAX);
    for (const auto global : closure.surfaces[0].localToGlobal)
        closure.surfaces[0].globalToSourceSlice[global] = 0;
    auto& weights = Mutable("local-weights");
    weights.image.layers = 6;
    weights.bytes = weights.image.ByteSize();
    std::vector<uint8_t> payload(static_cast<size_t>(weights.bytes), 0);
    for (uint32_t layer = 0; layer < 6; ++layer)
        for (uint32_t pixel = 0; pixel < 16; ++pixel)
        {
            const float weight = float(layer + 1) / 6.f;
            std::memcpy(payload.data() + (layer * 16 + pixel) * 4, &weight, 4);
        }
    weights.digest = ContentHasher::HashBytes(payload.data(), payload.size());
    TerrainProductionFixture::WriteFixtureBytes(root / weights.relativePath, payload);
    EXPECT_NO_THROW(closure.Validate());
    auto parsed = TerrainCompositedProductionFromJson(TerrainCompositedProductionToJson(closure));
    EXPECT_EQ(parsed.surfaces[0].localToGlobal, closure.surfaces[0].localToGlobal);
    EXPECT_EQ(parsed.Object("local-weights").image.layers, 6u);
    EXPECT_NO_THROW(ValidateTerrainCompositedProductionPayload(parsed.Object("local-weights"), payload));
    parsed.surfaces[0].inputs.localLayerWeights.clear();
    EXPECT_THROW(parsed.Validate(), std::exception);
}

TEST_F(TerrainCompositedProductionTest, RejectsWindowsDeviceTraversalAndAmbiguousPaths)
{
    for (const auto* path : {"../escape.bin", "/absolute.bin", "C:/drive.bin", "objects//empty",
        "objects/NUL.bin", "CON/file.bin", "objects/COM1.dat", "objects/LPT9",
        "objects/trailing.", "objects/trailing ", "objects\\backslash"})
    {
        auto changed = closure;
        changed.objects[0].relativePath = path;
        EXPECT_THROW(changed.Validate(), std::exception) << path;
    }
}

TEST_F(TerrainCompositedProductionTest, RejectsBadMappingFramePlacementAndHeightMip)
{
    auto changed = closure;
    changed.surfaces[0].globalToSourceSlice[7] = 99;
    EXPECT_THROW(changed.Validate(), std::exception);
    changed = closure;
    changed.surfaces[0].localToWorld[0] = -1;
    EXPECT_THROW(changed.Validate(), std::exception);
    changed = closure;
    changed.surfaces[0].sampling.frameUvScale[0] = std::numeric_limits<double>::quiet_NaN();
    EXPECT_THROW(changed.Validate(), std::exception);
    HeightBlend();
    Mutable("height").image.mipLevels = 3;
    Mutable("height").bytes = Mutable("height").image.ByteSize();
    EXPECT_THROW(closure.Validate(), std::exception);
}

TEST_F(TerrainCompositedProductionTest, RejectsLinearAlbedoEvenWhenFormatAndTransferAgree)
{
    auto& albedo = Mutable("albedo");
    for (const auto* format : {"rgba8-unorm", "bc7-unorm"})
    {
        albedo.image.format = format;
        albedo.image.transfer = TerrainCompositedProductionTransfer::Linear;
        albedo.bytes = albedo.image.ByteSize();
        EXPECT_THROW(closure.Validate(), std::exception) << format;
    }
}

TEST_F(TerrainCompositedProductionTest, AcceptsSrgbAlbedoFormatsAndRejectsMismatchedTransfer)
{
    auto& albedo = Mutable("albedo");
    for (const auto* format : {"rgba8-srgb", "bc7-srgb"})
    {
        albedo.image.format = format;
        albedo.image.transfer = TerrainCompositedProductionTransfer::Srgb;
        albedo.bytes = albedo.image.ByteSize();
        EXPECT_NO_THROW(closure.Validate()) << format;
        albedo.image.transfer = TerrainCompositedProductionTransfer::Linear;
        EXPECT_THROW(closure.Validate(), std::exception) << format;
    }
}

TEST_F(TerrainCompositedProductionTest, RejectsNonAsciiObjectPathsIncludingNtfsCaseAliases)
{
    for (const std::string path : {"objects/\xC3\xA4.bin", "objects/\xC3\x84.bin",
        "objects/\xE4\xB8\xAD.bin", "objects/bad\x7F.bin", "objects/has space.bin",
        "objects/name%2e.bin"})
    {
        auto changed = closure;
        changed.objects.front().relativePath = path;
        EXPECT_THROW(changed.Validate(), std::exception) << path;
    }
    auto aliased = closure;
    aliased.objects[0].relativePath = "objects/\xC3\xA4.bin";
    aliased.objects[1].relativePath = "objects/\xC3\x84.bin";
    EXPECT_THROW(aliased.Validate(), std::exception);
}

TEST_F(TerrainCompositedProductionTest, RejectsAsciiCaseAliasedObjectPaths)
{
    closure.objects[0].relativePath = "objects/ABC.bin";
    closure.objects[1].relativePath = "OBJECTS/abc.BIN";
    EXPECT_THROW(closure.Validate(), std::exception);
}

TEST_F(TerrainCompositedProductionTest, ObjectPathPolicyPreservesUnicodeSurfaceAndMaterialNames)
{
    const std::string displayName = "terrain|\xE5\xB1\xB1\xE5\x9C\xB0";
    closure.surfaces.front().surfaceId = displayName;
    closure.surfaces.front().runtimeMaterialInstancePath = "runtime/MI_\xE5\xB2\xA9\xE7\x9F\xB3.json";
    closure.adjacency = TerrainCompositedAdjacencyGraph({displayName}, {});
    EXPECT_NO_THROW(closure.Validate());
    EXPECT_NO_THROW(TerrainCompositedProductionFromJson(TerrainCompositedProductionToJson(closure)));
}

TEST_F(TerrainCompositedProductionTest, AuthorDependenciesStrictJsonRoundTrip)
{
    const auto document = TerrainCompositedProductionToJson(closure);
    const auto& entry = document.at("surfaces").at(0).at("authorDependencies").at(0);
    EXPECT_EQ(entry.size(), 5u);
    EXPECT_EQ(entry.at("root"), "project");
    EXPECT_EQ(entry.at("productionObjectIds"), nlohmann::json::array({"compute"}));
    const auto parsed = TerrainCompositedProductionFromJson(document);
    EXPECT_EQ(TerrainCompositedProductionToJson(parsed), document);
}

TEST_F(TerrainCompositedProductionTest, RejectsMissingAuthorDependencyFieldAndUnknownEntryFields)
{
    auto document = TerrainCompositedProductionToJson(closure);
    document["surfaces"][0].erase("authorDependencies");
    EXPECT_THROW(TerrainCompositedProductionFromJson(document), std::exception);
    document = TerrainCompositedProductionToJson(closure);
    document["surfaces"][0]["authorDependencies"][0]["extra"] = true;
    EXPECT_THROW(TerrainCompositedProductionFromJson(document), std::exception);
    document = TerrainCompositedProductionToJson(closure);
    document["surfaces"][0]["authorDependencies"][0].erase("productionObjectIds");
    EXPECT_THROW(TerrainCompositedProductionFromJson(document), std::exception);
}

TEST_F(TerrainCompositedProductionTest, RejectsWrongAuthorDependencyJsonTypes)
{
    const auto original = TerrainCompositedProductionToJson(closure);
    auto document = original;
    document["surfaces"][0]["authorDependencies"] = nlohmann::json::object();
    EXPECT_THROW(TerrainCompositedProductionFromJson(document), std::exception);
    for (const auto* field : {"root", "path", "mode", "digest", "productionObjectIds"})
    {
        document = original;
        document["surfaces"][0]["authorDependencies"][0][field] = 3;
        EXPECT_THROW(TerrainCompositedProductionFromJson(document), std::exception) << field;
    }
}

TEST_F(TerrainCompositedProductionTest, RejectsDuplicateAuthorKeysAndBadRootsOrModes)
{
    auto changed = closure;
    changed.surfaces[0].authorDependencies.push_back(changed.surfaces[0].authorDependencies.front());
    EXPECT_THROW(changed.Validate(), std::exception);
    changed = closure;
    changed.surfaces[0].authorDependencies.front().root = "filesystem";
    EXPECT_THROW(changed.Validate(), std::exception);
    changed = closure;
    changed.surfaces[0].authorDependencies.front().mode = "whatever";
    EXPECT_THROW(changed.Validate(), std::exception);
}

TEST_F(TerrainCompositedProductionTest, RejectsUnsafeAuthorPathsButAllowsChineseAuthorNames)
{
    for (const auto* path : {"../escape", "C:/author.json", "/absolute.json", "shader//bad",
        "shader/./bad", "shader/../bad", "shader\\bad", "shader/NUL.json", "shader/bad."})
    {
        auto changed = closure;
        changed.surfaces[0].authorDependencies.front().path = path;
        EXPECT_THROW(changed.Validate(), std::exception) << path;
    }
    closure.surfaces[0].authorDependencies.front().root = "resource";
    closure.surfaces[0].authorDependencies.front().path = "Materials/MI_\xE5\xB2\xA9\xE7\x9F\xB3.json";
    EXPECT_NO_THROW(closure.Validate());
}

TEST_F(TerrainCompositedProductionTest, RejectsMalformedAndNoncanonicalAuthorDigests)
{
    for (const auto& digest : {std::string{}, std::string("invalid"), std::string(64, 'A'),
        std::string(64, '0'), std::string(65, 'a')})
    {
        auto changed = closure;
        changed.surfaces[0].authorDependencies.front().digest = digest;
        EXPECT_THROW(changed.Validate(), std::exception);
    }
}

TEST_F(TerrainCompositedProductionTest, AbsentAuthorModeRequiresCanonicalSentinel)
{
    auto& dependency = closure.surfaces[0].authorDependencies.front();
    dependency.mode = "absent";
    EXPECT_THROW(closure.Validate(), std::exception);
    dependency.digest = ContentHasher::HashString("terrain-optional-absent-v1").ToHex();
    EXPECT_NO_THROW(closure.Validate());
}

TEST_F(TerrainCompositedProductionTest, RejectsEmptyUnknownAndDuplicateAuthorObjectBindings)
{
    for (const auto& references : {std::vector<std::string>{},
        std::vector<std::string>{"missing"}, std::vector<std::string>{"compute", "compute"}})
    {
        auto changed = closure;
        changed.surfaces[0].authorDependencies.front().productionObjectIds = references;
        EXPECT_THROW(changed.Validate(), std::exception);
    }
}

TEST_F(TerrainCompositedProductionTest, AuthorBaselineOrderDoesNotAffectPublicationIdentity)
{
    auto& dependencies = closure.surfaces[0].authorDependencies;
    dependencies.front().productionObjectIds = {"compute", "frame-normal"};
    dependencies.push_back({"resource", "Materials/MI_terrain.json", "bytes",
        ContentHasher::HashString("author material bytes").ToHex(), {"catalog"}});
    const auto before = closure.ContentHash();
    std::reverse(dependencies.begin(), dependencies.end());
    std::reverse(dependencies.back().productionObjectIds.begin(), dependencies.back().productionObjectIds.end());
    EXPECT_EQ(closure.ContentHash(), before);
}

TEST_F(TerrainCompositedProductionTest, FullAuthorFormattingAndDynamicChangesDoNotInvalidateBasePages)
{
    const auto base = closure.SurfaceContentHash("a");
    auto full = closure.ContentHash();
    auto& dependency = closure.surfaces[0].authorDependencies.front();
    dependency.path = "Materials/MI_dynamic.json";
    dependency.root = "resource";
    dependency.digest = ContentHasher::HashString("reformatted and changed dynamic detail").ToHex();
    EXPECT_NE(closure.ContentHash(), full);
    EXPECT_EQ(closure.SurfaceContentHash("a"), base);
    full = closure.ContentHash();
    dependency.productionObjectIds = {"frame-tangent"};
    EXPECT_NE(closure.ContentHash(), full);
    EXPECT_EQ(closure.SurfaceContentHash("a"), base);
    Mutable("albedo").digest = ContentHasher::HashString("actual static albedo changed");
    EXPECT_NE(closure.SurfaceContentHash("a"), base);
}

TEST_F(TerrainCompositedProductionTest, DynamicAuthorObjectAndLocationDoNotPolluteBaseProjection)
{
    auto global = closure.Object("albedo");
    global.objectId = "dynamic-colour";
    global.relativePath = "objects/dynamic-colour.bin";
    closure.objects.push_back(global);
    closure.surfaces[0].inputs.globalColour = global.objectId;
    closure.surfaces[0].authorDependencies.push_back({"resource", "Maps/global.png", "bytes",
        global.digest.ToHex(), {global.objectId}});
    const auto base = closure.SurfaceContentHash("a");
    const auto full = closure.ContentHash();
    Mutable(global.objectId).relativePath = "relocated/new-colour.bin";
    Mutable(global.objectId).digest = ContentHasher::HashString("changed global colour");
    closure.surfaces[0].authorDependencies.back().path = "Other/global.png";
    closure.surfaces[0].authorDependencies.back().digest = Mutable(global.objectId).digest.ToHex();
    EXPECT_NE(closure.ContentHash(), full);
    EXPECT_EQ(closure.SurfaceContentHash("a"), base);
}

TEST_F(TerrainCompositedProductionTest, GameSchemaValidationNeverReadsAuthorPaths)
{
    auto& dependency = closure.surfaces[0].authorDependencies.front();
    dependency.root = "resource";
    dependency.path = "author-files-not-in-game/Missing.json";
    EXPECT_FALSE(std::filesystem::exists(root / dependency.path));
    EXPECT_NO_THROW(TerrainCompositedProductionFromJson(TerrainCompositedProductionToJson(closure)));
}

TEST_F(TerrainCompositedProductionTest, UsesApprovedGenerationThreePaddingIdentity)
{
    EXPECT_EQ(closure.generator.version, 3u);
    EXPECT_EQ(closure.generator.filterRule,
        "mip0-center-coarse-area-local-normal-rvt-adjacency-padding-v3");
    EXPECT_EQ(closure.generator.encoding, "gpu-rvt-adjacency-area4-rgba8-v3");
    EXPECT_NO_THROW(closure.Validate());
}

TEST_F(TerrainCompositedProductionTest, RejectsGenerationTwoEvenWithNewShaderInputs)
{
    closure.generator.version = 2;
    EXPECT_THROW(closure.Validate(), std::exception);
    EXPECT_THROW(closure.ContentHash(), std::exception);
    EXPECT_THROW(closure.SurfaceContentHash("a"), std::exception);
}

TEST_F(TerrainCompositedProductionTest, RejectsPreviousFilterAndEncodingIdentities)
{
    const auto current = closure.generator;
    for (const auto* filter : {"mip0-center-coarse-area-local-normal-v2",
        "mip0-center-coarse-area-local-normal-rvt-adjacency-v2"})
    {
        closure.generator = current;
        closure.generator.filterRule = filter;
        EXPECT_THROW(closure.Validate(), std::exception);
    }
    for (const auto* encoding : {"rgba8-srgb-rgba8-local-normal-rgba8-orm",
        "rgba8-srgb-rgba8-local-normal-rgba8-orm-rvt-adjacency-padding-v3",
        "gpu-rvt-adjacency-area4-rgba8-v2"})
    {
        closure.generator = current;
        closure.generator.encoding = encoding;
        EXPECT_THROW(closure.Validate(), std::exception);
    }
}

namespace
{
using namespace VL;

// 生产身份只认编译 manifest 的 runtime* 输出角色；debug* 是反射/调试副本。
TEST(TerrainCompositedProductionIdentityRoleTest, RuntimeOutputRolesExcludeDebugReflection)
{
    for (const auto* role : {"runtimeVertex", "runtimeFragment",
        "runtimeTessControl", "runtimeTessEvaluation", "runtimeCompute"})
        EXPECT_TRUE(IsShaderRuntimeOutputRole(role)) << role;
    for (const auto* role : {"debugVertex", "debugFragment",
        "debugTessControl", "debugTessEvaluation", "debugCompute"})
        EXPECT_FALSE(IsShaderRuntimeOutputRole(role)) << role;
}

class TerrainCompositedProductionIdentityTest : public testing::Test
{
protected:
    TerrainCompositedProductionIdentityTest()
    {
        const auto add = [this](const std::string& id,
            TerrainCompositedProductionObjectKind kind, const std::string& digest)
        {
            TerrainCompositedProductionObject object;
            object.objectId = id;
            object.kind = kind;
            object.digest = ContentDigest::FromHex(digest);
            object.relativePath = "objects/" + digest + ".bin";
            expected.objects.push_back(std::move(object));
            return id;
        };
        worldSource = Hash("world.comp source");
        paddingSource = Hash("padding.comp source");
        sharedInclude = Hash("shared bake include source");
        worldRuntime = Hash("world runtime spv");
        worldDebug = Hash("world debug reflection spv");
        paddingRuntime = Hash("padding runtime spv");
        paddingDebug = Hash("padding debug reflection spv");
        expected.generator.computeSource = add("world-source",
            TerrainCompositedProductionObjectKind::ShaderSource, worldSource);
        expected.generator.includeSources = {
            add("padding-source", TerrainCompositedProductionObjectKind::ShaderSource, paddingSource),
            add("shared-include", TerrainCompositedProductionObjectKind::ShaderSource, sharedInclude)};
        // publication 冻结时 runtime 与 debug 输出都作为 ShaderArtifact 记录。
        expected.generator.compiledArtifacts = {
            add("world-runtime-artifact", TerrainCompositedProductionObjectKind::ShaderArtifact, worldRuntime),
            add("world-debug-artifact", TerrainCompositedProductionObjectKind::ShaderArtifact, worldDebug),
            add("padding-runtime-artifact", TerrainCompositedProductionObjectKind::ShaderArtifact, paddingRuntime),
            add("padding-debug-artifact", TerrainCompositedProductionObjectKind::ShaderArtifact, paddingDebug)};
    }

    static std::string Hash(const std::string& text)
    {
        return ContentHasher::HashString(text).ToHex();
    }

    static void AddKernel(ShaderBuildManifestSnapshot& manifest, const std::string& logicalBuildId,
        const std::string& path, const std::string& primaryDigest,
        const std::vector<std::string>& dependencyDigests,
        const std::string& runtimeDigest, const std::string& debugDigest)
    {
        ShaderBuildArtifactRecord artifact;
        artifact.logicalBuildId = logicalBuildId;
        artifact.kind = "StageEntry";
        artifact.normalizedKey = "kind=StageEntry|path=" + path + "|stage=compute";
        artifact.primarySources.push_back({path, primaryDigest});
        for (const std::string& digest : dependencyDigests)
            artifact.dependencies.push_back(
                {"materialFunction/terrainCompositedBake.glsl", digest, {}, 0});
        artifact.outputs.emplace("runtimeCompute",
            ShaderBuildOutputRecord{"spv/terrain/" + logicalBuildId + "_comp.spv", runtimeDigest});
        artifact.outputs.emplace("debugCompute",
            ShaderBuildOutputRecord{"spv/terrain/" + logicalBuildId + "_comp.debug", debugDigest});
        manifest.artifacts.emplace(logicalBuildId, std::move(artifact));
    }

    ShaderBuildManifestSnapshot Manifest() const
    {
        ShaderBuildManifestSnapshot manifest;
        AddKernel(manifest, "world", "terrain/terrainCompositedBakeWorld.comp",
            worldSource, {sharedInclude}, worldRuntime, worldDebug);
        AddKernel(manifest, "padding", "terrain/terrainCompositedBakeWorldPadding.comp",
            paddingSource, {sharedInclude}, paddingRuntime, paddingDebug);
        return manifest;
    }

    TerrainCompositedProductionClosure expected;
    std::string worldSource;
    std::string paddingSource;
    std::string sharedInclude;
    std::string worldRuntime;
    std::string worldDebug;
    std::string paddingRuntime;
    std::string paddingDebug;
};

TEST_F(TerrainCompositedProductionIdentityTest, DebugReflectionRewriteKeepsProductionIdentity)
{
    // 模拟 P4 hotreload：源码只加一个换行，runtime SPV 不变，debug 产物重编译。
    auto actual = Manifest();
    actual.artifacts.at("world").outputs.at("debugCompute").digest =
        Hash("rewritten world debug reflection spv");
    actual.artifacts.at("padding").outputs.at("debugCompute").digest =
        Hash("rewritten padding debug reflection spv");
    EXPECT_TRUE(MatchesTerrainCompositedGeneratorClosure(expected, actual));
}

TEST_F(TerrainCompositedProductionIdentityTest, ChangedRuntimeSpvStillFails)
{
    auto actual = Manifest();
    actual.artifacts.at("world").outputs.at("runtimeCompute").digest =
        Hash("changed world runtime spv");
    EXPECT_FALSE(MatchesTerrainCompositedGeneratorClosure(expected, actual));

    actual = Manifest();
    actual.artifacts.at("padding").outputs.at("runtimeCompute").digest =
        Hash("changed padding runtime spv");
    EXPECT_FALSE(MatchesTerrainCompositedGeneratorClosure(expected, actual));
}

TEST_F(TerrainCompositedProductionIdentityTest, MissingOrExtraKernelStillFails)
{
    auto actual = Manifest();
    actual.artifacts.erase("padding");
    EXPECT_FALSE(MatchesTerrainCompositedGeneratorClosure(expected, actual));

    actual = Manifest();
    actual.artifacts.at("world").outputs.erase("runtimeCompute");
    EXPECT_FALSE(MatchesTerrainCompositedGeneratorClosure(expected, actual));

    actual = Manifest();
    AddKernel(actual, "extra", "terrain/terrainCompositedBakeWorld.comp",
        worldSource, {sharedInclude}, Hash("extra runtime spv"), Hash("extra debug spv"));
    EXPECT_FALSE(MatchesTerrainCompositedGeneratorClosure(expected, actual));
}

TEST_F(TerrainCompositedProductionIdentityTest, ChangedIncludeSourceStillFails)
{
    auto actual = Manifest();
    actual.artifacts.at("world").dependencies.front().digest = Hash("changed include source");
    EXPECT_FALSE(MatchesTerrainCompositedGeneratorClosure(expected, actual));
}
}
