#include <algorithm>
#include <array>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

#include "resource/image/textureIO.h"
#include "textureAssetLoader.h"

namespace
{
HostImage MakeLayer(uint8_t value)
{
    HostImage image;
    image.width = 2;
    image.height = 2;
    image.rowStrideBytes = 8;
    image.data.assign(16, value);
    return image;
}

class TextureArrayTest : public ::testing::Test
{
protected:
    void SetUp() override
    {
        const auto id = std::chrono::steady_clock::now().time_since_epoch().count();
        directory = std::filesystem::temp_directory_path() /
            ("vulkanlearn_texture_array_" + std::to_string(id));
        ASSERT_TRUE(std::filesystem::create_directory(directory));
    }

    void TearDown() override
    {
        std::filesystem::remove_all(directory);
    }

    std::filesystem::path WriteBmp(
        const std::string& name,
        uint8_t red = 0x33,
        bool singleRow = false)
    {
        // 1x2 BMP: bottom pixel RGB=(red,0x22,0x11), top=(0xcc,0xbb,0xaa).
        std::array<uint8_t, 62> bytes = {
            0x42, 0x4d, 0x3e, 0x00, 0x00, 0x00, 0x00, 0x00,
            0x00, 0x00, 0x36, 0x00, 0x00, 0x00, 0x28, 0x00,
            0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x02, 0x00,
            0x00, 0x00, 0x01, 0x00, 0x18, 0x00, 0x00, 0x00,
            0x00, 0x00, 0x08, 0x00, 0x00, 0x00, 0x00, 0x00,
            0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
            0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x11, 0x22,
            0x33, 0x00, 0xaa, 0xbb, 0xcc, 0x00};
        bytes[56] = red;
        if (singleRow)
        {
            bytes[2] = 58;
            bytes[22] = 1;
            bytes[34] = 4;
        }
        const auto path = directory / name;
        std::ofstream stream(path, std::ios::binary);
        stream.write(reinterpret_cast<const char*>(bytes.data()), singleRow ? 58 : bytes.size());
        return path;
    }

    std::filesystem::path WriteTextureArrayAsset(
        const std::string& name,
        const nlohmann::json& sources)
    {
        nlohmann::json asset = {
            {"name", "T_textureArrayAssetGate"},
            {"type", "textureArray"},
            {"sources", sources},
            {"colorSpace", "linear"},
            {"mipmaps", false},
            {"filter", "nearest"},
            {"wrapMode", "clamp"}};
        const auto path = directory / name;
        std::ofstream stream(path);
        stream << asset.dump(2);
        return path;
    }

    std::filesystem::path directory;
};
}

TEST(HostImageArray, KeepsEightLayersInInputOrder)
{
    std::vector<HostImage> layers;
    for (uint8_t index = 0; index < 8; ++index)
        layers.push_back(MakeLayer(index));
    HostImage image = HostImage::CombineArrayLayers(std::move(layers));
    EXPECT_EQ(image.arrayLayers, 8u);
    EXPECT_EQ(image.GetSize(), 128u);
    for (uint32_t index = 0; index < 8; ++index)
    {
        const auto* pixels = static_cast<const uint8_t*>(image.GetMipData(0, index));
        for (size_t byte = 0; byte < 16; ++byte)
            EXPECT_EQ(pixels[byte], index);
    }
}

TEST(HostImageArray, MutableLayerAccessChangesOnlySelectedLayer)
{
    auto image = HostImage::CombineArrayLayers({MakeLayer(1), MakeLayer(2)});
    auto* pixels = static_cast<uint8_t*>(image.GetMipData(0, 1));
    pixels[0] = 99;
    EXPECT_EQ(image.data[0], 1);
    EXPECT_EQ(image.data[16], 99);
}

TEST(HostImageArray, SupportsImplicitTightRowStride)
{
    auto layer = MakeLayer(5);
    layer.rowStrideBytes = 0;
    const auto image = HostImage::CombineArrayLayers({layer, MakeLayer(7)});
    EXPECT_EQ(static_cast<const uint8_t*>(image.GetMipData(0, 1))[0], 7);
}

