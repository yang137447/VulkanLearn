#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <cstring>
#include <fstream>
#include <functional>
#include <iterator>
#include <limits>

#include <nlohmann/json.hpp>

#include "shader/build/contentHash.h"
#include "terrain/terrainHeightAtlasStore.h"

namespace
{
namespace fs = std::filesystem;
using Json = nlohmann::json;
using Layout = VL::TerrainHeightAtlasLayout;

class TerrainHeightAtlasStoreTest : public testing::Test
{
protected:
    void SetUp() override
    {
        static std::atomic<uint64_t> sequence{0};
        root = fs::temp_directory_path() / ("vl-height-atlas-store-" +
            std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()) +
            "-" + std::to_string(sequence++));
        fs::create_directories(root);
        path = root / "surface.height.json";
        const auto layout = VL::BuildTerrainHeightAtlasLayout(
            {{4, 259, 137, 3}}, {{2, 4, 1}, {7, 4, 1}}, UINT64_MAX);
        data.desc = {layout.width, layout.height, static_cast<uint32_t>(layout.pages.size()),
            static_cast<uint32_t>(layout.layerMap.size())};
        data.layerMap = layout.layerMap;
        data.atlas.resize(static_cast<size_t>(layout.atlasBytes), 0);
        for (uint32_t pageIndex = 0; pageIndex < layout.pages.size(); ++pageIndex)
            for (const auto& copy : VL::BuildTerrainHeightAtlasCopies(layout, pageIndex))
                for (uint32_t offsetY = 0; offsetY < copy.height; ++offsetY)
                    for (uint32_t offsetX = 0; offsetX < copy.width; ++offsetX)
                    {
                        const uint16_t value = static_cast<uint16_t>(
                            (copy.sourceX + offsetX) * 317 + (copy.sourceY + offsetY) * 109 + 5309);
                        const auto offset = (uint64_t(copy.destinationY + offsetY) * layout.width +
                            copy.destinationX + offsetX) * 2;
                        data.atlas[offset] = static_cast<uint8_t>(value);
                        data.atlas[offset + 1] = static_cast<uint8_t>(value >> 8);
                    }
    }

    void TearDown() override
    {
        std::error_code error;
        fs::remove_all(root, error);
    }

    Json Manifest() const
    {
        std::ifstream input(path);
        return Json::parse(input);
    }

    static std::vector<uint8_t> Bytes(const fs::path& file)
    {
        std::ifstream input(file, std::ios::binary);
        return std::vector<uint8_t>(std::istreambuf_iterator<char>(input), {});
    }

    static void Write(const fs::path& file, const std::vector<uint8_t>& bytes)
    {
        std::ofstream output(file, std::ios::binary | std::ios::trunc);
        output.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
        ASSERT_TRUE(output.good());
    }

    void WriteManifest(const Json& manifest) const
    {
        std::ofstream output(path);
        output << manifest.dump(2);
        ASSERT_TRUE(output.good());
    }

    void Resign(const std::function<void(std::vector<uint8_t>&)>& change)
    {
        auto manifest = Manifest();
        auto bytes = Bytes(root / manifest.at("package").get<std::string>());
        change(bytes);
        const auto digest = VL::ContentHasher::HashBytes(bytes.data(), bytes.size()).ToHex();
        manifest["packageHash"] = digest;
        manifest["package"] = digest + ".height.bin";
        Write(root / manifest.at("package").get<std::string>(), bytes);
        WriteManifest(manifest);
    }

    uint64_t Budget() const
    {
        return VL::TerrainHeightAtlasStorePayloadBytes(data.desc, UINT64_MAX);
    }

    fs::path root;
    fs::path path;
    VL::TerrainHeightAtlasStoreData data;
};
}

