#include <gtest/gtest.h>

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

#include "editor/terrain/terrainPaintSession.h"
#include "shader/build/atomicFile.h"
#include "terrain/terrainCollisionTypes.h"
#include "terrain/terrainPaintFile.h"
#include "terrain/terrainPaintManifest.h"
#include "terrain/terrainPaintSha256.h"

namespace
{

class TerrainPaintManifestTest : public testing::Test
{
protected:
    void SetUp() override
    {
        root = std::filesystem::temp_directory_path() /
            "vl-terrain-paint-manifest-tests";
        std::error_code ignored;
        std::filesystem::remove_all(root, ignored);
        std::filesystem::create_directories(root);
    }

    void TearDown() override
    {
        std::error_code ignored;
        std::filesystem::remove_all(root, ignored);
    }

    std::filesystem::path paintPath() const
    {
        return root / "terrain_tile_0_0_paint.vlpaint";
    }

    static VL::TerrainPaintManifest MakeManifest()
    {
        VL::TerrainPaintManifest manifest;
        manifest.version = 2;
        manifest.tileX = 1;
        manifest.tileY = 2;
        manifest.tileGrid = 4;
        manifest.width = 512;
        manifest.height = 512;
        manifest.layerCount = 3;
        manifest.layerKeys = {"ground", "rock", "water"};
        manifest.featherTexels = 4.0f;
        manifest.contentSha256 = std::string(64, 'a');
        return manifest;
    }

    static nlohmann::json ManifestJson()
    {
        return nlohmann::json{
            {"version", 2},
            {"kind", "terrainLayerWeights"},
            {"tileX", 1},
            {"tileY", 2},
            {"tileGrid", 4},
            {"width", 512},
            {"height", 512},
            {"layerCount", 3},
            {"layerKeys", {"ground", "rock", "water"}},
            {"featherTexels", 4.0},
            {"contentSha256", "abc123"},
        };
    }

    void WriteJson(const nlohmann::json& document) const
    {
        const std::filesystem::path path =
            VL::TerrainPaintManifestPath(paintPath());
        std::ofstream output(path, std::ios::binary | std::ios::trunc);
        ASSERT_TRUE(output.is_open());
        output << document.dump(2) << "\n";
    }

    void WriteRaw(const std::string& text) const
    {
        const std::filesystem::path path =
            VL::TerrainPaintManifestPath(paintPath());
        std::ofstream output(path, std::ios::binary | std::ios::trunc);
        ASSERT_TRUE(output.is_open());
        output << text;
    }

    std::filesystem::path root;
};

VL::TerrainAssetDesc MakeLayeredDesc()
{
    VL::TerrainAssetDesc desc;
    desc.assetPath = "Maps/SC_test/TR_test.json";
    VL::TerrainLayerDesc explicitKey;
    explicitKey.name = "Gravel";
    explicitKey.key = "kitedemo.Gravel";
    VL::TerrainLayerDesc fallbackKey;
    fallbackKey.name = "Grass";
    desc.layers = {explicitKey, fallbackKey};
    return desc;
}

VL::TerrainLayerPaintData MakePaint(
    uint32_t width = 4,
    uint32_t layerCount = 2)
{
    VL::TerrainLayerPaintData paint;
    paint.width = width;
    paint.height = width;
    paint.layerCount = layerCount;
    const size_t count =
        static_cast<size_t>(width) * width * layerCount;
    paint.weights.resize(count);
    paint.opacity.resize(count);
    for (size_t index = 0; index < count; ++index)
    {
        paint.weights[index] =
            static_cast<float>(index % 9) / 8.0f;
        paint.opacity[index] = static_cast<uint8_t>((index * 17) % 256);
    }
    return paint;
}

} // namespace

