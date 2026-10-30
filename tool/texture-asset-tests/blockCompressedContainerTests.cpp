#include <algorithm>
#include <array>
#include <chrono>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <stdexcept>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include "resource/image/compressedTextureDecoder.h"
#include "resource/image/hostImageVulkanPlan.h"
#include "resource/image/textureIO.h"

namespace
{
constexpr uint32_t DdsMagic = 0x20534444u;
constexpr uint32_t DdsFourCcDx10 = 0x30315844u;
constexpr uint32_t DdsFourCcAti2 = 0x32495441u;

void RequireRange(size_t offset, size_t length, size_t size)
{
    if (offset > size || length > size - offset)
        throw std::out_of_range("test fixture write exceeds container size");
}

void WriteU32(std::vector<uint8_t>& bytes, size_t offset, uint32_t value)
{
    RequireRange(offset, sizeof(uint32_t), bytes.size());
    std::memcpy(bytes.data() + offset, &value, sizeof(value));
}

void WriteU64(std::vector<uint8_t>& bytes, size_t offset, uint64_t value)
{
    std::memcpy(bytes.data() + offset, &value, sizeof(value));
}

size_t BcMipLayerBytes(uint32_t width, uint32_t height, uint32_t mip)
{
    const uint32_t mipWidth = mip == 0 ? width : std::max(1u, width >> mip);
    const uint32_t mipHeight = mip == 0 ? height : std::max(1u, height >> mip);
    return static_cast<size_t>((mipWidth + 3) / 4) * ((mipHeight + 3) / 4) * 16;
}

size_t BcLayerBytes(uint32_t width, uint32_t height, uint32_t mipLevels)
{
    size_t bytes = 0;
    for (uint32_t mip = 0; mip < mipLevels; ++mip)
        bytes += BcMipLayerBytes(width, height, mip);
    return bytes;
}

uint8_t RegionTag(uint32_t layer, uint32_t mip)
{
    return static_cast<uint8_t>(0x10 * layer + mip);
}

void FillRegion(std::vector<uint8_t>& bytes, size_t offset, size_t count, uint8_t value)
{
    RequireRange(offset, count, bytes.size());
    std::fill(bytes.begin() + static_cast<std::ptrdiff_t>(offset),
              bytes.begin() + static_cast<std::ptrdiff_t>(offset + count), value);
}

// HostImage 期望布局：layer-major、层内 mip-major。
std::vector<uint8_t> ExpectedBlockImage(
    uint32_t width,
    uint32_t height,
    uint32_t mipLevels,
    uint32_t arrayLayers)
{
    const size_t layerBytes = BcLayerBytes(width, height, mipLevels);
    std::vector<uint8_t> expected(layerBytes * arrayLayers, 0);
    size_t mipOffset = 0;
    for (uint32_t mip = 0; mip < mipLevels; ++mip)
    {
        const size_t mipBytes = BcMipLayerBytes(width, height, mip);
        for (uint32_t layer = 0; layer < arrayLayers; ++layer)
        {
            FillRegion(
                expected,
                static_cast<size_t>(layer) * layerBytes + mipOffset,
                mipBytes,
                RegionTag(layer, mip));
        }
        mipOffset += mipBytes;
    }
    return expected;
}

struct DdsOptions
{
    uint32_t width = 8;
    uint32_t height = 8;
    uint32_t mipLevels = 1;
    uint32_t arrayLayers = 1;
    uint32_t dxgiFormat = 98; // BC7_UNORM
    uint32_t resourceDimension = 3; // D3D10_RESOURCE_DIMENSION_TEXTURE2D
    uint32_t miscFlag = 0;
    uint32_t caps2 = 0;
    uint32_t depth = 0;
    int32_t mipCountField = -1;
    uint32_t pitchOrLinearSize = 0;
    bool legacyBc5 = false;
    bool truncatePayload = false;
};

std::vector<uint8_t> BuildDds(const DdsOptions& options)
{
    const size_t pixelDataOffset = options.legacyBc5 ? 128u : 148u;
    std::vector<uint8_t> bytes(pixelDataOffset, 0);

    WriteU32(bytes, 0, DdsMagic);
    WriteU32(bytes, 4, 124);
    WriteU32(bytes, 8, 0x1u | 0x2u | 0x4u | 0x1000u);
    WriteU32(bytes, 12, options.height);
    WriteU32(bytes, 16, options.width);
    WriteU32(bytes, 20, options.pitchOrLinearSize);
    WriteU32(bytes, 24, options.depth);
    WriteU32(
        bytes,
        28,
        options.mipCountField >= 0
            ? static_cast<uint32_t>(options.mipCountField)
            : options.mipLevels);
    WriteU32(bytes, 76, 32);
    WriteU32(bytes, 80, 0x4u);
    WriteU32(bytes, 84, options.legacyBc5 ? DdsFourCcAti2 : DdsFourCcDx10);
    WriteU32(bytes, 104, 0x1000u);
    WriteU32(bytes, 108, options.caps2);

    if (!options.legacyBc5)
    {
        WriteU32(bytes, 128, options.dxgiFormat);
        WriteU32(bytes, 132, options.resourceDimension);
        WriteU32(bytes, 136, options.miscFlag);
        WriteU32(bytes, 140, options.arrayLayers);
        WriteU32(bytes, 144, 0);
    }

    const uint32_t mipLevels = options.mipLevels == 0 ? 1u : options.mipLevels;
    const size_t layerBytes = BcLayerBytes(options.width, options.height, mipLevels);
    const size_t totalBytes = layerBytes * options.arrayLayers;
    bytes.resize(pixelDataOffset + totalBytes, 0);

    // DDS DX10 数组按 element 存完整 mip 链，和 HostImage 的 layer-major 布局一致。
    size_t mipOffset = 0;
    for (uint32_t mip = 0; mip < mipLevels; ++mip)
    {
        const size_t mipBytes = BcMipLayerBytes(options.width, options.height, mip);
        for (uint32_t layer = 0; layer < options.arrayLayers; ++layer)
        {
            FillRegion(
                bytes,
                pixelDataOffset + static_cast<size_t>(layer) * layerBytes + mipOffset,
                mipBytes,
                RegionTag(layer, mip));
        }
        mipOffset += mipBytes;
    }
    if (options.truncatePayload && !bytes.empty())
        bytes.pop_back();
    return bytes;
}

struct Ktx2Options
{
    uint32_t width = 8;
    uint32_t height = 8;
    uint32_t mipLevels = 1;
    uint32_t arrayLayers = 1;
    uint32_t vkFormat = 145; // BC7_UNORM
    uint32_t pixelDepth = 0;
    uint32_t faceCount = 1;
    uint32_t supercompression = 0;
    uint32_t levelCountField = UINT32_MAX;
    uint8_t colorModel = 134; // KHR_DF_MODEL_BC7
    uint8_t transferFunction = 1; // KHR_DF_TRANSFER_LINEAR
    uint8_t bytesPlane0 = 16;
    bool truncateLevelData = false;
};

std::vector<uint8_t> BuildKtx2(const Ktx2Options& options)
{
    const uint32_t levelCount =
        options.levelCountField == UINT32_MAX ? options.mipLevels : options.levelCountField;
    const uint32_t arrayLayers = options.arrayLayers == 0 ? 1u : options.arrayLayers;

    std::vector<size_t> levelBytes(levelCount, 0);
    size_t payloadBytes = 0;
    for (uint32_t mip = 0; mip < levelCount; ++mip)
    {
        levelBytes[mip] = BcMipLayerBytes(options.width, options.height, mip) * arrayLayers;
        payloadBytes += levelBytes[mip];
    }

    const size_t dfdOffset = 80 + static_cast<size_t>(levelCount) * 24;
    const size_t dfdSize = 28;
    std::vector<uint8_t> bytes(dfdOffset + dfdSize + payloadBytes, 0);

    const uint8_t identifier[12] = {
        0xAB, 0x4B, 0x54, 0x58, 0x20, 0x32, 0x30, 0xBB,
        0x0D, 0x0A, 0x1A, 0x0A };
    std::memcpy(bytes.data(), identifier, sizeof(identifier));
    WriteU32(bytes, 12, options.vkFormat);
    WriteU32(bytes, 16, 1); // typeSize
    WriteU32(bytes, 20, options.width);
    WriteU32(bytes, 24, options.height);
    WriteU32(bytes, 28, options.pixelDepth);
    WriteU32(bytes, 32, options.arrayLayers);
    WriteU32(bytes, 36, options.faceCount);
    WriteU32(bytes, 40, levelCount);
    WriteU32(bytes, 44, options.supercompression);
    WriteU32(bytes, 48, static_cast<uint32_t>(dfdOffset));
    WriteU32(bytes, 52, static_cast<uint32_t>(dfdSize));
    WriteU32(bytes, 56, 0);
    WriteU32(bytes, 60, 0);
    WriteU64(bytes, 64, 0);
    WriteU64(bytes, 72, 0);

    // DFD：totalSize + 一个 24 字节 descriptor block。
    WriteU32(bytes, dfdOffset, static_cast<uint32_t>(dfdSize));
    const size_t block = dfdOffset + 4;
    WriteU32(bytes, block, 0);
    WriteU32(bytes, block + 4, (24u << 16) | 2u);
    bytes[block + 8] = options.colorModel;
    bytes[block + 9] = 1; // KHR_DF_PRIMARIES_BT709
    bytes[block + 10] = options.transferFunction;
    bytes[block + 11] = 0;
    bytes[block + 12] = 3; // 4x4
    bytes[block + 13] = 3;
    bytes[block + 14] = 0;
    bytes[block + 15] = 0;
    bytes[block + 16] = options.bytesPlane0;

    // KTX2 每个 level 的数据块内按 layer 顺序排列（mip-major）。
    size_t levelDataOffset = dfdOffset + dfdSize;
    for (uint32_t mip = 0; mip < levelCount; ++mip)
    {
        const size_t entry = 80 + static_cast<size_t>(mip) * 24;
        WriteU64(bytes, entry, levelDataOffset);
        WriteU64(bytes, entry + 8, levelBytes[mip]);
        WriteU64(bytes, entry + 16, levelBytes[mip]);

        const size_t mipLayerBytes = BcMipLayerBytes(options.width, options.height, mip);
        for (uint32_t layer = 0; layer < arrayLayers; ++layer)
        {
            FillRegion(
                bytes,
                levelDataOffset + static_cast<size_t>(layer) * mipLayerBytes,
                mipLayerBytes,
                RegionTag(layer, mip));
        }
        levelDataOffset += levelBytes[mip];
    }
    if (options.truncateLevelData && !bytes.empty())
        bytes.pop_back();
    return bytes;
}

class CompressedTextureTest : public ::testing::Test
{
protected:
    void SetUp() override
    {
        const auto id = std::chrono::steady_clock::now().time_since_epoch().count();
        directory = std::filesystem::temp_directory_path() /
            ("vulkanlearn_bc_texture_" + std::to_string(id));
        ASSERT_TRUE(std::filesystem::create_directory(directory));
    }