TEST_F(TerrainHeightAtlasStoreTest, RoundTripsR16BytesSparseAndSharedLayerMap)
{
    ASSERT_NO_THROW(VL::WriteTerrainHeightAtlasStore(path, data));
    const auto loaded = VL::ReadTerrainHeightAtlasStore(path, Budget());
    EXPECT_EQ(loaded.atlas, data.atlas);
    EXPECT_EQ(loaded.layerMap, data.layerMap);
    EXPECT_EQ(loaded.desc.width, data.desc.width);
    EXPECT_EQ(loaded.desc.pageCount, data.desc.pageCount);
    EXPECT_EQ(loaded.packagePath, VL::TerrainHeightAtlasStorePackagePath(path));
    const auto manifest = Manifest();
    EXPECT_EQ(manifest.at("kind"), "terrain-tiling-height-atlas");
    EXPECT_EQ(manifest.at("package").get<std::string>(),
        manifest.at("packageHash").get<std::string>() + ".height.bin");
    EXPECT_EQ(manifest.at("packageBytes"), Budget() + 64);
    EXPECT_EQ(manifest.at("sourceMip"), 2);
    EXPECT_EQ(manifest.at("atlasFormat"), "r16-unorm");
    EXPECT_EQ(manifest.at("lutFormat"), "rgba32-float");
}

TEST_F(TerrainHeightAtlasStoreTest, PublishesStableContentAddressedBinaryAndManifest)
{
    VL::WriteTerrainHeightAtlasStore(path, data);
    const auto first = Bytes(path);
    const auto filename = Manifest().at("package").get<std::string>();
    VL::WriteTerrainHeightAtlasStore(path, data);
    EXPECT_EQ(first, Bytes(path));
    EXPECT_TRUE(fs::is_regular_file(root / filename));
    EXPECT_EQ(std::distance(fs::directory_iterator(root), fs::directory_iterator()), 2);
}

TEST_F(TerrainHeightAtlasStoreTest, ChecksBudgetBeforeReadingPayload)
{
    VL::WriteTerrainHeightAtlasStore(path, data);
    EXPECT_THROW(VL::ReadTerrainHeightAtlasStore(path, Budget() - 1), std::runtime_error);
    EXPECT_NO_THROW(VL::ReadTerrainHeightAtlasStore(path, Budget()));
    auto desc = data.desc;
    desc.width = UINT32_MAX;
    EXPECT_THROW(VL::TerrainHeightAtlasStorePayloadBytes(desc, UINT64_MAX), std::runtime_error);
    desc = {4080, 4080, 900, 4096};
    EXPECT_LT(VL::TerrainHeightAtlasStorePayloadBytes(desc, UINT64_MAX),
        VL::TerrainHeightAtlasStoreMaxPayloadBytes);
}

TEST_F(TerrainHeightAtlasStoreTest, RejectsWrongProtocolFormatAndExtraFields)
{
    VL::WriteTerrainHeightAtlasStore(path, data);
    const auto original = Manifest();
    for (const auto& field : {"kind", "atlasFormat", "lutFormat", "addressMode", "filter"})
    {
        auto changed = original;
        changed[field] = "unsupported";
        WriteManifest(changed);
        EXPECT_THROW(VL::ReadTerrainHeightAtlasStore(path, Budget()), std::runtime_error) << field;
    }
    auto changed = original;
    changed["extra"] = 1;
    WriteManifest(changed);
    EXPECT_THROW(VL::ReadTerrainHeightAtlasStore(path, Budget()), std::runtime_error);
}

TEST_F(TerrainHeightAtlasStoreTest, RejectsDuplicateManifestFields)
{
    VL::WriteTerrainHeightAtlasStore(path, data);
    auto text = Manifest().dump();
    text.insert(1, "\"version\":1,");
    Write(path, std::vector<uint8_t>(text.begin(), text.end()));
    EXPECT_THROW(VL::ReadTerrainHeightAtlasStore(path, Budget()), std::runtime_error);
}

TEST_F(TerrainHeightAtlasStoreTest, RejectsMissingTruncatedAndTrailingBinaryBytes)
{
    VL::WriteTerrainHeightAtlasStore(path, data);
    const auto binary = root / Manifest().at("package").get<std::string>();
    const auto original = Bytes(binary);
    auto changed = original;
    changed.pop_back();
    Write(binary, changed);
    EXPECT_THROW(VL::ReadTerrainHeightAtlasStore(path, Budget()), std::runtime_error);
    changed = original;
    changed.push_back(0);
    Write(binary, changed);
    EXPECT_THROW(VL::ReadTerrainHeightAtlasStore(path, Budget()), std::runtime_error);
    fs::remove(binary);
    EXPECT_THROW(VL::ReadTerrainHeightAtlasStore(path, Budget()), std::runtime_error);
}