TEST_F(TerrainPaintManifestTest, SaveLoadValidateRoundTrip)
{
    const VL::TerrainPaintManifest manifest = MakeManifest();
    std::string error;
    ASSERT_TRUE(VL::SaveTerrainPaintManifest(paintPath(), manifest, error))
        << error;

    EXPECT_TRUE(std::filesystem::exists(
        VL::TerrainPaintManifestPath(paintPath())));
    EXPECT_TRUE(VL::TerrainPaintManifestExists(paintPath(), error)) << error;

    VL::TerrainPaintManifest loaded;
    ASSERT_TRUE(VL::LoadTerrainPaintManifest(paintPath(), loaded, error))
        << error;
    EXPECT_EQ(loaded.version, manifest.version);
    EXPECT_EQ(loaded.tileX, manifest.tileX);
    EXPECT_EQ(loaded.tileY, manifest.tileY);
    EXPECT_EQ(loaded.tileGrid, manifest.tileGrid);
    EXPECT_EQ(loaded.width, manifest.width);
    EXPECT_EQ(loaded.height, manifest.height);
    EXPECT_EQ(loaded.layerCount, manifest.layerCount);
    EXPECT_EQ(loaded.layerKeys, manifest.layerKeys);
    EXPECT_FLOAT_EQ(loaded.featherTexels, manifest.featherTexels);
    EXPECT_EQ(loaded.contentSha256, manifest.contentSha256);

    EXPECT_TRUE(VL::ValidateTerrainPaintManifest(
        loaded, manifest.layerKeys, manifest.width, manifest.height, error))
        << error;
}

TEST_F(TerrainPaintManifestTest, LayerKeyMismatchReportsFirstDifference)
{
    VL::TerrainPaintManifest manifest = MakeManifest();
    manifest.layerKeys = {"ground", "wrong", "water"};
    std::string error;
    EXPECT_FALSE(VL::ValidateTerrainPaintManifest(
        manifest, {"ground", "rock", "water"}, 512, 512, error));
    EXPECT_NE(error.find("layer 1"), std::string::npos) << error;
    EXPECT_NE(error.find("expected 'rock'"), std::string::npos) << error;
    EXPECT_NE(error.find("got 'wrong'"), std::string::npos) << error;

    manifest.layerKeys = {"rock", "ground", "water"};
    EXPECT_FALSE(VL::ValidateTerrainPaintManifest(
        manifest, {"ground", "rock", "water"}, 512, 512, error));
    EXPECT_NE(error.find("layer 0"), std::string::npos) << error;
    EXPECT_NE(error.find("expected 'ground'"), std::string::npos) << error;
    EXPECT_NE(error.find("got 'rock'"), std::string::npos) << error;
}

TEST_F(TerrainPaintManifestTest, ShapeMismatchFails)
{
    const VL::TerrainPaintManifest manifest = MakeManifest();
    std::string error;

    VL::TerrainPaintManifest wrongCount = manifest;
    wrongCount.layerCount = 4;
    EXPECT_FALSE(VL::ValidateTerrainPaintManifest(
        wrongCount, {"ground", "rock", "water"}, 512, 512, error));
    EXPECT_NE(error.find("layerCount"), std::string::npos) << error;

    VL::TerrainPaintManifest wrongWidth = manifest;
    wrongWidth.width = 256;
    EXPECT_FALSE(VL::ValidateTerrainPaintManifest(
        wrongWidth, {"ground", "rock", "water"}, 512, 512, error));
    EXPECT_NE(error.find("width"), std::string::npos) << error;

    VL::TerrainPaintManifest wrongHeight = manifest;
    wrongHeight.height = 1024;
    EXPECT_FALSE(VL::ValidateTerrainPaintManifest(
        wrongHeight, {"ground", "rock", "water"}, 512, 512, error));
    EXPECT_NE(error.find("height"), std::string::npos) << error;
}

TEST_F(TerrainPaintManifestTest, LoadRejectsWrongVersionOrKind)
{
    std::string error;
    VL::TerrainPaintManifest loaded;

    nlohmann::json wrongVersion = ManifestJson();
    wrongVersion["version"] = 1;
    WriteJson(wrongVersion);
    EXPECT_FALSE(VL::LoadTerrainPaintManifest(paintPath(), loaded, error));
    EXPECT_NE(error.find("version 1"), std::string::npos) << error;

    nlohmann::json wrongKind = ManifestJson();
    wrongKind["kind"] = "somethingElse";
    WriteJson(wrongKind);
    EXPECT_FALSE(VL::LoadTerrainPaintManifest(paintPath(), loaded, error));
    EXPECT_NE(error.find("kind"), std::string::npos) << error;
}

