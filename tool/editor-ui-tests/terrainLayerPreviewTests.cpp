#include <gtest/gtest.h>

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <limits>
#include <string>
#include <thread>
#include <vector>

#include <nlohmann/json.hpp>

#include "editor/terrain/terrainLayerPreview.h"
#include "resource/image/textureIO.h"

namespace
{
using namespace VL;
using namespace VL::Editor;
using Json = nlohmann::json;

class TerrainLayerPreviewTest : public testing::Test
{
protected:
    void SetUp() override
    {
        const uint64_t stamp = static_cast<uint64_t>(
            std::chrono::steady_clock::now().time_since_epoch().count());
        root = std::filesystem::temp_directory_path() /
            ("vl-terrain-layer-preview-" + std::to_string(stamp));
        std::filesystem::create_directories(root / "Source");
        std::filesystem::create_directories(root / "Textures");
        std::filesystem::create_directories(root / "Materials");
    }

    void TearDown() override
    {
        std::error_code error;
        std::filesystem::remove_all(root, error);
    }

    HostImage MakeAlbedo(uint8_t seed) const
    {
        HostImage image;
        image.width = 16;
        image.height = 16;
        image.channels = 4;
        image.format = HostImage::PixelFormat::RGBA8_SRGB;
        image.rowStrideBytes = image.width * 4;
        image.data.resize(static_cast<size_t>(image.rowStrideBytes) * image.height);
        for (uint32_t y = 0; y < image.height; ++y)
        {
            for (uint32_t x = 0; x < image.width; ++x)
            {
                const size_t offset = (static_cast<size_t>(y) * image.width + x) * 4;
                image.data[offset + 0] = static_cast<uint8_t>(
                    32u + seed * 17u + x * 9u);
                image.data[offset + 1] = static_cast<uint8_t>(
                    64u + seed * 11u + y * 7u);
                image.data[offset + 2] = static_cast<uint8_t>(
                    96u + seed * 5u + (x + y) * 3u);
                image.data[offset + 3] = 255;
            }
        }
        return image;
    }

    HostImage MakeNormal() const
    {
        HostImage image;
        image.width = 16;
        image.height = 16;
        image.channels = 4;
        image.format = HostImage::PixelFormat::RGBA8_UNORM;
        image.rowStrideBytes = image.width * 4;
        image.data.resize(static_cast<size_t>(image.rowStrideBytes) * image.height);
        for (size_t offset = 0; offset < image.data.size(); offset += 4)
        {
            image.data[offset + 0] = 128;
            image.data[offset + 1] = 128;
            image.data[offset + 2] = 255;
            image.data[offset + 3] = 255;
        }
        return image;
    }

    HostImage MakePbr(uint8_t roughness, uint8_t displacement = 128) const
    {
        HostImage image;
        image.width = 16;
        image.height = 16;
        image.channels = 4;
        image.format = HostImage::PixelFormat::RGBA8_UNORM;
        image.rowStrideBytes = image.width * 4;
        image.data.resize(static_cast<size_t>(image.rowStrideBytes) * image.height);
        for (size_t offset = 0; offset < image.data.size(); offset += 4)
        {
            image.data[offset + 0] = roughness;
            image.data[offset + 1] = 0;
            image.data[offset + 2] = 242;
            image.data[offset + 3] = displacement;
        }
        return image;
    }

    void WritePng(const std::filesystem::path& relativePath, const HostImage& image) const
    {
        TextureIO::SaveOptions options;
        options.format = TextureIO::FileFormat::Png;
        ASSERT_TRUE(TextureIO::Save(root / relativePath, image, options));
    }

    static uint32_t Crc32(const uint8_t* data, size_t size)
    {
        uint32_t crc = 0xFFFFFFFFu;
        for (size_t index = 0; index < size; ++index)
        {
            crc ^= data[index];
            for (uint32_t bit = 0; bit < 8; ++bit)
                crc = (crc >> 1u) ^ (0xEDB88320u & (0u - (crc & 1u)));
        }
        return ~crc;
    }