TEST(HostImageArray, PreservesPaddedRows)
{
    auto first = MakeLayer(3);
    first.rowStrideBytes = 12;
    first.data.resize(24, 6);
    auto second = first;
    second.data.assign(24, 8);
    const auto image = HostImage::CombineArrayLayers({first, second});
    EXPECT_EQ(image.GetSize(), 48u);
    EXPECT_EQ(static_cast<const uint8_t*>(image.GetMipData(0, 1))[0], 8);
}

TEST(HostImageArray, RejectsEmptyArray)
{
    EXPECT_THROW(HostImage::CombineArrayLayers({}), std::invalid_argument);
}

TEST(HostImageArray, RejectsDifferentDimensions)
{
    auto layer = MakeLayer(0);
    layer.width = 1;
    EXPECT_THROW(HostImage::CombineArrayLayers({MakeLayer(1), layer}), std::invalid_argument);
}

TEST(HostImageArray, RejectsDifferentTransferFormats)
{
    auto layer = MakeLayer(0);
    layer.format = HostImage::PixelFormat::RGBA8_SRGB;
    EXPECT_THROW(HostImage::CombineArrayLayers({MakeLayer(1), layer}), std::invalid_argument);
}

TEST(HostImageArray, RejectsDifferentChannelMetadata)
{
    auto layer = MakeLayer(0);
    layer.channels = 3;
    EXPECT_THROW(HostImage::CombineArrayLayers({MakeLayer(1), layer}), std::invalid_argument);
}

TEST(HostImageArray, RejectsDifferentSemantics)
{
    auto layer = MakeLayer(0);
    layer.semantic = HostImage::TextureSemantic::Normal;
    EXPECT_THROW(HostImage::CombineArrayLayers({MakeLayer(1), layer}), std::invalid_argument);
}

TEST(HostImageArray, RejectsDifferentSwizzles)
{
    auto layer = MakeLayer(0);
    layer.swizzle.r = 2;
    EXPECT_THROW(HostImage::CombineArrayLayers({MakeLayer(1), layer}), std::invalid_argument);
}

TEST(HostImageArray, RejectsNestedArrays)
{
    auto image = HostImage::CombineArrayLayers({MakeLayer(1), MakeLayer(2)});
    EXPECT_THROW(HostImage::CombineArrayLayers({image}), std::invalid_argument);
}

TEST(HostImageArray, RejectsCpuMipChains)
{
    auto layer = MakeLayer(0);
    layer.mipLevels = 2;
    EXPECT_THROW(HostImage::CombineArrayLayers({layer}), std::invalid_argument);
}

TEST(HostImageArray, RejectsTruncatedPixels)
{
    auto layer = MakeLayer(0);
    layer.data.pop_back();
    EXPECT_THROW(HostImage::CombineArrayLayers({layer}), std::invalid_argument);
}

TEST(HostImageArray, RejectsExtraPixels)
{
    auto layer = MakeLayer(0);
    layer.data.push_back(1);
    EXPECT_THROW(HostImage::CombineArrayLayers({layer}), std::invalid_argument);
}

TEST(HostImageArray, RejectsUndersizedStride)
{
    auto layer = MakeLayer(0);
    layer.rowStrideBytes = 4;
    EXPECT_THROW(HostImage::CombineArrayLayers({layer}), std::invalid_argument);
}

TEST(HostImageArray, RejectsZeroDimensions)
{
    auto layer = MakeLayer(0);
    layer.height = 0;
    EXPECT_THROW(HostImage::CombineArrayLayers({layer}), std::invalid_argument);
}

TEST(HostImageArray, RejectsLayerOutsideArray)
{
    const auto image = HostImage::CombineArrayLayers({MakeLayer(1), MakeLayer(2)});
    EXPECT_THROW(image.GetMipData(0, 2), std::out_of_range);
}

TEST(HostImageArray, RejectsMipOutsideStoredData)
{
    const auto image = HostImage::CombineArrayLayers({MakeLayer(1)});
    EXPECT_THROW(image.GetMipData(1), std::invalid_argument);
}