TEST_F(TerrainPaintManifestTest, LoadRejectsCorruptOrMissingFields)
{
    std::string error;
    VL::TerrainPaintManifest loaded;

    WriteRaw("{ this is not json");
    EXPECT_FALSE(VL::LoadTerrainPaintManifest(paintPath(), loaded, error));
    EXPECT_FALSE(error.empty());

    nlohmann::json missing = ManifestJson();
    missing.erase("layerKeys");
    WriteJson(missing);
    EXPECT_FALSE(VL::LoadTerrainPaintManifest(paintPath(), loaded, error));
    EXPECT_NE(error.find("layerKeys"), std::string::npos) << error;

    nlohmann::json wrongType = ManifestJson();
    wrongType["width"] = "512";
    WriteJson(wrongType);
    EXPECT_FALSE(VL::LoadTerrainPaintManifest(paintPath(), loaded, error));
    EXPECT_NE(error.find("width"), std::string::npos) << error;
}

TEST_F(TerrainPaintManifestTest, MissingSidecarIsReported)
{
    std::string error;
    VL::TerrainPaintManifest loaded;
    EXPECT_FALSE(VL::TerrainPaintManifestExists(paintPath(), error));
    EXPECT_TRUE(error.empty()) << error;
    EXPECT_FALSE(VL::LoadTerrainPaintManifest(paintPath(), loaded, error));
    EXPECT_FALSE(error.empty());
}

TEST_F(TerrainPaintManifestTest, LayerKeyCountMismatchReportsMissingAndExtra)
{
    VL::TerrainPaintManifest missing = MakeManifest();
    missing.layerKeys = {"ground", "rock"};
    std::string error;
    EXPECT_FALSE(VL::ValidateTerrainPaintManifest(
        missing, {"ground", "rock", "water"}, 512, 512, error));
    EXPECT_NE(error.find("layer 2"), std::string::npos) << error;
    EXPECT_NE(error.find("expected 'water'"), std::string::npos) << error;
    EXPECT_NE(error.find("got '<missing>'"), std::string::npos) << error;

    VL::TerrainPaintManifest extra = MakeManifest();
    extra.layerKeys = {"ground", "rock", "water", "stray"};
    EXPECT_FALSE(VL::ValidateTerrainPaintManifest(
        extra, {"ground", "rock", "water"}, 512, 512, error));
    EXPECT_NE(error.find("layer 3"), std::string::npos) << error;
    EXPECT_NE(error.find("expected '<missing>'"), std::string::npos) << error;
    EXPECT_NE(error.find("'stray'"), std::string::npos) << error;
}

TEST_F(TerrainPaintManifestTest, LayerKeysPreferExplicitKeyAndFallBackToAssetPath)
{
    const VL::TerrainAssetDesc desc = MakeLayeredDesc();
    const std::vector<std::string> keys = VL::TerrainPaintLayerKeys(desc);
    ASSERT_EQ(keys.size(), 2u);
    EXPECT_EQ(keys[0], "kitedemo.Gravel");
    EXPECT_EQ(keys[1], "Maps/SC_test/TR_test.json#Grass");
}

TEST_F(TerrainPaintManifestTest, LayerKeysNormalizeBackslashAssetPath)
{
    VL::TerrainAssetDesc desc = MakeLayeredDesc();
    desc.assetPath = "Maps\\SC_test\\TR_test.json";
    const std::vector<std::string> keys = VL::TerrainPaintLayerKeys(desc);
    ASSERT_EQ(keys.size(), 2u);
    EXPECT_EQ(keys[1], "Maps/SC_test/TR_test.json#Grass");
}

