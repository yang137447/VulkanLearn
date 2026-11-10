#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <optional>
#include <stdexcept>
#include <string>
#include <vector>

#include <gtest/gtest.h>
#include <nlohmann/json.hpp>
#include <stb_image.h>

#include "resource/image/textureIO.h"
#include "terrain/terrainAssetLoader.h"
#include "terrain/terrainHeightmap.h"
#include "terrain/terrainHeightmapIO.h"
#include "terrain/terrainHeightmapWriter.h"

namespace
{

constexpr float Q16(uint16_t value)
{
    return static_cast<float>(value) / 65535.0f;
}

class TerrainHeightmapIOTest : public testing::Test
{
protected:
    void SetUp() override
    {
        const uint64_t stamp = static_cast<uint64_t>(
            std::chrono::steady_clock::now().time_since_epoch().count());
        directory = std::filesystem::temp_directory_path() /
            ("vl-terrain-heightmap-io-" + std::to_string(stamp));
        ASSERT_TRUE(std::filesystem::create_directory(directory));
    }

    void TearDown() override
    {
        std::error_code error;
        std::filesystem::remove_all(directory, error);
    }

    HostImage MakeNormalized(
        uint32_t width,
        uint32_t height,
        const std::vector<float>& samples) const
    {
        HostImage image;
        image.width = width;
        image.height = height;
        image.channels = 1;
        image.mipLevels = 1;
        image.arrayLayers = 1;
        image.format = HostImage::PixelFormat::R32_FLOAT;
        image.semantic = HostImage::TextureSemantic::Data;
        image.rowStrideBytes = width * static_cast<uint32_t>(sizeof(float));
        image.data.resize(samples.size() * sizeof(float));
        std::memcpy(
            image.data.data(),
            samples.data(),
            samples.size() * sizeof(float));
        return image;
    }

    void WriteSixteenBitPng(
        const std::filesystem::path& path,
        const HostImage& image) const
    {
        VL::TerrainHeightmapWriter::WriteOptions options;
        options.pngBitDepth =
            VL::TerrainHeightmapWriter::WriteOptions::PngBitDepth::Sixteen;
        std::string error;
        ASSERT_TRUE(VL::TerrainHeightmapWriter::Write(
            path, image, options, error)) << error;
    }

    std::vector<uint8_t> ReadBytes(const std::filesystem::path& path) const
    {
        std::ifstream stream(path, std::ios::binary);
        return std::vector<uint8_t>(
            std::istreambuf_iterator<char>(stream),
            std::istreambuf_iterator<char>());
    }

    void WriteBytes(
        const std::filesystem::path& path,
        const std::vector<uint8_t>& bytes) const
    {
        std::ofstream stream(path, std::ios::binary | std::ios::trunc);
        ASSERT_TRUE(stream.is_open());
        stream.write(
            reinterpret_cast<const char*>(bytes.data()),
            static_cast<std::streamsize>(bytes.size()));
        ASSERT_TRUE(static_cast<bool>(stream));
    }

    std::filesystem::path directory;
};

} // namespace

TEST_F(TerrainHeightmapIOTest, LoadsSixteenBitPngWithoutTruncatingSamples)
{
    const std::filesystem::path path = directory / "height16.png";
    WriteSixteenBitPng(
        path,
        MakeNormalized(3, 1, {0.0f, Q16(0x8000), 1.0f}));
    EXPECT_EQ(stbi_is_16_bit(path.string().c_str()), 1);

    std::string error;
    const std::optional<HostImage> loaded =
        VL::TerrainHeightmapIO::Load(path, error);
    ASSERT_TRUE(loaded.has_value()) << error;
    EXPECT_EQ(loaded->format, HostImage::PixelFormat::R32_FLOAT);
    EXPECT_EQ(loaded->channels, 1u);
    EXPECT_EQ(loaded->rowStrideBytes, 3u * sizeof(float));
    ASSERT_EQ(loaded->data.size(), 3u * sizeof(float));
    const float* samples =
        reinterpret_cast<const float*>(loaded->data.data());
    EXPECT_FLOAT_EQ(samples[0], 0.0f);
    EXPECT_FLOAT_EQ(samples[1], Q16(0x8000));
    EXPECT_FLOAT_EQ(samples[2], 1.0f);
    // 0x8000/65535 与 8-bit 截断后的 128/255 不同；该断言防止静默降位。
    EXPECT_GT(std::abs(samples[1] - 0.5f), 1.0e-6f);
}