TEST(HostImageArray, RejectsTruncatedLayerAccess)
{
    auto image = HostImage::CombineArrayLayers({MakeLayer(1), MakeLayer(2)});
    image.data.pop_back();
    EXPECT_THROW(image.GetMipData(0, 1), std::invalid_argument);
}

TEST_F(TextureArrayTest, DecodesEightSourcesAndFlipsEachLayerOnce)
{
    std::vector<std::filesystem::path> paths;
    for (uint8_t layer = 0; layer < 8; ++layer)
        paths.push_back(WriteBmp(std::to_string(layer) + ".bmp", layer));
    TextureIO::LoadOptions options;
    options.flipY = TextureIO::LoadOptions::FlipYMode::ForceOn;
    options.transfer = TextureIO::LoadOptions::Transfer::SRGB;
    const HostImage image = TextureIO::LoadArray(paths, options);
    ASSERT_EQ(image.arrayLayers, 8u);
    EXPECT_EQ(image.width, 1u);
    EXPECT_EQ(image.height, 2u);
    EXPECT_EQ(image.format, HostImage::PixelFormat::RGBA8_SRGB);
    for (uint32_t layer = 0; layer < 8; ++layer)
    {
        const auto* pixels = static_cast<const uint8_t*>(image.GetMipData(0, layer));
        EXPECT_EQ(pixels[0], layer);
        EXPECT_EQ(pixels[4], 0xcc);
        EXPECT_EQ(pixels[3], 0xff);
        EXPECT_EQ(pixels[7], 0xff);
    }
}

TEST_F(TextureArrayTest, PreservesLinearNormalMetadata)
{
    TextureIO::LoadOptions options;
    options.semantic = HostImage::TextureSemantic::Normal;
    options.transfer = TextureIO::LoadOptions::Transfer::Linear;
    const auto image = TextureIO::LoadArray({WriteBmp("normal.bmp")}, options);
    EXPECT_EQ(image.arrayLayers, 1u);
    EXPECT_EQ(image.semantic, HostImage::TextureSemantic::Normal);
    EXPECT_EQ(image.format, HostImage::PixelFormat::RGBA8_UNORM);
    EXPECT_EQ(image.data[0], 0xcc);
}

TEST_F(TextureArrayTest, MissingSourceReportsLayerAndPath)
{
    const auto missing = directory / "missing.bmp";
    try
    {
        TextureIO::LoadArray({WriteBmp("valid.bmp"), missing}, {});
        FAIL() << "Missing layer must reject the complete array";
    }
    catch (const std::runtime_error& error)
    {
        const std::string message(error.what());
        EXPECT_NE(message.find("layer 1"), std::string::npos);
        EXPECT_NE(message.find(missing.string()), std::string::npos);
    }
}

TEST_F(TextureArrayTest, RejectsDifferentDecodedDimensions)
{
    EXPECT_THROW(TextureIO::LoadArray(
        {WriteBmp("two-rows.bmp"), WriteBmp("one-row.bmp", 0x33, true)}, {}), std::invalid_argument);
}

TEST_F(TextureArrayTest, RejectsDifferentDecodedFormats)
{
    HostImage hdr;
    hdr.width = 1;
    hdr.height = 2;
    hdr.format = HostImage::PixelFormat::RGBA16_FLOAT;
    hdr.rowStrideBytes = 8;
    hdr.data.resize(16);
    const auto exr = directory / "hdr.exr";
    TextureIO::SaveOptions save;
    save.format = TextureIO::FileFormat::Exr;
    ASSERT_TRUE(TextureIO::Save(exr, hdr, save));
    EXPECT_THROW(TextureIO::LoadArray({WriteBmp("ldr.bmp"), exr}, {}), std::invalid_argument);
}

TEST_F(TextureArrayTest, RejectsEmptySourceList)
{
    EXPECT_THROW(TextureIO::LoadArray({}, {}), std::invalid_argument);
}