TEST_F(TerrainPaintManifestTest, ExistsReportsQueryError)
{
    std::string error;
#ifdef _WIN32
    // 非法字符让 GetFileAttributesW 返回 ERROR_INVALID_NAME，而不是
    // FILE_NOT_FOUND/PATH_NOT_FOUND。
    const bool exists = VL::TerrainPaintManifestExists(
        root / "bad<>.vlpaint", error);
    EXPECT_FALSE(exists);
    if (error.empty())
    {
        GTEST_SKIP() << "filesystem maps the invalid path to not-found";
    }
#else
    const std::filesystem::path blocker = root / "blocker";
    {
        std::ofstream output(blocker, std::ios::binary | std::ios::trunc);
        ASSERT_TRUE(output.is_open());
        output << "not a directory";
    }
    EXPECT_FALSE(VL::TerrainPaintManifestExists(
        blocker / "paint.vlpaint", error));
    EXPECT_FALSE(error.empty());
#endif
}

TEST_F(TerrainPaintManifestTest, BuildPaintBytesMatchesSavedFile)
{
    const VL::TerrainLayerPaintData paint = MakePaint(8, 3);
    std::string error;
    std::vector<uint8_t> built;
    ASSERT_TRUE(VL::BuildTerrainLayerPaintBytes(paint, built, error))
        << error;
    ASSERT_TRUE(VL::SaveTerrainLayerPaintFile(paintPath(), paint, error))
        << error;
    const std::vector<uint8_t> saved = VL::ReadBinaryFile(paintPath());
    EXPECT_EQ(built, saved);
}

TEST_F(TerrainPaintManifestTest, BuildPaintBytesMatchesHardcodedGolden)
{
    VL::TerrainLayerPaintData paint;
    paint.width = 2;
    paint.height = 2;
    paint.layerCount = 2;
    paint.weights = {
        0.0f, 0.25f, 0.5f, 0.75f,
        1.0f, 0.5f, 0.25f, 0.0f};
    paint.opacity = {0, 1, 2, 3, 255, 254, 253, 252};

    std::string error;
    std::vector<uint8_t> built;
    ASSERT_TRUE(VL::BuildTerrainLayerPaintBytes(paint, built, error))
        << error;
    const std::vector<uint8_t> expected = {
        'V', 'L', 'P', '1',
        0x02, 0x00, 0x00, 0x00,
        0x02, 0x00, 0x00, 0x00,
        0x02, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x80, 0x3E,
        0x00, 0x00, 0x00, 0x3F,
        0x00, 0x00, 0x40, 0x3F,
        0x00, 0x00, 0x80, 0x3F,
        0x00, 0x00, 0x00, 0x3F,
        0x00, 0x00, 0x80, 0x3E,
        0x00, 0x00, 0x00, 0x00,
        0x00, 0x01, 0x02, 0x03,
        0xFF, 0xFE, 0xFD, 0xFC,
    };
    ASSERT_EQ(expected.size(), 64u);
    EXPECT_EQ(built, expected);
}

TEST_F(TerrainPaintManifestTest, BundleWritesPaintAndManifestWithContentHash)
{
    const VL::TerrainLayerPaintData paint = MakePaint(4, 2);
    VL::TerrainPaintManifest manifest = MakeManifest();
    manifest.width = paint.width;
    manifest.height = paint.height;
    manifest.layerCount = paint.layerCount;
    manifest.layerKeys = {"ground", "rock"};
    manifest.contentSha256.clear();

    std::string error;
    ASSERT_TRUE(VL::SaveTerrainLayerPaintBundle(
        paintPath(), paint, manifest, error)) << error;

    VL::TerrainLayerPaintData loadedPaint;
    ASSERT_TRUE(VL::LoadTerrainLayerPaintFile(
        paintPath(), paint.layerCount, loadedPaint, error)) << error;
    EXPECT_EQ(loadedPaint.weights, paint.weights);
    EXPECT_EQ(loadedPaint.opacity, paint.opacity);

    VL::TerrainPaintManifest loadedManifest;
    ASSERT_TRUE(VL::LoadTerrainPaintManifest(
        paintPath(), loadedManifest, error)) << error;
    EXPECT_EQ(loadedManifest.tileX, manifest.tileX);
    EXPECT_EQ(loadedManifest.tileY, manifest.tileY);
    EXPECT_EQ(loadedManifest.tileGrid, manifest.tileGrid);
    EXPECT_FLOAT_EQ(loadedManifest.featherTexels, manifest.featherTexels);
    EXPECT_EQ(loadedManifest.width, paint.width);
    EXPECT_EQ(loadedManifest.height, paint.height);
    EXPECT_EQ(loadedManifest.layerCount, paint.layerCount);
    EXPECT_EQ(loadedManifest.contentSha256,
        VL::TerrainPaintSha256Hex(VL::ReadBinaryFile(paintPath())));
    EXPECT_FALSE(loadedManifest.contentSha256.empty());
    EXPECT_TRUE(VL::ValidateTerrainPaintManifest(
        loadedManifest,
        manifest.layerKeys,
        paint.width,
        paint.height,
        error)) << error;
}

