#include <gtest/gtest.h>

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <vector>

#include "terrain/terrainPaintFile.h"

namespace
{

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

class TerrainPaintFileTest : public testing::Test
{
protected:
    void SetUp() override
    {
        root = std::filesystem::temp_directory_path() /
            "vl-terrain-paint-file-tests";
        std::error_code ignored;
        std::filesystem::remove_all(root, ignored);
        std::filesystem::create_directories(root);
    }

    void TearDown() override
    {
        std::error_code ignored;
        std::filesystem::remove_all(root, ignored);
    }

    std::filesystem::path root;
};

} // namespace

TEST_F(TerrainPaintFileTest, RoundTripPreservesLayerMajorWeightsAndOpacity)
{
    const VL::TerrainLayerPaintData paint = MakePaint(8, 3);
    const std::filesystem::path path = root / "paint.vlpaint";
    std::string error;
    ASSERT_TRUE(VL::SaveTerrainLayerPaintFile(path, paint, error)) << error;

    const size_t count = paint.weights.size();
    ASSERT_EQ(std::filesystem::file_size(path), 24 + count * sizeof(float) + count);

    VL::TerrainLayerPaintData loaded;
    ASSERT_TRUE(VL::LoadTerrainLayerPaintFile(
        path, paint.layerCount, loaded, error)) << error;
    EXPECT_EQ(loaded.width, paint.width);
    EXPECT_EQ(loaded.height, paint.height);
    EXPECT_EQ(loaded.layerCount, paint.layerCount);
    EXPECT_EQ(loaded.weights, paint.weights);
    EXPECT_EQ(loaded.opacity, paint.opacity);
}

TEST_F(TerrainPaintFileTest, ReaderRejectsWrongMagic)
{
    const std::filesystem::path path = root / "paint.vlpaint";
    std::string error;
    ASSERT_TRUE(VL::SaveTerrainLayerPaintFile(path, MakePaint(), error)) << error;
    {
        std::fstream file(path, std::ios::binary | std::ios::in | std::ios::out);
        ASSERT_TRUE(file.is_open());
        const char wrong = 'X';
        file.seekp(0, std::ios::beg);
        file.write(&wrong, 1);
    }
    VL::TerrainLayerPaintData loaded;
    EXPECT_FALSE(VL::LoadTerrainLayerPaintFile(path, 2, loaded, error));
    EXPECT_NE(error.find(path.string()), std::string::npos) << error;
    EXPECT_NE(error.find("magic"), std::string::npos) << error;
}

TEST_F(TerrainPaintFileTest, ReaderRejectsWrongLayerCount)
{
    const std::filesystem::path path = root / "paint.vlpaint";
    std::string error;
    ASSERT_TRUE(VL::SaveTerrainLayerPaintFile(path, MakePaint(4, 2), error)) << error;
    VL::TerrainLayerPaintData loaded;
    EXPECT_FALSE(VL::LoadTerrainLayerPaintFile(path, 3, loaded, error));
    EXPECT_NE(error.find(path.string()), std::string::npos) << error;
    EXPECT_NE(error.find("layerCount"), std::string::npos) << error;
}

TEST_F(TerrainPaintFileTest, ReaderRejectsTruncatedFile)
{
    const std::filesystem::path path = root / "paint.vlpaint";
    std::string error;
    ASSERT_TRUE(VL::SaveTerrainLayerPaintFile(path, MakePaint(), error)) << error;
    ASSERT_GT(std::filesystem::file_size(path), 3u);
    std::filesystem::resize_file(
        path, std::filesystem::file_size(path) - 3);

    VL::TerrainLayerPaintData loaded;
    EXPECT_FALSE(VL::LoadTerrainLayerPaintFile(path, 2, loaded, error));
    EXPECT_NE(error.find(path.string()), std::string::npos) << error;
}

TEST_F(TerrainPaintFileTest, ReaderRejectsOutOfRangeWeight)
{
    const std::filesystem::path path = root / "paint.vlpaint";
    std::string error;
    ASSERT_TRUE(VL::SaveTerrainLayerPaintFile(path, MakePaint(), error)) << error;
    {
        std::fstream file(path, std::ios::binary | std::ios::in | std::ios::out);
        ASSERT_TRUE(file.is_open());
        // 2.0f 的小端位模式 0x40000000。
        const uint8_t tooLarge[4] = {0x00, 0x00, 0x00, 0x40};
        file.seekp(24, std::ios::beg);
        file.write(reinterpret_cast<const char*>(tooLarge), 4);
    }

    VL::TerrainLayerPaintData loaded;
    EXPECT_FALSE(VL::LoadTerrainLayerPaintFile(path, 2, loaded, error));
    EXPECT_NE(error.find(path.string()), std::string::npos) << error;
    EXPECT_NE(error.find("weight"), std::string::npos) << error;
}

TEST_F(TerrainPaintFileTest, AtomicWriterLeavesNoTemporaryFile)
{
    const std::filesystem::path path = root / "paint.vlpaint";
    std::string error;
    ASSERT_TRUE(VL::SaveTerrainLayerPaintFile(path, MakePaint(), error)) << error;

    const std::string temporaryPrefix =
        path.filename().string() + ".tmp.";
    for (const auto& entry : std::filesystem::directory_iterator(root))
    {
        const std::string filename = entry.path().filename().string();
        EXPECT_NE(filename.rfind(temporaryPrefix, 0), 0u) << filename;
    }
}

TEST_F(TerrainPaintFileTest, ReaderRejectsNonZeroFlagsOrReserved)
{
    const std::filesystem::path path = root / "paint.vlpaint";
    std::string error;
    ASSERT_TRUE(VL::SaveTerrainLayerPaintFile(path, MakePaint(), error)) << error;
    {
        std::fstream file(path, std::ios::binary | std::ios::in | std::ios::out);
        ASSERT_TRUE(file.is_open());
        const char flags = 1;
        file.seekp(16, std::ios::beg);
        file.write(&flags, 1);
    }
    VL::TerrainLayerPaintData loaded;
    EXPECT_FALSE(VL::LoadTerrainLayerPaintFile(path, 2, loaded, error));
    EXPECT_NE(error.find("flags"), std::string::npos) << error;
}

TEST_F(TerrainPaintFileTest, ResolverKeepsPaintTargetsInsideTheResourceRoot)
{
    std::filesystem::path resolved;
    std::string error;
    ASSERT_TRUE(VL::ResolveTerrainPaintPath(
        root, "Generated/Paint/paint.vlpaint", resolved, error)) << error;
    EXPECT_EQ(resolved.lexically_relative(root),
        std::filesystem::path("Generated/Paint/paint.vlpaint"));

    EXPECT_FALSE(VL::ResolveTerrainPaintPath(
        root, "../paint.vlpaint", resolved, error));
    EXPECT_FALSE(error.empty());
    EXPECT_FALSE(VL::ResolveTerrainPaintPath(
        root, (root / "paint.vlpaint").string(), resolved, error));
    EXPECT_FALSE(error.empty());
}