    static void AppendBigEndian32(std::vector<uint8_t>& output, uint32_t value)
    {
        output.push_back(static_cast<uint8_t>((value >> 24u) & 0xFFu));
        output.push_back(static_cast<uint8_t>((value >> 16u) & 0xFFu));
        output.push_back(static_cast<uint8_t>((value >> 8u) & 0xFFu));
        output.push_back(static_cast<uint8_t>(value & 0xFFu));
    }

    static void AppendPngChunk(
        std::vector<uint8_t>& output,
        const char* type,
        const std::vector<uint8_t>& data)
    {
        AppendBigEndian32(output, static_cast<uint32_t>(data.size()));
        const size_t typeOffset = output.size();
        output.insert(output.end(), type, type + 4);
        output.insert(output.end(), data.begin(), data.end());
        AppendBigEndian32(output, Crc32(
            output.data() + typeOffset, output.size() - typeOffset));
    }

    // 测试侧手写 PNG：stb 只提供解码，仓库没有 16-bit 写入器；这里用 stored
    // deflate 生成真正的 16-bit grayscale，确保 stbi_load_16 拿到原始样本。
    void WriteHeight16(
        const std::filesystem::path& relativePath,
        uint32_t width,
        uint32_t height,
        const std::vector<uint16_t>& samples) const
    {
        ASSERT_EQ(samples.size(), static_cast<size_t>(width) * height);
        std::vector<uint8_t> raw;
        raw.reserve((static_cast<size_t>(width) * 2 + 1) * height);
        for (uint32_t y = 0; y < height; ++y)
        {
            raw.push_back(0);
            for (uint32_t x = 0; x < width; ++x)
            {
                const uint16_t value =
                    samples[static_cast<size_t>(y) * width + x];
                raw.push_back(static_cast<uint8_t>(value >> 8u));
                raw.push_back(static_cast<uint8_t>(value & 0xFFu));
            }
        }

        std::vector<uint8_t> compressed = {0x78, 0x01};
        const uint16_t length = static_cast<uint16_t>(raw.size());
        const uint16_t inverse = static_cast<uint16_t>(~length);
        compressed.push_back(0x01);
        compressed.push_back(static_cast<uint8_t>(length & 0xFFu));
        compressed.push_back(static_cast<uint8_t>((length >> 8u) & 0xFFu));
        compressed.push_back(static_cast<uint8_t>(inverse & 0xFFu));
        compressed.push_back(static_cast<uint8_t>((inverse >> 8u) & 0xFFu));
        compressed.insert(compressed.end(), raw.begin(), raw.end());
        uint32_t first = 1;
        uint32_t second = 0;
        for (const uint8_t byte : raw)
        {
            first = (first + byte) % 65521u;
            second = (second + first) % 65521u;
        }
        AppendBigEndian32(compressed, (second << 16u) | first);

        std::vector<uint8_t> image = {137, 80, 78, 71, 13, 10, 26, 10};
        std::vector<uint8_t> header(13, 0);
        header[0] = static_cast<uint8_t>(width >> 24u);
        header[1] = static_cast<uint8_t>(width >> 16u);
        header[2] = static_cast<uint8_t>(width >> 8u);
        header[3] = static_cast<uint8_t>(width);
        header[4] = static_cast<uint8_t>(height >> 24u);
        header[5] = static_cast<uint8_t>(height >> 16u);
        header[6] = static_cast<uint8_t>(height >> 8u);
        header[7] = static_cast<uint8_t>(height);
        header[8] = 16;
        header[9] = 0;
        AppendPngChunk(image, "IHDR", header);
        AppendPngChunk(image, "IDAT", compressed);
        AppendPngChunk(image, "IEND", {});

        const std::filesystem::path target = relativePath.is_absolute() ?
            relativePath : root / relativePath;
        std::ofstream stream(target, std::ios::binary | std::ios::trunc);
        ASSERT_TRUE(stream.is_open());
        stream.write(
            reinterpret_cast<const char*>(image.data()),
            static_cast<std::streamsize>(image.size()));
        ASSERT_TRUE(static_cast<bool>(stream));
    }

    void WriteText(const std::filesystem::path& relativePath, const std::string& text) const
    {
        std::ofstream stream(root / relativePath, std::ios::binary | std::ios::trunc);
        ASSERT_TRUE(stream.is_open());
        stream << text;
    }