TEST_F(TerrainPaintManifestTest, BundleRejectsLayerKeysShapeMismatch)
{
    const VL::TerrainLayerPaintData paint = MakePaint(4, 2);
    VL::TerrainPaintManifest manifest = MakeManifest();
    manifest.layerKeys = {"only-one"};
    std::string error;
    EXPECT_FALSE(VL::SaveTerrainLayerPaintBundle(
        paintPath(), paint, manifest, error));
    EXPECT_NE(error.find("layerKeys"), std::string::npos) << error;
    EXPECT_FALSE(std::filesystem::exists(paintPath()));
    EXPECT_FALSE(std::filesystem::exists(
        VL::TerrainPaintManifestPath(paintPath())));
}

#ifdef _WIN32

TEST_F(TerrainPaintManifestTest, BundleFailureKeepsOldPaintAndSidecar)
{
    const VL::TerrainLayerPaintData oldPaint = MakePaint(4, 2);
    VL::TerrainPaintManifest oldManifest = MakeManifest();
    oldManifest.width = oldPaint.width;
    oldManifest.height = oldPaint.height;
    oldManifest.layerCount = oldPaint.layerCount;
    oldManifest.layerKeys = {"ground", "rock"};
    std::string error;
    ASSERT_TRUE(VL::SaveTerrainLayerPaintFile(
        paintPath(), oldPaint, error)) << error;
    ASSERT_TRUE(VL::SaveTerrainPaintManifest(
        paintPath(), oldManifest, error)) << error;
    const std::vector<uint8_t> oldPaintBytes =
        VL::ReadBinaryFile(paintPath());
    const std::vector<uint8_t> oldManifestBytes =
        VL::ReadBinaryFile(VL::TerrainPaintManifestPath(paintPath()));

    const std::filesystem::path manifestPath =
        VL::TerrainPaintManifestPath(paintPath());
    HANDLE lock = CreateFileW(
        manifestPath.c_str(),
        GENERIC_READ,
        0,
        nullptr,
        OPEN_EXISTING,
        FILE_ATTRIBUTE_NORMAL,
        nullptr);
    ASSERT_NE(lock, INVALID_HANDLE_VALUE);

    VL::TerrainLayerPaintData newPaint = MakePaint(4, 2);
    newPaint.weights[0] = 0.125f;
    VL::TerrainPaintManifest newManifest = oldManifest;
    const bool saved = VL::SaveTerrainLayerPaintBundle(
        paintPath(), newPaint, newManifest, error);
    CloseHandle(lock);

    EXPECT_FALSE(saved);
    EXPECT_FALSE(error.empty());
    EXPECT_EQ(VL::ReadBinaryFile(paintPath()), oldPaintBytes);
    EXPECT_EQ(
        VL::ReadBinaryFile(manifestPath), oldManifestBytes);

    for (const auto& entry : std::filesystem::directory_iterator(root))
    {
        const std::string name = entry.path().filename().string();
        EXPECT_EQ(name.find(".tmp."), std::string::npos) << name;
        EXPECT_EQ(name.find(".rollback."), std::string::npos) << name;
    }
}

#else

TEST_F(TerrainPaintManifestTest, BundleFailureKeepsOldPaintAndSidecar)
{
    GTEST_SKIP() << "Windows share-mode lock test";
}

#endif

