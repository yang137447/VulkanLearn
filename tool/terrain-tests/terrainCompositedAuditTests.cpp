#include <gtest/gtest.h>

#include "terrain/terrainCompositedDependencies.h"
#include "terrain/terrainCompositedVTStore.h"
#include "terrain/terrainHeightAtlasLayout.h"
#include "terrainCompositedProductionFixture.h"

#include <atomic>
#include <chrono>
#include <fstream>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

namespace
{
namespace fs = std::filesystem;
using Json = nlohmann::json;
using namespace VL;

class TerrainCompositedAudit : public testing::Test
{
protected:
    void SetUp() override
    {
        static std::atomic<uint64_t> sequence{0};
        directory = fs::temp_directory_path() /
            ("vl-composited-audit-" +
             std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()) +
             "-" + std::to_string(sequence++));
        roots = {directory / "resources", directory / "project"};
        fs::create_directories(roots.resourceRoot);
        fs::create_directories(roots.projectRoot);
    }
    void TearDown() override
    {
        std::error_code error;
        fs::remove_all(directory, error);
    }
    void Write(const fs::path& path, const std::string& bytes)
    {
        fs::create_directories(path.parent_path());
        std::ofstream output(path, std::ios::binary | std::ios::trunc);
        output.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
        ASSERT_TRUE(output.good());
    }
    void Authors()
    {
        plan.desc.assetPath = "Maps/TR_audit.json";
        plan.desc.heightmapPath = "Maps/height.bin";
        plan.desc.materialInstancePath = "Maps/MI_audit.json";
        plan.placementId = "terrain|audit";
        Write(roots.resourceRoot / plan.desc.assetPath,
            Json{{"type", "terrain"}, {"heightmapPath", plan.desc.heightmapPath},
                {"materialInstancePath", plan.desc.materialInstancePath}}.dump());
        Write(roots.resourceRoot / plan.desc.heightmapPath, "height");
        Write(roots.resourceRoot / plan.desc.materialInstancePath,
            Json{{"type", "materialInstance"}, {"material", "shader/glsl/M_audit.json"}}.dump());
        material = {{"type", "material"}, {"shaderEvaluation",
            {{"vertex", "M_audit.vertex.glsl"}, {"surface", "M_audit.surface.glsl"}}}};
        Write(roots.projectRoot / "shader/glsl/M_audit.json", material.dump());
        Write(roots.projectRoot / "shader/glsl/M_audit.vertex.glsl", "void vertex() {}");
        Write(roots.projectRoot / "shader/glsl/M_audit.surface.glsl",
            "#include \"materialFunction/audit.glsl\"\nvoid surface() {}\n");
        Write(roots.projectRoot / "shader/glsl/materialFunction/audit.glsl", "float value = 0.0;");
    }
    Json Dependencies()
    {
        return BuildTerrainCompositedDependencies(
            roots, plan, placement, plan.desc.materialInstancePath, Json{{"generatorVersion", 1}});
    }
    TerrainCompositedVTStore Package(uint32_t resolution)
    {
        TerrainCompositedVTBuildDesc desc;
        desc.surfaceId = "audit";
        desc.width = desc.height = resolution;
        desc.encoding = "rgba8-audit";
        desc.channels = {TerrainCompositedVTFormat::RGBA8UNorm};
        const auto layout = desc.Layout();
        std::vector<TerrainCompositedVTPageData> pages;
        for (uint32_t mip = 0; mip < layout.MipCount(); ++mip)
            for (uint32_t y = 0; y < layout.Mip(mip).pagesY; ++y)
                for (uint32_t x = 0; x < layout.Mip(mip).pagesX; ++x)
                    pages.push_back({{"audit", mip, x, y},
                        std::vector<uint8_t>(desc.PageBytes(), static_cast<uint8_t>(mip))});
        TerrainCompositedVTStore::Publish(directory / "store.json",
            TerrainProductionFixture::Publication(directory, {desc}, pages), pages);
        return TerrainCompositedVTStore::Open(directory / "store.json");
    }
    fs::path directory;
    TerrainCompositedDependencyRoots roots;
    TerrainBuildPlan plan;
    Json material;
    std::array<float, 16> placement{1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};
};