TEST_F(TerrainHeightAtlasStoreTest, RejectsBinaryCorruptionAndForgedHeader)
{
    VL::WriteTerrainHeightAtlasStore(path, data);
    const auto binary = root / Manifest().at("package").get<std::string>();
    auto original = Bytes(binary);
    original[64] ^= 1;
    Write(binary, original);
    EXPECT_THROW(VL::ReadTerrainHeightAtlasStore(path, Budget()), std::runtime_error);
    original[64] ^= 1;
    Write(binary, original);
    Resign([](auto& bytes) { bytes[56] = 1; });
    EXPECT_THROW(VL::ReadTerrainHeightAtlasStore(path, Budget()), std::runtime_error);
}

TEST_F(TerrainHeightAtlasStoreTest, RejectsResignedRepeatBorderDamage)
{
    VL::WriteTerrainHeightAtlasStore(path, data);
    Resign([](auto& bytes) { bytes[64] ^= 1; });
    EXPECT_THROW(VL::ReadTerrainHeightAtlasStore(path, Budget()), std::runtime_error);
}

TEST_F(TerrainHeightAtlasStoreTest, RejectsResignedNonfiniteLutAndOutOfBoundsPages)
{
    VL::WriteTerrainHeightAtlasStore(path, data);
    const auto atlasBytes = data.atlas.size();
    Resign([&](auto& bytes)
    {
        const auto offset = 64 + atlasBytes + 2 * 16;
        bytes[offset] = 0;
        bytes[offset + 1] = 0;
        bytes[offset + 2] = 0xc0;
        bytes[offset + 3] = 0x7f;
    });
    EXPECT_THROW(VL::ReadTerrainHeightAtlasStore(path, Budget()), std::runtime_error);
    data.layerMap[2][0] = 899;
    EXPECT_THROW(VL::WriteTerrainHeightAtlasStore(path, data), std::runtime_error);
}

TEST_F(TerrainHeightAtlasStoreTest, RejectsUnmappedPagesAndNoncanonicalAllocation)
{
    data.layerMap[2] = {};
    data.layerMap[7] = {};
    EXPECT_THROW(VL::ValidateTerrainHeightAtlasStoreData(data, Budget()), std::runtime_error);
    auto desc = data.desc;
    desc.height += Layout::PageExtent;
    EXPECT_THROW(VL::TerrainHeightAtlasStorePayloadBytes(desc, UINT64_MAX), std::runtime_error);
}

TEST_F(TerrainHeightAtlasStoreTest, RejectsUnsafeBinaryNameAndMismatchedByteCounts)
{
    VL::WriteTerrainHeightAtlasStore(path, data);
    const auto original = Manifest();
    auto changed = original;
    changed["package"] = "../escape.height.bin";
    WriteManifest(changed);
    EXPECT_THROW(VL::ReadTerrainHeightAtlasStore(path, Budget()), std::runtime_error);
    changed = original;
    changed["atlasBytes"] = original.at("atlasBytes").get<uint64_t>() + 2;
    WriteManifest(changed);
    EXPECT_THROW(VL::ReadTerrainHeightAtlasStore(path, Budget()), std::runtime_error);
}

TEST_F(TerrainHeightAtlasStoreTest, FailedPublishKeepsOldManifestAndBinaryReadable)
{
    VL::WriteTerrainHeightAtlasStore(path, data);
    const auto before = Bytes(path);
    const auto oldAtlas = data.atlas;
    data.atlas[0] ^= 1;
    EXPECT_THROW(VL::WriteTerrainHeightAtlasStore(path, data), std::runtime_error);
    EXPECT_EQ(Bytes(path), before);
    EXPECT_EQ(VL::ReadTerrainHeightAtlasStore(path, Budget()).atlas, oldAtlas);
}