TEST_F(TerrainPaintManifestTest, BundlePreservesExistingTileIdentityAndRefreshesHash)
{
    VL::TerrainPaintManifest existing = MakeManifest();
    existing.width = 4;
    existing.height = 4;
    existing.layerCount = 2;
    existing.layerKeys = {"ground", "rock"};
    existing.contentSha256 = std::string(64, 'b');
    std::string error;
    ASSERT_TRUE(VL::SaveTerrainPaintManifest(
        paintPath(), existing, error)) << error;

    VL::TerrainPaintManifest manifest;
    ASSERT_TRUE(VL::LoadTerrainPaintManifest(
        paintPath(), manifest, error)) << error;
    const VL::TerrainLayerPaintData paint = MakePaint(8, 2);
    manifest.width = paint.width;
    manifest.height = paint.height;
    manifest.layerCount = paint.layerCount;
    manifest.layerKeys = {"ground", "rock"};
    manifest.contentSha256.clear();
    ASSERT_TRUE(VL::SaveTerrainLayerPaintBundle(
        paintPath(), paint, manifest, error)) << error;

    VL::TerrainPaintManifest loaded;
    ASSERT_TRUE(VL::LoadTerrainPaintManifest(
        paintPath(), loaded, error)) << error;
    EXPECT_EQ(loaded.tileX, existing.tileX);
    EXPECT_EQ(loaded.tileY, existing.tileY);
    EXPECT_EQ(loaded.tileGrid, existing.tileGrid);
    EXPECT_FLOAT_EQ(loaded.featherTexels, existing.featherTexels);
    EXPECT_EQ(loaded.width, paint.width);
    EXPECT_EQ(loaded.height, paint.height);
    EXPECT_EQ(loaded.layerCount, paint.layerCount);
    EXPECT_NE(loaded.contentSha256, existing.contentSha256);
    EXPECT_EQ(loaded.contentSha256,
        VL::TerrainPaintSha256Hex(VL::ReadBinaryFile(paintPath())));
}

TEST_F(TerrainPaintManifestTest, SaveRejectsInvalidManifestShape)
{
    std::string error;

    VL::TerrainPaintManifest wrongVersion = MakeManifest();
    wrongVersion.version = 1;
    EXPECT_FALSE(VL::SaveTerrainPaintManifest(
        paintPath(), wrongVersion, error));
    EXPECT_NE(error.find("version"), std::string::npos) << error;

    VL::TerrainPaintManifest zeroWidth = MakeManifest();
    zeroWidth.width = 0;
    EXPECT_FALSE(VL::SaveTerrainPaintManifest(
        paintPath(), zeroWidth, error));
    EXPECT_NE(error.find("width"), std::string::npos) << error;

    VL::TerrainPaintManifest nonSquare = MakeManifest();
    nonSquare.height = 256;
    EXPECT_FALSE(VL::SaveTerrainPaintManifest(
        paintPath(), nonSquare, error));
    EXPECT_NE(error.find("non-square"), std::string::npos) << error;

    VL::TerrainPaintManifest tooLarge = MakeManifest();
    tooLarge.width = 4097;
    tooLarge.height = 4097;
    EXPECT_FALSE(VL::SaveTerrainPaintManifest(
        paintPath(), tooLarge, error));
    EXPECT_NE(error.find("4096"), std::string::npos) << error;

    VL::TerrainPaintManifest countMismatch = MakeManifest();
    countMismatch.layerKeys = {"ground", "rock"};
    EXPECT_FALSE(VL::SaveTerrainPaintManifest(
        paintPath(), countMismatch, error));
    EXPECT_NE(error.find("layerKeys"), std::string::npos) << error;

    VL::TerrainPaintManifest tooManyLayers = MakeManifest();
    tooManyLayers.layerCount = 257;
    tooManyLayers.layerKeys.assign(257, "layer");
    EXPECT_FALSE(VL::SaveTerrainPaintManifest(
        paintPath(), tooManyLayers, error));
    EXPECT_NE(error.find("256"), std::string::npos) << error;

    VL::TerrainPaintManifest zeroLayers = MakeManifest();
    zeroLayers.layerCount = 0;
    zeroLayers.layerKeys.clear();
    EXPECT_FALSE(VL::SaveTerrainPaintManifest(
        paintPath(), zeroLayers, error));
    EXPECT_NE(error.find("layerCount"), std::string::npos) << error;

    VL::TerrainPaintManifest zeroGrid = MakeManifest();
    zeroGrid.tileGrid = 0;
    EXPECT_FALSE(VL::SaveTerrainPaintManifest(
        paintPath(), zeroGrid, error));
    EXPECT_NE(error.find("tileGrid"), std::string::npos) << error;

    VL::TerrainPaintManifest badTileX = MakeManifest();
    badTileX.tileX = badTileX.tileGrid;
    EXPECT_FALSE(VL::SaveTerrainPaintManifest(
        paintPath(), badTileX, error));
    EXPECT_NE(error.find("tileX"), std::string::npos) << error;

    VL::TerrainPaintManifest badTileY = MakeManifest();
    badTileY.tileY = badTileY.tileGrid;
    EXPECT_FALSE(VL::SaveTerrainPaintManifest(
        paintPath(), badTileY, error));
    EXPECT_NE(error.find("tileY"), std::string::npos) << error;

    VL::TerrainPaintManifest negativeFeather = MakeManifest();
    negativeFeather.featherTexels = -0.5f;
    EXPECT_FALSE(VL::SaveTerrainPaintManifest(
        paintPath(), negativeFeather, error));
    EXPECT_NE(error.find("featherTexels"), std::string::npos) << error;

    VL::TerrainPaintManifest shortHash = MakeManifest();
    shortHash.contentSha256 = std::string(63, 'a');
    EXPECT_FALSE(VL::SaveTerrainPaintManifest(
        paintPath(), shortHash, error));
    EXPECT_NE(error.find("contentSha256"), std::string::npos) << error;

    VL::TerrainPaintManifest nonHexHash = MakeManifest();
    nonHexHash.contentSha256 = std::string(64, 'z');
    EXPECT_FALSE(VL::SaveTerrainPaintManifest(
        paintPath(), nonHexHash, error));
    EXPECT_NE(error.find("contentSha256"), std::string::npos) << error;
}