TEST_F(TerrainCompositedAudit, IncludedShaderEditMustInvalidateCook)
{
    Authors();
    const auto before = Dependencies();
    Write(roots.projectRoot / "shader/glsl/materialFunction/audit.glsl", "float value = 1.0;");
    const auto after = Dependencies();
    EXPECT_NE(before.at("hash"), after.at("hash"))
        << "Transitive shader include changed without changing Cook identity";
    EXPECT_THROW(ValidateTerrainCompositedAuthors(roots, before), std::runtime_error);
}

TEST_F(TerrainCompositedAudit, NewlyShadowingResourceMaterialMustInvalidateCook)
{
    Authors();
    const auto before = Dependencies();
    Write(roots.resourceRoot / "shader/glsl/M_audit.json", material.dump());
    Write(roots.resourceRoot / "shader/glsl/M_audit.vertex.glsl", "different vertex");
    Write(roots.resourceRoot / "shader/glsl/M_audit.surface.glsl", "different surface");
    EXPECT_NE(before.at("hash"), Dependencies().at("hash"));
    EXPECT_THROW(ValidateTerrainCompositedAuthors(roots, before), std::runtime_error)
        << "Resource material now wins resolution, but old project dependency still validates";
}

TEST_F(TerrainCompositedAudit, OddExtentPageMayOverlapTwoParentPages)
{
    const TerrainCompositedVTLayout layout("odd", 513, 257);
    const TerrainCompositedVTPageKey child{"odd", 0, 2, 0};
    const auto childUV = layout.PageUVRect(child);
    const auto parent = layout.Parent(child);
    EXPECT_EQ(parent, (TerrainCompositedVTPageKey{"odd", 2, 0, 0}));
    const auto parentUV = layout.PageUVRect(parent);
    EXPECT_LE(parentUV.minU, childUV.minU)
        << "Covering ancestor must include the entire original child UV domain";
    EXPECT_GE(parentUV.maxU, childUV.maxU);
}

TEST_F(TerrainCompositedAudit, ParentCoversAllRectangularOddPageDomainsUsingExactIntegerBounds)
{
    for (const auto dimensions : {
        std::pair<uint32_t, uint32_t>{129, 131}, {259, 131}, {513, 257}, {1023, 511}})
    {
        const TerrainCompositedVTLayout layout("odd", dimensions.first, dimensions.second);
        for (uint32_t mip = 0; mip + 1 < layout.MipCount(); ++mip)
        {
            const auto childMip = layout.Mip(mip);
            for (uint32_t y = 0; y < childMip.pagesY; ++y)
                for (uint32_t x = 0; x < childMip.pagesX; ++x)
                {
                    const TerrainCompositedVTPageKey child{"odd", mip, x, y};
                    const auto parent = layout.Parent(child);
                    ASSERT_GT(parent.mip, mip);
                    ASSERT_TRUE(layout.Contains(parent));
                    const auto parentMip = layout.Mip(parent.mip);
                    const auto childSize = layout.ValidExtent(child);
                    const auto parentSize = layout.ValidExtent(parent);
                    const uint64_t childX = uint64_t(x) * 128;
                    const uint64_t childY = uint64_t(y) * 128;
                    const uint64_t parentX = uint64_t(parent.x) * 128;
                    const uint64_t parentY = uint64_t(parent.y) * 128;
                    EXPECT_LE(parentX * childMip.width, childX * parentMip.width);
                    EXPECT_LE(parentY * childMip.height, childY * parentMip.height);
                    EXPECT_GE((parentX + parentSize.width) * childMip.width,
                        (childX + childSize.width) * parentMip.width);
                    EXPECT_GE((parentY + parentSize.height) * childMip.height,
                        (childY + childSize.height) * parentMip.height);
                }
        }
    }
}

