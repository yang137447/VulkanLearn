#include "compressedTextureDecoder.h"

#include <algorithm>
#include <cctype>
#include <cstring>
#include <limits>
#include <stdexcept>
#include <string>
#include <vector>

namespace
{
    // «KTX 20»\r\n\x1A\n
    constexpr uint8_t Ktx2Identifier[12] = {
        0xAB, 0x4B, 0x54, 0x58, 0x20, 0x32, 0x30, 0xBB,
        0x0D, 0x0A, 0x1A, 0x0A };

    constexpr size_t Ktx2HeaderSize = 80u; // identifier + header + index
    constexpr size_t Ktx2LevelIndexEntrySize = 24u;

    // VkFormat 编号（vulkan_core.h）
    constexpr uint32_t VkFormatBc5UnormBlock = 141u;
    constexpr uint32_t VkFormatBc7UnormBlock = 145u;
    constexpr uint32_t VkFormatBc7SrgbBlock = 146u;

    // KHR_DF_MODEL_* / KHR_DF_TRANSFER_*
    constexpr uint8_t DfdModelBc5 = 132u;
    constexpr uint8_t DfdModelBc7 = 134u;
    constexpr uint8_t DfdTransferLinear = 1u;
    constexpr uint8_t DfdTransferSrgb = 2u;

    uint32_t ReadU32(const uint8_t* data, size_t offset)
    {
        uint32_t value = 0;
        std::memcpy(&value, data + offset, sizeof(value));
        return value;
    }

    uint64_t ReadU64(const uint8_t* data, size_t offset)
    {
        uint64_t value = 0;
        std::memcpy(&value, data + offset, sizeof(value));
        return value;
    }

    bool HasRange(size_t offset, size_t length, size_t size)
    {
        return offset <= size && length <= size - offset;
    }

    size_t BlockRowBytes(uint32_t mipWidth)
    {
        const uint32_t blocksPerRow =
            (mipWidth + HostImage::TexelBlockSize - 1) / HostImage::TexelBlockSize;
        return static_cast<size_t>(blocksPerRow) * HostImage::TexelBlockBytes;
    }

    size_t BlockRowCount(uint32_t mipHeight)
    {
        return (mipHeight + HostImage::TexelBlockSize - 1) / HostImage::TexelBlockSize;
    }

    size_t CheckedMultiply(size_t lhs, size_t rhs, const char* message)
    {
        if (rhs != 0 && lhs > std::numeric_limits<size_t>::max() / rhs)
            throw std::runtime_error(message);
        return lhs * rhs;
    }

    size_t CheckedAdd(size_t lhs, size_t rhs, const char* message)
    {
        if (lhs > std::numeric_limits<size_t>::max() - rhs)
            throw std::runtime_error(message);
        return lhs + rhs;
    }

    const char* SupercompressionName(uint32_t scheme)
    {
        switch (scheme)
        {
        case 0: return "NONE";
        case 1: return "BasisLZ";
        case 2: return "Zstandard";
        case 3: return "ZLIB";
        default: return "unknown";
        }
    }

    // DFD 描述块的固定字段；BC7/BC5 必须声明 4×4 纹素、16 字节/块。
    struct DfdDescriptor
    {
        uint8_t colorModel = 0;
        uint8_t transferFunction = 0;
        uint8_t blockDimensions[4] = { 0, 0, 0, 0 };
        uint8_t bytesPlane0 = 0;
    };

    DfdDescriptor ParseDfd(const uint8_t* bytes, size_t size, uint32_t dfdOffset, uint32_t dfdLength)
    {
        if (dfdLength == 0 || !HasRange(dfdOffset, dfdLength, size))
            throw std::runtime_error("KTX2 data format descriptor is missing or out of range");
        const uint32_t dfdTotalSize = ReadU32(bytes, dfdOffset);
        if (dfdTotalSize < 4 + 24 || dfdTotalSize > dfdLength)
            throw std::runtime_error("KTX2 data format descriptor size is invalid");

        const size_t blockOffset = static_cast<size_t>(dfdOffset) + 4;
        const uint32_t descriptorBlockSize = ReadU32(bytes, blockOffset + 4) >> 16;
        if (descriptorBlockSize < 24 || dfdTotalSize < 4 + descriptorBlockSize)
            throw std::runtime_error("KTX2 descriptor block is truncated");

        DfdDescriptor descriptor;
        descriptor.colorModel = bytes[blockOffset + 8];
        descriptor.transferFunction = bytes[blockOffset + 10];
        for (size_t index = 0; index < 4; ++index)
            descriptor.blockDimensions[index] = bytes[blockOffset + 12 + index];
        descriptor.bytesPlane0 = bytes[blockOffset + 16];
        return descriptor;
    }
}