    void TearDown() override
    {
        std::filesystem::remove_all(directory);
    }

    std::filesystem::path WriteFile(const std::string& name, const std::vector<uint8_t>& bytes)
    {
        const auto path = directory / name;
        std::ofstream stream(path, std::ios::binary);
        stream.write(reinterpret_cast<const char*>(bytes.data()),
                     static_cast<std::streamsize>(bytes.size()));
        return path;
    }

    std::filesystem::path directory;
};
}

TEST(HostImageBlockLayout, ComputesBlockRowStrideAndMipOffsets)
{
    HostImage image;
    image.width = 8;
    image.height = 8;
    image.mipLevels = 2;
    image.arrayLayers = 2;
    image.format = HostImage::PixelFormat::BC7_UNORM;
    image.channels = 4;
    image.data.resize(160, 0);
    image.ValidateStorage();

    EXPECT_EQ(image.GetMipWidth(0), 8u);
    EXPECT_EQ(image.GetMipWidth(1), 4u);
    EXPECT_EQ(image.GetMipRowStride(0), 32u);
    EXPECT_EQ(image.GetMipRowStride(1), 16u);
    EXPECT_EQ(image.GetMipRowCount(0), 2u);
    EXPECT_EQ(image.GetMipRowCount(1), 1u);
    EXPECT_EQ(image.GetMipLayerBytes(0), 64u);
    EXPECT_EQ(image.GetMipLayerBytes(1), 16u);
    EXPECT_EQ(image.GetLayerBytes(), 80u);
    EXPECT_EQ(image.GetTotalBytes(), 160u);
    EXPECT_EQ(image.GetMipOffset(0, 0), 0u);
    EXPECT_EQ(image.GetMipOffset(1, 0), 64u);
    EXPECT_EQ(image.GetMipOffset(0, 1), 80u);
    EXPECT_EQ(image.GetMipOffset(1, 1), 144u);
    EXPECT_EQ(HostImage::BytesPerTexelOrBlock(HostImage::PixelFormat::BC7_UNORM), 16u);
    EXPECT_THROW(image.GetPixelSize(), std::runtime_error);
}