    void WriteJson(const std::filesystem::path& relativePath, const Json& value) const
    {
        WriteText(relativePath, value.dump(2));
    }

    void WriteTextureArrays()
    {
        WriteJson("Textures/T_albedo.json", Json{
            {"name", "T_albedo"},
            {"type", "textureArray"},
            {"sources", Json::array({
                "Source/grass_albedo.png",
                "Source/rock_albedo.png",
                "Source/sand_albedo.png"})},
            {"displacementSources", Json::array({
                "Source/grass_height16.png",
                "Source/rock_height16.png",
                "Source/sand_height16.png"})},
            {"colorSpace", "srgb"},
            {"mipmaps", true},
            {"filter", "linear"},
            {"wrapMode", "repeat"}});
        WriteJson("Textures/T_normal.json", Json{
            {"name", "T_normal"},
            {"type", "textureArray"},
            {"sources", Json::array({
                "Source/grass_normal.png",
                "Source/rock_normal.png",
                "Source/sand_normal.png"})},
            {"colorSpace", "linear"},
            {"mipmaps", true},
            {"filter", "linear"},
            {"wrapMode", "repeat"}});
        WriteJson("Textures/T_pbr.json", Json{
            {"name", "T_pbr"},
            {"type", "textureArray"},
            {"sources", Json::array({
                "Source/grass_pbr.png",
                "Source/rock_pbr.png",
                "Source/sand_pbr.png"})},
            {"colorSpace", "linear"},
            {"mipmaps", true},
            {"filter", "linear"},
            {"wrapMode", "repeat"}});
    }

    void WriteFixture()
    {
        WritePng("Source/grass_albedo.png", MakeAlbedo(1));
        WritePng("Source/rock_albedo.png", MakeAlbedo(2));
        WritePng("Source/sand_albedo.png", MakeAlbedo(3));
        for (const char* name : {"grass", "rock", "sand"})
        {
            WritePng(
                std::string("Source/") + name + "_normal.png", MakeNormal());
            WritePng(
                std::string("Source/") + name + "_pbr.png",
                MakePbr(
                    static_cast<uint8_t>(name[0] == 'g' ? 205 : 160),
                    static_cast<uint8_t>(
                        name[0] == 'g' ? 96 : (name[0] == 'r' ? 144 : 192))));
            std::vector<uint16_t> height(16 * 16, 0x8000);
            for (uint32_t y = 0; y < 16; ++y)
            {
                for (uint32_t x = 0; x < 16; ++x)
                {
                    const uint16_t wave = static_cast<uint16_t>(
                        ((x * 4096u + y * 2048u) & 0xFFFFu));
                    height[static_cast<size_t>(y) * 16 + x] = wave;
                }
            }
            WriteHeight16(
                std::string("Source/") + name + "_height16.png",
                16, 16, height);
        }
        WriteTextureArrays();
        WriteJson("Materials/MI_layers.json", Json{
            {"name", "MI_layers"},
            {"type", "materialInstance"},
            {"material", "shader/glsl/M_terrain.json"},
            {"textures", {
                {"layerAlbedo", "Textures/T_albedo.json"},
                {"layerNormal", "Textures/T_normal.json"},
                {"layerPbr", "Textures/T_pbr.json"}}}});
        desc.materialInstancePath = "Materials/MI_layers.json";
        desc.layers.resize(3);
        desc.layers[0].name = "Grass";
        desc.layers[0].sourceSet = 0;
        desc.layers[1].name = "Rock";
        desc.layers[1].sourceSet = 0;
        desc.layers[2].name = "Sand";
        desc.layers[2].sourceSet = 0;
    }