TEST_F(TerrainPaintManifestTest, Sha256KnownVectors)
{
    EXPECT_EQ(
        VL::TerrainPaintSha256Hex({}),
        "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855");
    const std::vector<uint8_t> abc = {'a', 'b', 'c'};
    EXPECT_EQ(
        VL::TerrainPaintSha256Hex(abc),
        "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");

    const std::vector<uint8_t> million(1000000, static_cast<uint8_t>('a'));
    EXPECT_EQ(
        VL::TerrainPaintSha256Hex(million),
        "cdc76e5c9914fb9281a1c7e284d73e67f1809a48a497200e046d39ccc7112cd0");

    const std::vector<uint8_t> remainder(63, static_cast<uint8_t>('a'));
    EXPECT_EQ(
        VL::TerrainPaintSha256Hex(remainder),
        "7d3e74a05d7db15bce4ad9ec0658ea98e3f06eeecf16b4c6fff2da457ddc2f34");
}

TEST_F(TerrainPaintManifestTest, PaintSessionLayerKeysFollowTerrainOrder)
{
    VL::TerrainCollisionInstance instance;
    instance.placementId = "terrain|Landscape";
    instance.plan.desc.assetPath = "Maps\\SC_test\\TR_test.json";
    VL::TerrainLayerDesc explicitKey;
    explicitKey.name = "Gravel";
    explicitKey.key = "kitedemo.Gravel";
    VL::TerrainLayerDesc fallbackKey;
    fallbackKey.name = "Grass";
    instance.plan.desc.layers = {explicitKey, fallbackKey};
    instance.plan.placementId = instance.placementId;

    VL::Editor::TerrainPaintSession session;
    std::string error;
    ASSERT_TRUE(VL::Editor::TerrainPaintSession::Create(
        instance, session, error)) << error;
    const std::vector<std::string>& keys = session.LayerKeys();
    ASSERT_EQ(keys.size(), 2u);
    EXPECT_EQ(keys[0], "kitedemo.Gravel");
    EXPECT_EQ(keys[1], "Maps/SC_test/TR_test.json#Grass");
}