TEST(HostImageBlockLayout, RejectsPaddedBlockRowStride)
{
    HostImage image;
    image.width = 8;
    image.height = 4;
    image.format = HostImage::PixelFormat::BC5_UNORM;
    image.rowStrideBytes = 64;
    image.data.resize(128, 0);
    EXPECT_THROW(image.ValidateStorage(), std::invalid_argument);
}

TEST(HostImageBlockLayout, AcceptsMatchingMipChainsInArrayLayers)
{
    const std::vector<uint8_t> layerBytes = ExpectedBlockImage(8, 8, 2, 1);
    std::vector<HostImage> layers;
    for (uint32_t layer = 0; layer < 2; ++layer)
    {
        HostImage image;
        image.width = 8;
        image.height = 8;
        image.mipLevels = 2;
        image.format = HostImage::PixelFormat::BC7_UNORM;
        image.channels = 4;
        image.data = layerBytes;
        image.data[0] = static_cast<uint8_t>(layer + 1);
        layers.push_back(image);
    }

    const HostImage combined = HostImage::CombineArrayLayers(layers);
    ASSERT_EQ(combined.arrayLayers, 2u);
    EXPECT_EQ(combined.mipLevels, 2u);
    EXPECT_EQ(combined.GetLayerBytes(), 80u);
    EXPECT_EQ(static_cast<const uint8_t*>(combined.GetMipData(0, 0))[0], 1u);
    EXPECT_EQ(static_cast<const uint8_t*>(combined.GetMipData(0, 1))[0], 2u);
    // mip1 区域保留夹具的 tag（layer 0 / mip 1），证明层内 mip 偏移正确拼接。
    EXPECT_EQ(static_cast<const uint8_t*>(combined.GetMipData(1, 1))[0], RegionTag(0, 1));
}