TEST_F(TerrainHeightmapIOTest, RoundTripsSixteenBitPngSamples)
{
    const std::filesystem::path path = directory / "roundtrip16.png";
    const std::vector<float> expected = {
        0.0f, Q16(0x8000), Q16(0x8001), Q16(0xFFFF)};
    WriteSixteenBitPng(path, MakeNormalized(4, 1, expected));

    std::string error;
    const std::optional<HostImage> loaded =
        VL::TerrainHeightmapIO::Load(path, error);
    ASSERT_TRUE(loaded.has_value()) << error;
    const float* samples =
        reinterpret_cast<const float*>(loaded->data.data());
    for (size_t index = 0; index < expected.size(); ++index)
        EXPECT_FLOAT_EQ(samples[index], expected[index]);
    EXPECT_NE(samples[1], samples[2]);
}

TEST_F(TerrainHeightmapIOTest, BilinearSamplingUsesSixteenBitSamples)
{
    const std::filesystem::path path = directory / "bilinear16.png";
    WriteSixteenBitPng(
        path,
        MakeNormalized(2, 2, {
            0.0f, Q16(0x8000),
            Q16(0x8000), 1.0f}));

    std::string error;
    const std::optional<HostImage> loaded =
        VL::TerrainHeightmapIO::Load(path, error);
    ASSERT_TRUE(loaded.has_value()) << error;
    const float sample =
        VL::TerrainHeightmap::SampleNormalized(*loaded, 0.5f, 0.5f);
    const float expected = (2.0f * Q16(0x8000) + 1.0f) * 0.25f;
    const float eightBitMidpoint =
        (2.0f * (128.0f / 255.0f) + 1.0f) * 0.25f;
    EXPECT_NEAR(sample, expected, 1.0e-7f);
    EXPECT_GT(std::abs(sample - eightBitMidpoint), 1.0e-4f);
}

TEST_F(TerrainHeightmapIOTest, RejectsTruncatedAndMalformedSixteenBitPng)
{
    const std::filesystem::path validPath = directory / "valid16.png";
    WriteSixteenBitPng(
        validPath,
        MakeNormalized(2, 2, {
            0.0f, Q16(0x8000),
            Q16(0x8000), 1.0f}));
    const std::vector<uint8_t> bytes = ReadBytes(validPath);
    ASSERT_FALSE(bytes.empty());

    const std::filesystem::path truncatedPath = directory / "truncated16.png";
    WriteBytes(
        truncatedPath,
        std::vector<uint8_t>(bytes.begin(), bytes.begin() + bytes.size() / 2));
    std::string error;
    EXPECT_FALSE(VL::TerrainHeightmapIO::Load(truncatedPath, error).has_value());
    EXPECT_FALSE(error.empty());
    EXPECT_NE(error.find("terrain heightmap"), std::string::npos);

    const std::filesystem::path emptyPath = directory / "empty16.png";
    WriteBytes(emptyPath, {});
    EXPECT_FALSE(VL::TerrainHeightmapIO::Load(emptyPath, error).has_value());
    EXPECT_FALSE(error.empty());
}