TEST_F(TextureArrayTest, RejectsUnsupportedChannelRequest)
{
    TextureIO::LoadOptions options;
    options.forceChannels = 3;
    EXPECT_THROW(TextureIO::LoadArray({WriteBmp("source.bmp")}, options), std::invalid_argument);
}

TEST_F(TextureArrayTest, RejectsCpuMipGeneration)
{
    TextureIO::LoadOptions options;
    options.genMipmaps = true;
    EXPECT_THROW(TextureIO::LoadArray({WriteBmp("source.bmp")}, options), std::invalid_argument);
}

TEST_F(TextureArrayTest, SingleImageExportRejectsArrayWithoutWriting)
{
    auto layer = MakeLayer(0);
    layer.format = HostImage::PixelFormat::RGBA16_FLOAT;
    layer.rowStrideBytes = 16;
    layer.data.resize(32);
    const auto image = HostImage::CombineArrayLayers({layer, layer});
    const auto output = directory / "array.exr";
    TextureIO::SaveOptions options;
    options.format = TextureIO::FileFormat::Exr;
    EXPECT_FALSE(TextureIO::Save(output, image, options));
    EXPECT_FALSE(std::filesystem::exists(output));
}

TEST_F(TextureArrayTest, LoadsArrayDescriptorInSourceOrder)
{
    const auto first = WriteBmp("first.bmp");
    const auto second = WriteBmp("second.bmp");
    const auto assetPath = WriteTextureArrayAsset(
        "T_textureArrayAsset.json",
        {first.string(), second.string()});

    const TextureBindingLoadDesc description =
        LoadTextureAssetDesc(assetPath.string());
    ASSERT_EQ(description.sources.size(), 2u);
    EXPECT_TRUE(description.textureArray);
    EXPECT_EQ(description.sources[0], first.string());
    EXPECT_EQ(description.sources[1], second.string());
    EXPECT_EQ(description.transfer, TextureIO::LoadOptions::Transfer::Linear);
    EXPECT_FALSE(description.generateMipmaps);

    const Texture::CreateDesc createDesc = ToTextureCreateDesc(description);
    EXPECT_TRUE(createDesc.textureArray);
    EXPECT_FALSE(createDesc.generateMipmaps);
    EXPECT_EQ(createDesc.filter, vk::Filter::eNearest);
    EXPECT_EQ(createDesc.wrapMode, vk::SamplerAddressMode::eClampToEdge);

    const std::string key = BuildTextureCacheKey(description);
    EXPECT_LT(key.find(first.string()), key.find(second.string()));
    TextureBindingLoadDesc reversed = description;
    std::reverse(reversed.sources.begin(), reversed.sources.end());
    EXPECT_NE(key, BuildTextureCacheKey(reversed));
}

TEST_F(TextureArrayTest, RejectsMixedSourceAndSourcesFields)
{
    const auto source = WriteBmp("mixed.bmp");
    const auto assetPath = directory / "T_mixed.json";
    std::ofstream stream(assetPath);
    stream << nlohmann::json({
        {"name", "T_mixed"},
        {"type", "textureArray"},
        {"source", source.string()},
        {"sources", {source.string()}},
        {"colorSpace", "linear"}}).dump(2);
    stream.close();
    EXPECT_THROW(LoadTextureAssetDesc(assetPath.string()), std::exception);
}

TEST_F(TextureArrayTest, RejectsEmptyArraySources)
{
    const auto assetPath = WriteTextureArrayAsset("T_empty_sources.json", nlohmann::json::array());
    EXPECT_THROW(LoadTextureAssetDesc(assetPath.string()), std::exception);
}

TEST_F(TextureArrayTest, RejectsSingleTextureWithSourcesField)
{
    const auto source = WriteBmp("single.bmp");
    const auto assetPath = directory / "T_single_sources.json";
    std::ofstream stream(assetPath);
    stream << nlohmann::json({
        {"name", "T_single_sources"},
        {"type", "texture"},
        {"sources", {source.string()}},
        {"colorSpace", "srgb"}}).dump(2);
    stream.close();
    EXPECT_THROW(LoadTextureAssetDesc(assetPath.string()), std::exception);
}