TEST(HostImageBlockLayout, RejectsMismatchedMipCounts)
{
    HostImage oneMip;
    oneMip.width = 8;
    oneMip.height = 8;
    oneMip.format = HostImage::PixelFormat::BC7_UNORM;
    oneMip.data.assign(64, 0);

    HostImage twoMips = oneMip;
    twoMips.mipLevels = 2;
    twoMips.data.assign(80, 0);
    EXPECT_THROW(HostImage::CombineArrayLayers({oneMip, twoMips}), std::invalid_argument);
}

TEST(CompressedTextureDds, DecodesBc7MipChainAndArrayLayers)
{
    DdsOptions options;
    options.mipLevels = 2;
    options.arrayLayers = 2;
    const std::vector<uint8_t> bytes = BuildDds(options);

    TextureIO::LoadOptions load;
    load.formatHint = TextureIO::FileFormat::Dds;
    load.transfer = TextureIO::LoadOptions::Transfer::Linear;
    const HostImage image =
        TextureIO::LoadFromMemory(bytes.data(), bytes.size(), load).value();

    EXPECT_EQ(image.width, 8u);
    EXPECT_EQ(image.height, 8u);
    EXPECT_EQ(image.mipLevels, 2u);
    EXPECT_EQ(image.arrayLayers, 2u);
    EXPECT_EQ(image.format, HostImage::PixelFormat::BC7_UNORM);
    EXPECT_EQ(image.channels, 4u);
    EXPECT_EQ(image.data, ExpectedBlockImage(8, 8, 2, 2));
    EXPECT_EQ(
        static_cast<const uint8_t*>(image.GetMipData(1, 1))[0],
        RegionTag(1, 1));
}