TEST_F(TerrainHeightmapIOTest, KeepsEightBitPngOnTheTextureIoPath)
{
    HostImage image;
    image.width = 2;
    image.height = 2;
    image.channels = 1;
    image.mipLevels = 1;
    image.arrayLayers = 1;
    image.format = HostImage::PixelFormat::R8_UNORM;
    image.semantic = HostImage::TextureSemantic::Data;
    image.rowStrideBytes = 2;
    image.data = {0, 64, 128, 255};

    const std::filesystem::path path = directory / "height8.png";
    TextureIO::SaveOptions saveOptions;
    saveOptions.format = TextureIO::FileFormat::Png;
    ASSERT_TRUE(TextureIO::Save(path, image, saveOptions));
    EXPECT_EQ(stbi_is_16_bit(path.string().c_str()), 0);

    std::string error;
    const std::optional<HostImage> loaded =
        VL::TerrainHeightmapIO::Load(path, error);
    ASSERT_TRUE(loaded.has_value()) << error;

    TextureIO::LoadOptions options;
    options.semantic = HostImage::TextureSemantic::Data;
    options.flipY = TextureIO::LoadOptions::FlipYMode::ForceOff;
    options.transfer = TextureIO::LoadOptions::Transfer::Linear;
    options.forceChannels = 4;
    const std::optional<HostImage> expected =
        TextureIO::Load(path, options);
    ASSERT_TRUE(expected.has_value());
    EXPECT_EQ(loaded->format, expected->format);
    EXPECT_EQ(loaded->channels, expected->channels);
    EXPECT_EQ(loaded->data, expected->data);
    EXPECT_EQ(loaded->format, HostImage::PixelFormat::RGBA8_UNORM);
    EXPECT_EQ(loaded->data[0], 0u);
    EXPECT_EQ(loaded->data[4], 64u);
    EXPECT_EQ(loaded->data[8], 128u);
    EXPECT_EQ(loaded->data[12], 255u);
}

TEST_F(TerrainHeightmapIOTest, WriterDefaultsNewPngToEightBit)
{
    const std::filesystem::path path = directory / "new_default.png";
    std::string error;
    ASSERT_TRUE(VL::TerrainHeightmapWriter::Write(
        path,
        MakeNormalized(2, 2, {0.0f, 0.5f, 1.0f, 0.25f}),
        error)) << error;
    EXPECT_EQ(stbi_is_16_bit(path.string().c_str()), 0);
}

TEST_F(TerrainHeightmapIOTest, ExrWriterRoundTripsHeightsWithoutClamping)
{
    const std::filesystem::path path = directory / "height_roundtrip.exr";
    const std::vector<float> expected = {0.0f, 0.25f, 1.5f, -0.25f};
    std::string error;
    ASSERT_TRUE(VL::TerrainHeightmapWriter::Write(
        path, MakeNormalized(2, 2, expected), error)) << error;

    const std::optional<HostImage> loaded =
        VL::TerrainHeightmapIO::Load(path, error);
    ASSERT_TRUE(loaded.has_value()) << error;
    EXPECT_EQ(loaded->format, HostImage::PixelFormat::RGBA32_FLOAT);
    EXPECT_EQ(loaded->width, 2u);
    EXPECT_EQ(loaded->height, 2u);
    const float* samples =
        reinterpret_cast<const float*>(loaded->data.data());
    for (size_t index = 0; index < expected.size(); ++index)
        EXPECT_NEAR(samples[index * 4], expected[index], 1.0e-3f);
}

TEST_F(TerrainHeightmapIOTest, BmpWriterRemainsEightBitAndReadable)
{
    const std::filesystem::path path = directory / "height_roundtrip.bmp";
    std::string error;
    ASSERT_TRUE(VL::TerrainHeightmapWriter::Write(
        path,
        MakeNormalized(2, 2, {0.0f, 0.25f, 1.0f, 0.5f}),
        error)) << error;

    const std::optional<HostImage> loaded =
        VL::TerrainHeightmapIO::Load(path, error);
    ASSERT_TRUE(loaded.has_value()) << error;
    EXPECT_EQ(loaded->format, HostImage::PixelFormat::RGBA8_UNORM);
    EXPECT_EQ(loaded->width, 2u);
    EXPECT_EQ(loaded->height, 2u);
    EXPECT_EQ(loaded->data[0], 0u);
    EXPECT_EQ(loaded->data[4], 64u);
    EXPECT_EQ(loaded->data[8], 255u);
    EXPECT_EQ(loaded->data[12], 128u);
}