TEST_F(TerrainCompositedAudit, TinyHeightSourcesExposeUnbudgetedCopyRegionExplosion)
{
    const auto layout = BuildTerrainHeightAtlasLayout(
        {{0, 1, 1, 900}}, {{0, 0, 0}}, UINT64_MAX);
    const auto copies = BuildTerrainHeightAtlasCopies(layout, 0);
    EXPECT_EQ(copies.size(), 136u * 136u);
    std::vector<TerrainHeightAtlasLayer> layers;
    for (uint32_t layer = 0; layer < 900; ++layer)
        layers.push_back({layer, 0, layer});
    const auto full = BuildTerrainHeightAtlasLayout(
        {{0, 1, 1, 900}}, layers, 32u * 1024u * 1024u);
    RecordProperty("gpuPayloadBytes", full.totalBytes);
    RecordProperty("copyRegionCount", uint64_t(full.pages.size()) * copies.size());
    EXPECT_GT(uint64_t(full.pages.size()) * copies.size(), 16000000u);
    EXPECT_LT(full.totalBytes, 32u * 1024u * 1024u);
}

TEST_F(TerrainCompositedAudit, StoreRejectsMissingFinalOneTexelPage)
{
    const auto store = Package(256);
    Json manifest;
    {
        std::ifstream input(directory / "store.json");
        input >> manifest;
    }
    ASSERT_GT(store.Surfaces()[0].Layout().MipCount(),
        store.Surfaces()[0].Layout().TailMip() + 1);
    manifest["pages"].erase(manifest["pages"].end() - 1);
    Write(directory / "store.json", manifest.dump());
    EXPECT_THROW(TerrainCompositedVTStore::Open(directory / "store.json"), std::runtime_error);
}

TEST_F(TerrainCompositedAudit, CorruptFinePageDoesNotPreventExistingStoreReadingBaseline)
{
    const auto store = Package(256);
    const auto mip = store.Surfaces()[0].Layout().MipCount() - 1;
    const auto baseline = store.ReadPage({"audit", mip, 0, 0});
    {
        std::fstream package(store.PackagePath(), std::ios::binary | std::ios::in | std::ios::out);
        package.seekp(static_cast<std::streamoff>(store.Pages().at({"audit", 0, 0, 0}).offset));
        package.put(static_cast<char>(0xff));
    }
    EXPECT_EQ(store.ReadPage({"audit", mip, 0, 0}), baseline);
    EXPECT_NO_THROW(TerrainCompositedVTStore::Open(directory / "store.json"));
    EXPECT_THROW(store.ValidateAll(), std::runtime_error);
}

TEST_F(TerrainCompositedAudit, StoreOpenDoesNotScanFinePayloadAndInstallationDoes)
{
#ifdef _WIN32
    const auto store = Package(1024);
    IO_COUNTERS before{};
    IO_COUNTERS after{};
    ASSERT_TRUE(GetProcessIoCounters(GetCurrentProcess(), &before));
    const auto opened = TerrainCompositedVTStore::Open(directory / "store.json");
    ASSERT_TRUE(GetProcessIoCounters(GetCurrentProcess(), &after));
    const auto packageBytes = fs::file_size(store.PackagePath());
    const auto readBytes = after.ReadTransferCount - before.ReadTransferCount;
    RecordProperty("packageBytes", packageBytes);
    RecordProperty("openReadBytes", readBytes);
    EXPECT_LT(readBytes, packageBytes);
    EXPECT_LE(readBytes, fs::file_size(directory / "store.json") + 64 * 1024);
    EXPECT_NO_THROW(opened.ValidateAll());
    EXPECT_EQ(opened.Pages().size(), store.Pages().size());
#else
    GTEST_SKIP() << "Requires Windows per-process IO counters";
#endif
}
}