TEST(CompressedTextureDds, TransferSelectsBc7SrgbOrUnorm)
{
    const std::vector<uint8_t> bytes = BuildDds(DdsOptions{});

    TextureIO::LoadOptions linear;
    linear.formatHint = TextureIO::FileFormat::Dds;
    EXPECT_EQ(
        TextureIO::LoadFromMemory(bytes.data(), bytes.size(), linear)->format,
        HostImage::PixelFormat::BC7_UNORM);

    TextureIO::LoadOptions srgb = linear;
    srgb.transfer = TextureIO::LoadOptions::Transfer::SRGB;
    EXPECT_EQ(
        TextureIO::LoadFromMemory(bytes.data(), bytes.size(), srgb)->format,
        HostImage::PixelFormat::BC7_SRGB);

    // 容器是 BC7_UNORM_SRGB（99）；资产要求 linear 时按 UNORM 采样同一组块。
    DdsOptions srgbContainer;
    srgbContainer.dxgiFormat = 99;
    const std::vector<uint8_t> srgbBytes = BuildDds(srgbContainer);
    EXPECT_EQ(
        TextureIO::LoadFromMemory(srgbBytes.data(), srgbBytes.size(), linear)->format,
        HostImage::PixelFormat::BC7_UNORM);
}

TEST(CompressedTextureDds, DecodesLegacyAti2AsBc5Unorm)
{
    DdsOptions options;
    options.legacyBc5 = true;
    options.mipLevels = 2;
    options.arrayLayers = 1;
    const std::vector<uint8_t> bytes = BuildDds(options);

    TextureIO::LoadOptions load;
    load.formatHint = TextureIO::FileFormat::Dds;
    load.transfer = TextureIO::LoadOptions::Transfer::SRGB;
    const HostImage image =
        TextureIO::LoadFromMemory(bytes.data(), bytes.size(), load).value();
    EXPECT_EQ(image.format, HostImage::PixelFormat::BC5_UNORM);
    EXPECT_EQ(image.channels, 2u);
    EXPECT_EQ(image.mipLevels, 2u);
    EXPECT_EQ(image.data, ExpectedBlockImage(8, 8, 2, 1));
}

TEST_F(CompressedTextureTest, LoadsFileWithMipChainByExtensionSniff)
{
    DdsOptions options;
    options.mipLevels = 2;
    options.arrayLayers = 2;
    const auto path = WriteFile("T_layers.dds", BuildDds(options));

    TextureIO::LoadOptions load;
    load.transfer = TextureIO::LoadOptions::Transfer::SRGB;
    const auto image = TextureIO::Load(path, load);
    ASSERT_TRUE(image.has_value());
    EXPECT_EQ(image->format, HostImage::PixelFormat::BC7_SRGB);
    EXPECT_EQ(image->mipLevels, 2u);
    EXPECT_EQ(image->arrayLayers, 2u);
}

TEST(CompressedTextureDds, RejectsCubemapVolumeAndUnknownFormats)
{
    TextureIO::LoadOptions load;
    load.formatHint = TextureIO::FileFormat::Dds;

    DdsOptions cubemap;
    cubemap.miscFlag = 0x4;
    auto bytes = BuildDds(cubemap);
    EXPECT_THROW(TextureIO::LoadFromMemory(bytes.data(), bytes.size(), load), std::runtime_error);

    DdsOptions volume;
    volume.depth = 1;
    bytes = BuildDds(volume);
    EXPECT_THROW(TextureIO::LoadFromMemory(bytes.data(), bytes.size(), load), std::runtime_error);

    DdsOptions oneDimensional;
    oneDimensional.resourceDimension = 2; // TEXTURE1D
    bytes = BuildDds(oneDimensional);
    EXPECT_THROW(TextureIO::LoadFromMemory(bytes.data(), bytes.size(), load), std::runtime_error);

    DdsOptions bc1;
    bc1.dxgiFormat = 71;
    bytes = BuildDds(bc1);
    EXPECT_THROW(TextureIO::LoadFromMemory(bytes.data(), bytes.size(), load), std::runtime_error);

    DdsOptions snorm;
    snorm.dxgiFormat = 84; // BC5_SNORM
    bytes = BuildDds(snorm);
    EXPECT_THROW(TextureIO::LoadFromMemory(bytes.data(), bytes.size(), load), std::runtime_error);
}