bool CompressedTextureDecoder::MatchesKtx2(const void* data, size_t size)
{
    if (data == nullptr || size < sizeof(Ktx2Identifier))
        return false;
    return std::memcmp(data, Ktx2Identifier, sizeof(Ktx2Identifier)) == 0;
}

bool CompressedTextureDecoder::IsCompressedContainerPath(const std::filesystem::path& path)
{
    std::string extension = path.extension().string();
    std::transform(extension.begin(), extension.end(), extension.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });
    return extension == ".dds" || extension == ".ktx2";
}

HostImage CompressedTextureDecoder::DecodeKtx2(
    const void* data,
    size_t size,
    HostImage::TextureSemantic semantic,
    bool srgb)
{
    const uint8_t* bytes = static_cast<const uint8_t*>(data);
    if (!MatchesKtx2(data, size) || size < Ktx2HeaderSize)
        throw std::runtime_error("KTX2 texture is truncated before its level index");

    const uint32_t vkFormat = ReadU32(bytes, 12);
    const uint32_t typeSize = ReadU32(bytes, 16);
    const uint32_t pixelWidth = ReadU32(bytes, 20);
    const uint32_t pixelHeight = ReadU32(bytes, 24);
    const uint32_t pixelDepth = ReadU32(bytes, 28);
    const uint32_t layerCountRaw = ReadU32(bytes, 32);
    const uint32_t faceCount = ReadU32(bytes, 36);
    const uint32_t levelCount = ReadU32(bytes, 40);
    const uint32_t supercompressionScheme = ReadU32(bytes, 44);
    const uint32_t dfdByteOffset = ReadU32(bytes, 48);
    const uint32_t dfdByteLength = ReadU32(bytes, 52);

    if (supercompressionScheme != 0)
    {
        throw std::runtime_error(
            std::string("KTX2 supercompression scheme ") +
            SupercompressionName(supercompressionScheme) + " (" +
            std::to_string(supercompressionScheme) +
            ") is not supported. Transcode Basis/Zstd/ZLIB data to BC7/BC5 offline first.");
    }
    if (typeSize == 0)
        throw std::runtime_error("KTX2 typeSize must be non-zero");
    if (pixelWidth == 0 || pixelHeight == 0)
        throw std::runtime_error("KTX2 must be a 2D texture with non-zero width and height");
    if (pixelDepth != 0)
        throw std::runtime_error("3D/volume KTX2 textures are not supported");
    if (faceCount != 1)
        throw std::runtime_error("Cubemap KTX2 textures are not supported");
    if (levelCount == 0)
    {
        throw std::runtime_error(
            "KTX2 levelCount=0 asks the loader to generate mips; "
            "BC textures require an offline mip chain");
    }
    if (pixelWidth % HostImage::TexelBlockSize != 0 ||
        pixelHeight % HostImage::TexelBlockSize != 0)
    {
        throw std::runtime_error(
            "Block-compressed KTX2 base level requires width and height to be multiples of 4");
    }
    if (!HasRange(80, static_cast<size_t>(levelCount) * Ktx2LevelIndexEntrySize, size))
        throw std::runtime_error("KTX2 level index is truncated");

    HostImage::PixelFormat format = HostImage::PixelFormat::BC7_UNORM;
    bool expectsSrgbTransfer = false;
    switch (vkFormat)
    {
    case VkFormatBc7UnormBlock:
    case VkFormatBc7SrgbBlock:
        format = srgb ? HostImage::PixelFormat::BC7_SRGB : HostImage::PixelFormat::BC7_UNORM;
        expectsSrgbTransfer = vkFormat == VkFormatBc7SrgbBlock;
        break;
    case VkFormatBc5UnormBlock:
        format = HostImage::PixelFormat::BC5_UNORM;
        break;
    default:
        throw std::runtime_error(
            "KTX2 vkFormat " + std::to_string(vkFormat) +
            " is not supported. Only BC5_UNORM, BC7_UNORM and BC7_SRGB are accepted.");
    }

    // DFD 校验容器自洽性：BC7/BC5 都是 4×4×1×1、16 字节/块，
    // 且传输函数必须与 vkFormat 的 sRGB 位一致。
    const DfdDescriptor descriptor = ParseDfd(bytes, size, dfdByteOffset, dfdByteLength);
    const uint8_t expectedModel =
        format == HostImage::PixelFormat::BC5_UNORM ? DfdModelBc5 : DfdModelBc7;
    if (descriptor.colorModel != expectedModel)
    {
        throw std::runtime_error(
            "KTX2 data format descriptor color model does not match vkFormat");
    }
    if (descriptor.blockDimensions[0] != HostImage::TexelBlockSize - 1 ||
        descriptor.blockDimensions[1] != HostImage::TexelBlockSize - 1 ||
        descriptor.blockDimensions[2] != 0 ||
        descriptor.blockDimensions[3] != 0 ||
        descriptor.bytesPlane0 != HostImage::TexelBlockBytes)
    {
        throw std::runtime_error(
            "KTX2 data format descriptor is not a 4x4/16-byte BC block layout");
    }
    const uint8_t expectedTransfer =
        expectsSrgbTransfer ? DfdTransferSrgb : DfdTransferLinear;
    if (descriptor.transferFunction != expectedTransfer)
    {
        throw std::runtime_error(
            "KTX2 data format descriptor transfer function does not match vkFormat");
    }

    const uint32_t arrayLayers = layerCountRaw == 0 ? 1u : layerCountRaw;

    uint32_t fullChain = 1;
    for (uint32_t w = pixelWidth, h = pixelHeight; w > 1 || h > 1;)
    {
        w = std::max(1u, w >> 1);
        h = std::max(1u, h >> 1);
        ++fullChain;
    }
    if (levelCount > fullChain)
    {
        throw std::runtime_error(
            "KTX2 level count " + std::to_string(levelCount) +
            " exceeds the full chain length " + std::to_string(fullChain));
    }

    size_t layerBytes = 0;
    for (uint32_t mip = 0; mip < levelCount; ++mip)
    {
        const uint32_t mipWidth = mip == 0 ? pixelWidth : std::max(1u, pixelWidth >> mip);
        const uint32_t mipHeight = mip == 0 ? pixelHeight : std::max(1u, pixelHeight >> mip);
        layerBytes = CheckedAdd(
            layerBytes,
            BlockRowBytes(mipWidth) * BlockRowCount(mipHeight),
            "KTX2 mip chain size overflows the address space");
    }

    HostImage image;
    image.width = pixelWidth;
    image.height = pixelHeight;
    image.channels = format == HostImage::PixelFormat::BC5_UNORM ? 2u : 4u;
    image.mipLevels = levelCount;
    image.arrayLayers = arrayLayers;
    image.format = format;
    image.semantic = semantic;
    image.rowStrideBytes = 0;
    image.data.resize(CheckedMultiply(
        layerBytes,
        arrayLayers,
        "KTX2 array size overflows the address space"));

    // KTX2 每个 level 的数据块内是 layer-major（一个 mip 的全部层连续），
    // 而 HostImage 是 layer-major、层内 mip-major，因此这里必须按 level/layer 重排。
    size_t hostMipOffset = 0;
    for (uint32_t mip = 0; mip < levelCount; ++mip)
    {
        const size_t levelIndexOffset = 80 + static_cast<size_t>(mip) * Ktx2LevelIndexEntrySize;
        const uint64_t levelByteOffset = ReadU64(bytes, levelIndexOffset);
        const uint64_t levelByteLength = ReadU64(bytes, levelIndexOffset + 8);
        if (levelByteOffset > size || levelByteLength > size - levelByteOffset)
            throw std::runtime_error("KTX2 level " + std::to_string(mip) + " is out of range");

        const uint32_t mipWidth = mip == 0 ? pixelWidth : std::max(1u, pixelWidth >> mip);
        const uint32_t mipHeight = mip == 0 ? pixelHeight : std::max(1u, pixelHeight >> mip);
        const size_t mipLayerBytes = BlockRowBytes(mipWidth) * BlockRowCount(mipHeight);
        const size_t expectedLevelBytes = CheckedMultiply(
            mipLayerBytes,
            arrayLayers,
            "KTX2 level size overflows the address space");
        if (levelByteLength < expectedLevelBytes)
        {
            throw std::runtime_error(
                "KTX2 level " + std::to_string(mip) + " is truncated: expected " +
                std::to_string(expectedLevelBytes) + " bytes, found " +
                std::to_string(levelByteLength));
        }

        for (uint32_t layer = 0; layer < arrayLayers; ++layer)
        {
            const size_t sourceOffset = static_cast<size_t>(levelByteOffset) +
                static_cast<size_t>(layer) * mipLayerBytes;
            const size_t destinationOffset =
                static_cast<size_t>(layer) * layerBytes + hostMipOffset;
            std::memcpy(
                image.data.data() + destinationOffset,
                bytes + sourceOffset,
                mipLayerBytes);
        }
        hostMipOffset += mipLayerBytes;
    }

    image.ValidateStorage();
    return image;
}