TEST_F(TerrainHeightmapIOTest, WriterAutoPreservesExistingSixteenBitPng)
{
    const std::filesystem::path path = directory / "preserve16.png";
    const HostImage image = MakeNormalized(2, 2, {
        0.0f, Q16(0x8000),
        Q16(0x8001), 1.0f});
    WriteSixteenBitPng(path, image);
    ASSERT_EQ(stbi_is_16_bit(path.string().c_str()), 1);

    std::string error;
    ASSERT_TRUE(VL::TerrainHeightmapWriter::Write(path, image, error)) << error;
    EXPECT_EQ(stbi_is_16_bit(path.string().c_str()), 1);

    VL::TerrainHeightmapWriter::WriteOptions eightBit;
    eightBit.pngBitDepth =
        VL::TerrainHeightmapWriter::WriteOptions::PngBitDepth::Eight;
    ASSERT_TRUE(VL::TerrainHeightmapWriter::Write(
        path, image, eightBit, error)) << error;
    EXPECT_EQ(stbi_is_16_bit(path.string().c_str()), 0);
}

TEST_F(TerrainHeightmapIOTest, TerrainAssetLoaderUsesSixteenBitHeightmapPath)
{
    const std::filesystem::path heightmapPath = directory / "terrain16.png";
    const HostImage heightmap = MakeNormalized(3, 2, {
        0.0f, Q16(0x8000), 1.0f,
        Q16(0x4000), Q16(0x8001), Q16(0xC000)});
    WriteSixteenBitPng(heightmapPath, heightmap);

    const std::filesystem::path materialPath = directory / "MI_terrain16.json";
    std::ofstream materialStream(materialPath);
    materialStream << nlohmann::json({
        {"name", "MI_terrain16"},
        {"type", "materialInstance"},
        {"material", "shader/glsl/M_unlit.json"}}).dump(2);
    materialStream.close();

    nlohmann::json asset = {
        {"name", "TR_terrain16"},
        {"type", "terrain"},
        {"heightmapPath", heightmapPath.string()},
        {"size", {1000.0, 1000.0}},
        {"heightScale", 100.0},
        {"heightOffset", 0.0},
        {"gridResolution", 4},
        {"chunkQuads", 2},
        {"materialInstancePath", materialPath.string()}};
    const std::filesystem::path terrainPath = directory / "TR_terrain16.json";
    std::ofstream terrainStream(terrainPath);
    terrainStream << asset.dump(2);
    terrainStream.close();

    const VL::TerrainBuildPlan plan =
        VL::TerrainAssetLoader::Prepare(terrainPath.string());
    ASSERT_TRUE(plan.heightmap != nullptr);
    EXPECT_EQ(plan.heightmap->format, HostImage::PixelFormat::R32_FLOAT);
    EXPECT_EQ(plan.heightmap->width, 3u);
    EXPECT_EQ(plan.heightmap->height, 2u);
    const float* samples =
        reinterpret_cast<const float*>(plan.heightmap->data.data());
    EXPECT_FLOAT_EQ(samples[0], 0.0f);
    EXPECT_FLOAT_EQ(samples[1], Q16(0x8000));
    EXPECT_FLOAT_EQ(samples[2], 1.0f);
    EXPECT_FLOAT_EQ(samples[4], Q16(0x8001));

    const std::filesystem::path tinyPath = directory / "tiny16.png";
    WriteSixteenBitPng(
        tinyPath,
        MakeNormalized(1, 1, {Q16(0x8000)}));
    asset["heightmapPath"] = tinyPath.string();
    const std::filesystem::path tinyTerrainPath =
        directory / "TR_terrain16_tiny.json";
    std::ofstream tinyTerrainStream(tinyTerrainPath);
    tinyTerrainStream << asset.dump(2);
    tinyTerrainStream.close();

    try
    {
        static_cast<void>(VL::TerrainAssetLoader::Prepare(
            tinyTerrainPath.string()));
        FAIL() << "1x1 16-bit heightmap must be rejected";
    }
    catch (const std::exception& exception)
    {
        EXPECT_NE(
            std::string(exception.what()).find("at least 2x2"),
            std::string::npos);
    }
}