TEST(CompressedTextureDds, RejectsTruncatedOrMalformedContainers)
{
    TextureIO::LoadOptions load;
    load.formatHint = TextureIO::FileFormat::Dds;

    DdsOptions truncated;
    truncated.mipLevels = 2;
    truncated.arrayLayers = 2;
    truncated.truncatePayload = true;
    auto bytes = BuildDds(truncated);
    EXPECT_THROW(TextureIO::LoadFromMemory(bytes.data(), bytes.size(), load), std::runtime_error);

    DdsOptions badMipCount;
    badMipCount.mipLevels = 2;
    badMipCount.mipCountField = 8;
    bytes = BuildDds(badMipCount);
    EXPECT_THROW(TextureIO::LoadFromMemory(bytes.data(), bytes.size(), load), std::runtime_error);

    DdsOptions misaligned;
    misaligned.width = 6;
    bytes = BuildDds(misaligned);
    EXPECT_THROW(TextureIO::LoadFromMemory(bytes.data(), bytes.size(), load), std::runtime_error);

    DdsOptions badPitch;
    badPitch.pitchOrLinearSize = 7;
    bytes = BuildDds(badPitch);
    EXPECT_THROW(TextureIO::LoadFromMemory(bytes.data(), bytes.size(), load), std::runtime_error);
}

TEST(CompressedTextureDds, RejectsRuntimeRowFlip)
{
    const std::vector<uint8_t> bytes = BuildDds(DdsOptions{});
    TextureIO::LoadOptions load;
    load.formatHint = TextureIO::FileFormat::Dds;
    load.flipY = TextureIO::LoadOptions::FlipYMode::ForceOn;
    try
    {
        TextureIO::LoadFromMemory(bytes.data(), bytes.size(), load);
        FAIL() << "BC payloads cannot be row-flipped at runtime";
    }
    catch (const std::runtime_error& error)
    {
        EXPECT_NE(std::string(error.what()).find("block-compressed"), std::string::npos);
    }
}

TEST_F(CompressedTextureTest, LoadArrayKeepsPerLayerMipChains)
{
    DdsOptions options;
    options.mipLevels = 2;
    const auto first = WriteFile("T_layer0.dds", BuildDds(options));
    const auto second = WriteFile("T_layer1.dds", BuildDds(options));

    TextureIO::LoadOptions load;
    load.transfer = TextureIO::LoadOptions::Transfer::Linear;
    const HostImage image = TextureIO::LoadArray({first, second}, load);
    EXPECT_EQ(image.arrayLayers, 2u);
    EXPECT_EQ(image.mipLevels, 2u);
    EXPECT_EQ(image.format, HostImage::PixelFormat::BC7_UNORM);
    EXPECT_EQ(static_cast<const uint8_t*>(image.GetMipData(1, 1))[0], RegionTag(0, 1));
}

TEST(CompressedTextureKtx2, ReordersMipMajorContainerIntoLayerMajorHostImage)
{
    Ktx2Options options;
    options.mipLevels = 2;
    options.arrayLayers = 2;
    const std::vector<uint8_t> bytes = BuildKtx2(options);

    TextureIO::LoadOptions load;
    load.formatHint = TextureIO::FileFormat::Ktx2;
    load.transfer = TextureIO::LoadOptions::Transfer::SRGB;
    const HostImage image =
        TextureIO::LoadFromMemory(bytes.data(), bytes.size(), load).value();

    EXPECT_EQ(image.format, HostImage::PixelFormat::BC7_SRGB);
    EXPECT_EQ(image.mipLevels, 2u);
    EXPECT_EQ(image.arrayLayers, 2u);
    EXPECT_EQ(image.data, ExpectedBlockImage(8, 8, 2, 2));
    EXPECT_EQ(static_cast<const uint8_t*>(image.GetMipData(0, 1))[0], RegionTag(1, 0));
    EXPECT_EQ(static_cast<const uint8_t*>(image.GetMipData(1, 0))[0], RegionTag(0, 1));
}