    std::filesystem::path root;
    TerrainAssetDesc desc;
};

TEST_F(TerrainLayerPreviewTest, SourceSliceCountsOnlyEarlierLayersInTheSameSet)
{
    TerrainAssetDesc layers;
    layers.layers.resize(5);
    layers.layers[0].sourceSet = 0;
    layers.layers[1].sourceSet = 1;
    layers.layers[2].sourceSet = 0;
    layers.layers[3].sourceSet = 2;
    layers.layers[4].sourceSet = 0;

    EXPECT_EQ(TerrainLayerSourceSlice(layers, 0), 0u);
    EXPECT_EQ(TerrainLayerSourceSlice(layers, 1), 0u);
    EXPECT_EQ(TerrainLayerSourceSlice(layers, 2), 1u);
    EXPECT_EQ(TerrainLayerSourceSlice(layers, 3), 0u);
    EXPECT_EQ(TerrainLayerSourceSlice(layers, 4), 2u);
    EXPECT_EQ(TerrainLayerSourceSlice(layers, 5),
        std::numeric_limits<size_t>::max());
}

TEST_F(TerrainLayerPreviewTest, ResolvesAlbedoNormalAndPbrAtMatchingSetLocalSlice)
{
    WriteFixture();
    const TerrainLayerPreviewSources grass =
        ResolveTerrainLayerPreviewSources(desc, 0, root);
    const TerrainLayerPreviewSources rock =
        ResolveTerrainLayerPreviewSources(desc, 1, root);
    const TerrainLayerPreviewSources sand =
        ResolveTerrainLayerPreviewSources(desc, 2, root);
    ASSERT_TRUE(grass.IsComplete());
    ASSERT_TRUE(rock.IsComplete());
    ASSERT_TRUE(sand.IsComplete());
    EXPECT_EQ(grass.albedoPath.filename(), "grass_albedo.png");
    EXPECT_EQ(grass.normalPath.filename(), "grass_normal.png");
    EXPECT_EQ(grass.pbrPath.filename(), "grass_pbr.png");
    EXPECT_TRUE(grass.displacementDeclared);
    EXPECT_EQ(grass.displacementPath.filename(), "grass_height16.png");
    EXPECT_EQ(rock.albedoPath.filename(), "rock_albedo.png");
    EXPECT_EQ(rock.normalPath.filename(), "rock_normal.png");
    EXPECT_EQ(rock.pbrPath.filename(), "rock_pbr.png");
    EXPECT_EQ(rock.displacementPath.filename(), "rock_height16.png");
    EXPECT_EQ(sand.albedoPath.filename(), "sand_albedo.png");
    EXPECT_EQ(sand.normalPath.filename(), "sand_normal.png");
    EXPECT_EQ(sand.pbrPath.filename(), "sand_pbr.png");
    EXPECT_EQ(sand.displacementPath.filename(), "sand_height16.png");
    EXPECT_EQ(
        ResolveTerrainLayerPreviewSource(desc, 0, root).filename(),
        "grass_albedo.png");

    desc.layers[2].sourceSet = 1;
    const TerrainLayerPreviewSources remapped =
        ResolveTerrainLayerPreviewSources(desc, 2, root);
    ASSERT_TRUE(remapped.IsComplete());
    EXPECT_EQ(remapped.albedoPath.filename(), "grass_albedo.png");
    EXPECT_EQ(remapped.normalPath.filename(), "grass_normal.png");
    EXPECT_EQ(remapped.pbrPath.filename(), "grass_pbr.png");
    EXPECT_EQ(remapped.displacementPath.filename(), "grass_height16.png");

    desc.layers[2].sourceSet = 0;
    desc.layers.resize(4);
    desc.layers[3].sourceSet = 0;
    EXPECT_FALSE(ResolveTerrainLayerPreviewSources(desc, 3, root).IsComplete());
}

TEST_F(TerrainLayerPreviewTest, PbrMaterialOrIncompletePbrSetFallsBack)
{
    WritePng("Source/grass_albedo.png", MakeAlbedo(1));
    WriteJson("Textures/T_albedo.json", Json{
        {"name", "T_albedo"},
        {"type", "textureArray"},
        {"sources", Json::array({"Source/grass_albedo.png"})},
        {"colorSpace", "srgb"},
        {"mipmaps", true},
        {"filter", "linear"},
        {"wrapMode", "repeat"}});
    desc.materialInstancePath = "Materials/MI_layers.json";
    desc.layers.resize(1);
    desc.layers[0].sourceSet = 0;

    WriteJson("Materials/MI_layers.json", Json{
        {"name", "MI_layers"},
        {"type", "materialInstance"},
        {"material", "shader/glsl/M_terrain.json"},
        {"textures", {{"layerAlbedo", "Textures/T_albedo.json"}}}});
    const TerrainLayerPreviewSources albedoOnly =
        ResolveTerrainLayerPreviewSources(desc, 0, root);
    EXPECT_FALSE(albedoOnly.albedoPath.empty());
    EXPECT_FALSE(albedoOnly.IsComplete());
    TerrainLayerPreviewCache cache(root);
    const TerrainLayerPreviewPaths incomplete = cache.Get(desc, 0);
    EXPECT_TRUE(incomplete.spherePath.empty());
    EXPECT_TRUE(incomplete.planePath.empty());

    WriteJson("Materials/MI_layers.json", Json{
        {"name", "MI_layers"},
        {"type", "materialInstance"},
        {"material", "shader/glsl/M_pbr.json"},
        {"textures", {{"layerAlbedo", "Textures/T_albedo.json"}}}});
    EXPECT_FALSE(
        ResolveTerrainLayerPreviewSources(desc, 0, root).IsComplete());

    WriteJson("Materials/MI_layers.json", Json{
        {"name", "MI_layers"},
        {"type", "materialInstance"},
        {"material", "shader/glsl/M_terrain.json"},
        {"textures", Json::object()}});
    EXPECT_FALSE(
        ResolveTerrainLayerPreviewSources(desc, 0, root).IsComplete());
}

TEST_F(TerrainLayerPreviewTest, BrokenNormalFallbackDoesNotEmitPreview)
{
    WriteFixture();
    WriteText("Source/grass_normal.png", "not a png");
    TerrainLayerPreviewCache cache(root);
    const TerrainLayerPreviewPaths paths = cache.Get(desc, 0);
    EXPECT_TRUE(paths.spherePath.empty());
    EXPECT_TRUE(paths.planePath.empty());
}

TEST_F(TerrainLayerPreviewTest, SphereAndPlaneUsePbrAndKeepSphereTransparency)
{
    const HostImage albedo = MakeAlbedo(2);
    const HostImage normal = MakeNormal();
    const TerrainLayerPreviewSettings settings =
        DefaultTerrainLayerPreviewSettings;
    HostImage sphere;
    ASSERT_TRUE(BuildTerrainLayerSpherePreview(
        albedo, normal, MakePbr(160, 124), nullptr, settings, sphere));
    EXPECT_EQ(sphere.width, 96u);
    EXPECT_EQ(sphere.height, 96u);
    EXPECT_EQ(sphere.format, HostImage::PixelFormat::RGBA8_SRGB);
    EXPECT_EQ(sphere.data[3], 0u);
    EXPECT_EQ(sphere.data[(static_cast<size_t>(48) * 96 + 48) * 4 + 3], 255u);

    HostImage plane;
    ASSERT_TRUE(BuildTerrainLayerPlanePreview(
        albedo, normal, MakePbr(160, 124), nullptr, settings, plane));
    EXPECT_EQ(plane.width, 160u);
    EXPECT_EQ(plane.height, 112u);
    EXPECT_EQ(plane.data[3], 255u);
    bool distinctPixels = false;
    for (size_t offset = 4; offset + 3 < plane.data.size(); offset += 4)
    {
        if (plane.data[offset] != plane.data[0] ||
            plane.data[offset + 1] != plane.data[1] ||
            plane.data[offset + 2] != plane.data[2])
        {
            distinctPixels = true;
            break;
        }
    }
    EXPECT_TRUE(distinctPixels);
}

TEST_F(TerrainLayerPreviewTest, RoughnessChangesSphereOutput)
{
    const HostImage albedo = MakeAlbedo(4);
    const HostImage normal = MakeNormal();
    const TerrainLayerPreviewSettings settings =
        DefaultTerrainLayerPreviewSettings;
    HostImage smooth;
    HostImage rough;
    ASSERT_TRUE(BuildTerrainLayerSpherePreview(
        albedo, normal, MakePbr(32, 128), nullptr, settings, smooth));
    ASSERT_TRUE(BuildTerrainLayerSpherePreview(
        albedo, normal, MakePbr(235, 128), nullptr, settings, rough));
    EXPECT_NE(smooth.data, rough.data);
}

TEST_F(TerrainLayerPreviewTest, LegacyFallbackUsesPbrAlphaWhenNoDisplacementSources)
{
    const HostImage albedo = MakeAlbedo(5);
    const HostImage normal = MakeNormal();
    const TerrainLayerPreviewSettings settings =
        DefaultTerrainLayerPreviewSettings;
    HostImage low;
    HostImage high;
    ASSERT_TRUE(BuildTerrainLayerSpherePreview(
        albedo, normal, MakePbr(180, 64), nullptr, settings, low));
    ASSERT_TRUE(BuildTerrainLayerSpherePreview(
        albedo, normal, MakePbr(180, 220), nullptr, settings, high));
    EXPECT_NE(low.data, high.data);
}

TEST_F(TerrainLayerPreviewTest, SixteenBitDisplacementLoadPreservesSamples)
{
    WriteHeight16("Source/probe16.png", 2, 1, {0xFFFF, 0x8000});
    TerrainLayerHeightField field;
    ASSERT_TRUE(LoadTerrainLayerDisplacement(
        root / "Source/probe16.png", field));
    ASSERT_EQ(field.samples.size(), 2u);
    // 8-bit 截断会把两个样本都变成 255；这里必须保留 0xFFFF / 0x8000 的差别。
    EXPECT_EQ(field.samples[0], 0xFFFFu);
    EXPECT_EQ(field.samples[1], 0x8000u);
    EXPECT_NE(field.samples[0], field.samples[1]);
    EXPECT_NEAR(field.SampleNormalized(0.25f, 0.5f), 1.0f, 1.0e-4f);
    EXPECT_NEAR(field.SampleNormalized(0.75f, 0.5f), 0.5f, 1.0e-4f);
}

TEST_F(TerrainLayerPreviewTest, SixteenBitDisplacementChangesSphereRelief)
{
    const HostImage albedo = MakeAlbedo(6);
    const HostImage normal = MakeNormal();
    const TerrainLayerPreviewSettings settings =
        DefaultTerrainLayerPreviewSettings;

    TerrainLayerHeightField flat;
    flat.width = 16;
    flat.height = 16;
    flat.samples.assign(16 * 16, 0x8000);

    TerrainLayerHeightField strong = flat;
    for (uint32_t y = 0; y < strong.height; ++y)
    {
        for (uint32_t x = 0; x < strong.width; ++x)
        {
            strong.samples[static_cast<size_t>(y) * strong.width + x] =
                ((x + y) % 2 == 0) ? 0xFFFF : 0x0000;
        }
    }

    HostImage flatPreview;
    HostImage strongPreview;
    ASSERT_TRUE(BuildTerrainLayerSpherePreview(
        albedo, normal, MakePbr(180, 128), &flat, settings, flatPreview));
    ASSERT_TRUE(BuildTerrainLayerSpherePreview(
        albedo, normal, MakePbr(180, 128), &strong, settings, strongPreview));
    EXPECT_EQ(flatPreview.width, strongPreview.width);
    EXPECT_NE(flatPreview.data, strongPreview.data);
}

TEST_F(TerrainLayerPreviewTest, MissingDisplacementSourcesFallsBackToLegacyHeight)
{
    WriteFixture();
    WriteJson("Textures/T_albedo.json", Json{
        {"name", "T_albedo"},
        {"type", "textureArray"},
        {"sources", Json::array({
            "Source/grass_albedo.png",
            "Source/rock_albedo.png",
            "Source/sand_albedo.png"})},
        {"colorSpace", "srgb"},
        {"mipmaps", true},
        {"filter", "linear"},
        {"wrapMode", "repeat"}});
    const TerrainLayerPreviewSources sources =
        ResolveTerrainLayerPreviewSources(desc, 0, root);
    EXPECT_TRUE(sources.IsComplete());
    EXPECT_FALSE(sources.displacementDeclared);
    EXPECT_TRUE(sources.displacementPath.empty());

    TerrainLayerPreviewCache cache(root);
    const TerrainLayerPreviewPaths paths = cache.Get(desc, 0);
    EXPECT_FALSE(paths.spherePath.empty());
    EXPECT_FALSE(paths.planePath.empty());
}

TEST_F(TerrainLayerPreviewTest, CacheWritesReusablePngsWithoutRewriting)
{
    WriteFixture();
    TerrainLayerPreviewCache cache(root);
    const TerrainLayerPreviewPaths first = cache.Get(desc, 0);
    const TerrainLayerPreviewPaths second = cache.Get(desc, 1);
    ASSERT_FALSE(first.spherePath.empty());
    ASSERT_FALSE(first.planePath.empty());
    ASSERT_FALSE(second.spherePath.empty());
    ASSERT_FALSE(second.planePath.empty());
    EXPECT_NE(first.spherePath, second.spherePath);
    EXPECT_NE(first.planePath, second.planePath);
    EXPECT_TRUE(std::filesystem::is_regular_file(first.spherePath));
    EXPECT_TRUE(std::filesystem::is_regular_file(first.planePath));

    const auto sphere = TextureIO::Load(first.spherePath);
    ASSERT_TRUE(sphere.has_value());
    EXPECT_EQ(sphere->width, 96u);
    EXPECT_EQ(sphere->height, 96u);
    const auto plane = TextureIO::Load(first.planePath);
    ASSERT_TRUE(plane.has_value());
    EXPECT_EQ(plane->width, 160u);
    EXPECT_EQ(plane->height, 112u);

    const std::filesystem::file_time_type sphereWriteTime =
        std::filesystem::last_write_time(first.spherePath);
    const std::filesystem::file_time_type planeWriteTime =
        std::filesystem::last_write_time(first.planePath);
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    cache.Reset();
    const TerrainLayerPreviewPaths reused = cache.Get(desc, 0);
    EXPECT_EQ(reused.spherePath, first.spherePath);
    EXPECT_EQ(reused.planePath, first.planePath);
    EXPECT_EQ(
        std::filesystem::last_write_time(reused.spherePath),
        sphereWriteTime);
    EXPECT_EQ(
        std::filesystem::last_write_time(reused.planePath),
        planeWriteTime);
}

TEST_F(TerrainLayerPreviewTest, DisplacementChangeInvalidatesPreviewCache)
{
    WriteFixture();
    TerrainLayerPreviewCache cache(root);
    const TerrainLayerPreviewPaths first = cache.Get(desc, 0);
    ASSERT_FALSE(first.spherePath.empty());
    const auto firstImage = TextureIO::Load(first.spherePath);
    ASSERT_TRUE(firstImage.has_value());

    const std::filesystem::path heightPath =
        ResolveTerrainLayerPreviewSources(desc, 0, root).displacementPath;
    ASSERT_FALSE(heightPath.empty());
    std::vector<uint16_t> strong(16 * 16, 0x0000);
    for (uint32_t y = 0; y < 16; ++y)
    {
        for (uint32_t x = 0; x < 16; ++x)
        {
            strong[static_cast<size_t>(y) * 16 + x] =
                ((x / 2 + y / 2) % 2 == 0) ? 0xFFFF : 0x0000;
        }
    }
    WriteHeight16(heightPath, 16, 16, strong);
    std::filesystem::last_write_time(
        heightPath,
        std::filesystem::last_write_time(heightPath) +
            std::chrono::seconds(3));

    const TerrainLayerPreviewPaths second = cache.Get(desc, 0);
    ASSERT_FALSE(second.spherePath.empty());
    EXPECT_NE(second.spherePath, first.spherePath);
    const auto secondImage = TextureIO::Load(second.spherePath);
    ASSERT_TRUE(secondImage.has_value());
    EXPECT_NE(firstImage->data, secondImage->data);
}

TEST_F(TerrainLayerPreviewTest, NormalOrPbrMetadataChangeInvalidatesCachePath)
{
    WriteFixture();
    TerrainLayerPreviewCache cache(root);
    const TerrainLayerPreviewPaths first = cache.Get(desc, 0);
    ASSERT_FALSE(first.spherePath.empty());

    const std::filesystem::path normalPath =
        ResolveTerrainLayerPreviewSources(desc, 0, root).normalPath;
    ASSERT_FALSE(normalPath.empty());
    const auto changedNormalTime =
        std::filesystem::last_write_time(normalPath) + std::chrono::seconds(3);
    std::filesystem::last_write_time(normalPath, changedNormalTime);
    const TerrainLayerPreviewPaths afterNormal = cache.Get(desc, 0);
    ASSERT_FALSE(afterNormal.spherePath.empty());
    EXPECT_NE(afterNormal.spherePath, first.spherePath);

    const std::filesystem::path pbrPath =
        ResolveTerrainLayerPreviewSources(desc, 0, root).pbrPath;
    ASSERT_FALSE(pbrPath.empty());
    const auto changedPbrTime =
        std::filesystem::last_write_time(pbrPath) + std::chrono::seconds(6);
    std::filesystem::last_write_time(pbrPath, changedPbrTime);
    const TerrainLayerPreviewPaths afterPbr = cache.Get(desc, 0);
    ASSERT_FALSE(afterPbr.spherePath.empty());
    EXPECT_NE(afterPbr.spherePath, afterNormal.spherePath);
}

TEST_F(TerrainLayerPreviewTest, CacheKeyIncludesAllSourcesSettingsAndKind)
{
    const std::filesystem::file_time_type writeTime =
        std::filesystem::file_time_type::clock::now();
    const TerrainLayerPreviewSourceStamp albedo = {
        "D:/resources/Source/grass_albedo.png", 4096, writeTime};
    const TerrainLayerPreviewSourceStamp normal = {
        "D:/resources/Source/grass_normal.png", 8192, writeTime};
    const TerrainLayerPreviewSourceStamp pbr = {
        "D:/resources/Source/grass_pbr.png", 2048, writeTime};
    const TerrainLayerPreviewSourceStamp displacement = {
        "D:/resources/Source/grass_height16.png", 16384, writeTime};
    const TerrainLayerPreviewSettings settings =
        DefaultTerrainLayerPreviewSettings;
    const std::string sphere = TerrainLayerPreviewCacheKey(
        albedo, normal, pbr, displacement, settings,
        TerrainLayerPreviewKind::Sphere);
    const std::string sphereAgain = TerrainLayerPreviewCacheKey(
        albedo, normal, pbr, displacement, settings,
        TerrainLayerPreviewKind::Sphere);
    const std::string plane = TerrainLayerPreviewCacheKey(
        albedo, normal, pbr, displacement, settings,
        TerrainLayerPreviewKind::Plane);
    TerrainLayerPreviewSourceStamp changedNormal = normal;
    changedNormal.writeTime += std::chrono::seconds(1);
    TerrainLayerPreviewSourceStamp changedPbr = pbr;
    changedPbr.size += 1;
    TerrainLayerPreviewSourceStamp changedDisplacement = displacement;
    changedDisplacement.writeTime += std::chrono::seconds(2);
    TerrainLayerPreviewSettings changedSettings = settings;
    changedSettings.sphereSize = 112;

    EXPECT_EQ(sphere, sphereAgain);
    EXPECT_NE(sphere, plane);
    EXPECT_NE(sphere, TerrainLayerPreviewCacheKey(
        albedo, changedNormal, pbr, displacement, settings,
        TerrainLayerPreviewKind::Sphere));
    EXPECT_NE(sphere, TerrainLayerPreviewCacheKey(
        albedo, normal, changedPbr, displacement, settings,
        TerrainLayerPreviewKind::Sphere));
    EXPECT_NE(sphere, TerrainLayerPreviewCacheKey(
        albedo, normal, pbr, changedDisplacement, settings,
        TerrainLayerPreviewKind::Sphere));
    EXPECT_NE(sphere, TerrainLayerPreviewCacheKey(
        albedo, normal, pbr, TerrainLayerPreviewSourceStamp{}, settings,
        TerrainLayerPreviewKind::Sphere));
    EXPECT_NE(sphere, TerrainLayerPreviewCacheKey(
        albedo, normal, pbr, displacement, changedSettings,
        TerrainLayerPreviewKind::Sphere));
    EXPECT_EQ(sphere.size(), 64u);
}

} // namespace