TEST(CompressedTextureKtx2, TransferOverridesContainerBc7Srgb)
{
    Ktx2Options options;
    options.vkFormat = 146; // BC7_SRGB
    options.transferFunction = 2;
    const std::vector<uint8_t> bytes = BuildKtx2(options);

    TextureIO::LoadOptions load;
    load.formatHint = TextureIO::FileFormat::Ktx2;
    load.transfer = TextureIO::LoadOptions::Transfer::Linear;
    EXPECT_EQ(
        TextureIO::LoadFromMemory(bytes.data(), bytes.size(), load)->format,
        HostImage::PixelFormat::BC7_UNORM);

    load.transfer = TextureIO::LoadOptions::Transfer::SRGB;
    EXPECT_EQ(
        TextureIO::LoadFromMemory(bytes.data(), bytes.size(), load)->format,
        HostImage::PixelFormat::BC7_SRGB);
}

TEST(CompressedTextureKtx2, DecodesBc5AsUnorm)
{
    Ktx2Options options;
    options.vkFormat = 141;
    options.colorModel = 132;
    options.mipLevels = 2;
    const std::vector<uint8_t> bytes = BuildKtx2(options);

    TextureIO::LoadOptions load;
    load.formatHint = TextureIO::FileFormat::Ktx2;
    load.transfer = TextureIO::LoadOptions::Transfer::SRGB;
    const HostImage image =
        TextureIO::LoadFromMemory(bytes.data(), bytes.size(), load).value();
    EXPECT_EQ(image.format, HostImage::PixelFormat::BC5_UNORM);
    EXPECT_EQ(image.channels, 2u);
}

TEST(CompressedTextureKtx2, RejectsSupercompressionAndBasis)
{
    TextureIO::LoadOptions load;
    load.formatHint = TextureIO::FileFormat::Ktx2;
    for (uint32_t scheme : {1u, 2u, 3u})
    {
        Ktx2Options options;
        options.supercompression = scheme;
        const std::vector<uint8_t> bytes = BuildKtx2(options);
        try
        {
            TextureIO::LoadFromMemory(bytes.data(), bytes.size(), load);
            FAIL() << "supercompression must be rejected";
        }
        catch (const std::runtime_error& error)
        {
            const std::string message(error.what());
            EXPECT_NE(message.find("BC7/BC5"), std::string::npos) << message;
        }
    }
}

TEST(CompressedTextureKtx2, RejectsCubeVolumeAndUnknownFormats)
{
    TextureIO::LoadOptions load;
    load.formatHint = TextureIO::FileFormat::Ktx2;

    Ktx2Options cube;
    cube.faceCount = 6;
    auto bytes = BuildKtx2(cube);
    EXPECT_THROW(TextureIO::LoadFromMemory(bytes.data(), bytes.size(), load), std::runtime_error);

    Ktx2Options volume;
    volume.pixelDepth = 1;
    bytes = BuildKtx2(volume);
    EXPECT_THROW(TextureIO::LoadFromMemory(bytes.data(), bytes.size(), load), std::runtime_error);

    Ktx2Options rgba8;
    rgba8.vkFormat = 37; // VK_FORMAT_R8G8B8A8_UNORM
    bytes = BuildKtx2(rgba8);
    EXPECT_THROW(TextureIO::LoadFromMemory(bytes.data(), bytes.size(), load), std::runtime_error);

    Ktx2Options wrongModel;
    wrongModel.colorModel = 132; // BC5 DFD on a BC7 vkFormat
    bytes = BuildKtx2(wrongModel);
    EXPECT_THROW(TextureIO::LoadFromMemory(bytes.data(), bytes.size(), load), std::runtime_error);
}

TEST(CompressedTextureKtx2, RejectsZeroLevelsTruncationAndBadIdentifier)
{
    TextureIO::LoadOptions load;
    load.formatHint = TextureIO::FileFormat::Ktx2;

    Ktx2Options generatedMips;
    generatedMips.levelCountField = 0;
    auto bytes = BuildKtx2(generatedMips);
    try
    {
        TextureIO::LoadFromMemory(bytes.data(), bytes.size(), load);
        FAIL() << "levelCount=0 requires offline mips";
    }
    catch (const std::runtime_error& error)
    {
        EXPECT_NE(std::string(error.what()).find("offline mip chain"), std::string::npos);
    }

    Ktx2Options truncated;
    truncated.mipLevels = 2;
    truncated.truncateLevelData = true;
    bytes = BuildKtx2(truncated);
    EXPECT_THROW(TextureIO::LoadFromMemory(bytes.data(), bytes.size(), load), std::runtime_error);

    bytes = BuildKtx2(Ktx2Options{});
    bytes[0] = 0;
    EXPECT_THROW(TextureIO::LoadFromMemory(bytes.data(), bytes.size(), load), std::runtime_error);
}

TEST(CompressedTextureVulkanPlan, MapsBcFormatsToVulkanBlockFormats)
{
    EXPECT_EQ(
        HostImageVulkan::ToVulkanFormat(HostImage::PixelFormat::BC7_SRGB),
        vk::Format::eBc7SrgbBlock);
    EXPECT_EQ(
        HostImageVulkan::ToVulkanFormat(HostImage::PixelFormat::BC7_UNORM),
        vk::Format::eBc7UnormBlock);
    EXPECT_EQ(
        HostImageVulkan::ToVulkanFormat(HostImage::PixelFormat::BC5_UNORM),
        vk::Format::eBc5UnormBlock);
}

TEST(CompressedTextureVulkanPlan, BuildsOneRegionPerMipAndLayerWithBlockStrides)
{
    HostImage image;
    image.width = 8;
    image.height = 8;
    image.mipLevels = 2;
    image.arrayLayers = 2;
    image.format = HostImage::PixelFormat::BC7_SRGB;
    image.data.assign(160, 0);

    const std::vector<vk::BufferImageCopy> regions =
        HostImageVulkan::BuildUploadRegions(image);
    ASSERT_EQ(regions.size(), 4u);

    const vk::BufferImageCopy& first = regions[0];
    EXPECT_EQ(first.bufferOffset, 0u);
    EXPECT_EQ(first.bufferRowLength, 0u);
    EXPECT_EQ(first.imageSubresource.mipLevel, 0u);
    EXPECT_EQ(first.imageSubresource.baseArrayLayer, 0u);
    EXPECT_EQ(first.imageExtent.width, 8u);
    EXPECT_EQ(first.imageExtent.height, 8u);

    EXPECT_EQ(regions[1].bufferOffset, 64u);
    EXPECT_EQ(regions[1].imageSubresource.mipLevel, 1u);
    EXPECT_EQ(regions[1].imageExtent.width, 4u);

    EXPECT_EQ(regions[2].bufferOffset, 80u);
    EXPECT_EQ(regions[2].imageSubresource.mipLevel, 0u);
    EXPECT_EQ(regions[2].imageSubresource.baseArrayLayer, 1u);

    EXPECT_EQ(regions[3].bufferOffset, 144u);
}

TEST(CompressedTextureVulkanPlan, KeepsPaddedRowStrideInTexels)
{
    HostImage image;
    image.width = 2;
    image.height = 2;
    image.rowStrideBytes = 12;
    image.data.assign(24, 0);

    const std::vector<vk::BufferImageCopy> regions =
        HostImageVulkan::BuildUploadRegions(image);
    ASSERT_EQ(regions.size(), 1u);
    EXPECT_EQ(regions[0].bufferRowLength, 3u);
    EXPECT_EQ(regions[0].imageExtent.width, 2u);
}
