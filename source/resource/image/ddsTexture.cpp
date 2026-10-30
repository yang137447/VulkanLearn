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
    constexpr uint32_t DdsMagic = 0x20534444u;
    constexpr uint32_t DdsHeaderSize = 124u;
    constexpr size_t DdsPixelDataOffset = 128u;
    constexpr size_t DdsDx10HeaderSize = 20u;

    constexpr uint32_t DdsFourCcDx10 = 0x30315844u;
    constexpr uint32_t DdsFourCcAti2 = 0x32495441u;
    constexpr uint32_t DdsFourCcBc5U = 0x55354342u;

    constexpr uint32_t DdsdCaps = 0x1u;
    constexpr uint32_t DdsdHeight = 0x2u;
    constexpr uint32_t DdsdWidth = 0x4u;
    constexpr uint32_t DdsdPixelFormat = 0x1000u;
    constexpr uint32_t DdpfFourCc = 0x4u;
    constexpr uint32_t DdscapsTexture = 0x1000u;
    constexpr uint32_t Ddscaps2Cubemap = 0x200u;
    constexpr uint32_t Ddscaps2Volume = 0x200000u;

    constexpr uint32_t DxgiBc5Unorm = 83u;
    constexpr uint32_t DxgiBc5Snorm = 84u;
    constexpr uint32_t DxgiBc7Unorm = 98u;
    constexpr uint32_t DxgiBc7UnormSrgb = 99u;

    // D3D10_RESOURCE_DIMENSION: BUFFER=1, TEXTURE1D=2, TEXTURE2D=3, TEXTURE3D=4。
    constexpr uint32_t D3d10ResourceDimensionTexture1D = 2u;
    constexpr uint32_t D3d10ResourceDimensionTexture2D = 3u;
    constexpr uint32_t D3d10ResourceDimensionTexture3D = 4u;

    // DDS 是严格的小端容器；逐字段 memcpy 读取，避免未对齐指针转换。
    uint32_t ReadU32(const uint8_t* data, size_t offset)
    {
        uint32_t value = 0;
        std::memcpy(&value, data + offset, sizeof(value));
        return value;
    }

    uint32_t FullMipChainLength(uint32_t width, uint32_t height)
    {
        uint32_t levels = 1;
        uint32_t currentWidth = width;
        uint32_t currentHeight = height;
        while (currentWidth > 1 || currentHeight > 1)
        {
            currentWidth = std::max(1u, currentWidth >> 1);
            currentHeight = std::max(1u, currentHeight >> 1);
            ++levels;
        }
        return levels;
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

    size_t LayerBytes(uint32_t width, uint32_t height, uint32_t mipLevels)
    {
        size_t bytes = 0;
        for (uint32_t mip = 0; mip < mipLevels; ++mip)
        {
            const uint32_t mipWidth = mip == 0 ? width : std::max(1u, width >> mip);
            const uint32_t mipHeight = mip == 0 ? height : std::max(1u, height >> mip);
            const size_t mipBytes = BlockRowBytes(mipWidth) * BlockRowCount(mipHeight);
            if (bytes > std::numeric_limits<size_t>::max() - mipBytes)
                throw std::runtime_error("DDS mip chain size overflows the address space");
            bytes += mipBytes;
        }
        return bytes;
    }

    size_t CheckedArrayBytes(size_t layerBytes, uint32_t arrayLayers)
    {
        if (arrayLayers != 0 && layerBytes > std::numeric_limits<size_t>::max() / arrayLayers)
            throw std::runtime_error("DDS array size overflows the address space");
        return layerBytes * arrayLayers;
    }

    HostImage::PixelFormat ResolveBc7Format(bool srgb)
    {
        return srgb ? HostImage::PixelFormat::BC7_SRGB : HostImage::PixelFormat::BC7_UNORM;
    }

    [[noreturn]] void ThrowUnsupportedFormat(const std::string& detail)
    {
        throw std::runtime_error(
            "DDS texture format is not supported: " + detail +
            ". Only BC7_UNORM/BC7_UNORM_SRGB and BC5_UNORM are accepted.");
    }
}

bool CompressedTextureDecoder::MatchesDds(const void* data, size_t size)
{
    if (data == nullptr || size < sizeof(uint32_t))
        return false;
    return ReadU32(static_cast<const uint8_t*>(data), 0) == DdsMagic;
}

HostImage CompressedTextureDecoder::DecodeDds(
    const void* data,
    size_t size,
    HostImage::TextureSemantic semantic,
    bool srgb)
{
    const uint8_t* bytes = static_cast<const uint8_t*>(data);
    if (!MatchesDds(data, size) || size < DdsPixelDataOffset)
        throw std::runtime_error("DDS texture is truncated before its header");

    if (ReadU32(bytes, 4) != DdsHeaderSize)
        throw std::runtime_error("DDS header size must be 124 bytes");

    const uint32_t flags = ReadU32(bytes, 8);
    const uint32_t height = ReadU32(bytes, 12);
    const uint32_t width = ReadU32(bytes, 16);
    const uint32_t pitchOrLinearSize = ReadU32(bytes, 20);
    const uint32_t depth = ReadU32(bytes, 24);
    const uint32_t mipCountRaw = ReadU32(bytes, 28);

    const uint32_t pixelFormatSize = ReadU32(bytes, 76);
    const uint32_t pixelFormatFlags = ReadU32(bytes, 80);
    const uint32_t fourCc = ReadU32(bytes, 84);
    const uint32_t caps = ReadU32(bytes, 104);
    const uint32_t caps2 = ReadU32(bytes, 108);

    if ((flags & (DdsdCaps | DdsdHeight | DdsdWidth | DdsdPixelFormat)) !=
        (DdsdCaps | DdsdHeight | DdsdWidth | DdsdPixelFormat))
        throw std::runtime_error("DDS header is missing required capability/size flags");
    if (pixelFormatSize != 32)
        throw std::runtime_error("DDS pixel format size must be 32 bytes");
    if ((caps & DdscapsTexture) == 0)
        throw std::runtime_error("DDS texture requires the DDSCAPS_TEXTURE capability");
    if (depth != 0 || (caps2 & Ddscaps2Volume) != 0)
        throw std::runtime_error("Volume DDS textures are not supported");
    if ((caps2 & Ddscaps2Cubemap) != 0)
        throw std::runtime_error("Cubemap DDS textures are not supported");
    if (width == 0 || height == 0)
        throw std::runtime_error("DDS texture dimensions must be non-zero");
    if (width % HostImage::TexelBlockSize != 0 || height % HostImage::TexelBlockSize != 0)
        throw std::runtime_error(
            "Block-compressed DDS base level requires width and height to be multiples of 4");

    HostImage::PixelFormat format = HostImage::PixelFormat::BC7_UNORM;
    uint32_t arrayLayers = 1;
    size_t dataOffset = DdsPixelDataOffset;

    if ((pixelFormatFlags & DdpfFourCc) == 0)
    {
        throw std::runtime_error(
            "DDS pixel format must be a BC7/BC5 FourCC or a DX10 header");
    }

    if (fourCc == DdsFourCcDx10)
    {
        if (size < DdsPixelDataOffset + DdsDx10HeaderSize)
            throw std::runtime_error("DDS DX10 header is truncated");

        const uint32_t dxgiFormat = ReadU32(bytes, 128);
        const uint32_t resourceDimension = ReadU32(bytes, 132);
        const uint32_t miscFlag = ReadU32(bytes, 136);
        arrayLayers = ReadU32(bytes, 140);

        if (miscFlag != 0)
        {
            // D3D10_RESOURCE_MISC_TEXTURECUBE = 0x4；其余位当前都没有合法语义。
            if ((miscFlag & 0x4u) != 0)
                throw std::runtime_error("Cubemap DDS textures are not supported");
            throw std::runtime_error("DDS DX10 miscFlags are not supported");
        }
        if (resourceDimension == 1)
            throw std::runtime_error("DDS DX10 resource dimension BUFFER is not a texture");
        if (resourceDimension == D3d10ResourceDimensionTexture1D)
            throw std::runtime_error("1D DDS textures are not supported");
        if (resourceDimension == D3d10ResourceDimensionTexture3D)
            throw std::runtime_error("3D/volume DDS textures are not supported");
        if (resourceDimension != D3d10ResourceDimensionTexture2D)
            throw std::runtime_error("DDS DX10 resource dimension must be TEXTURE2D");
        if (arrayLayers == 0)
            throw std::runtime_error("DDS DX10 arraySize must be at least one");

        switch (dxgiFormat)
        {
        case DxgiBc7Unorm:
        case DxgiBc7UnormSrgb:
            // sRGB 解释由资产的 colorSpace 决定，容器位深一致；两者都产出同一组 BC7 块。
            format = ResolveBc7Format(srgb);
            break;
        case DxgiBc5Unorm:
            format = HostImage::PixelFormat::BC5_UNORM;
            break;
        case DxgiBc5Snorm:
            ThrowUnsupportedFormat("BC5_SNORM");
        default:
            ThrowUnsupportedFormat("DXGI format " + std::to_string(dxgiFormat));
        }
        dataOffset += DdsDx10HeaderSize;
    }
    else if (fourCc == DdsFourCcAti2 || fourCc == DdsFourCcBc5U)
    {
        format = HostImage::PixelFormat::BC5_UNORM;
    }
    else
    {
        ThrowUnsupportedFormat("FourCC 0x" + std::to_string(fourCc));
    }

    const uint32_t fullChain = FullMipChainLength(width, height);
    const uint32_t mipLevels = mipCountRaw == 0 ? 1u : mipCountRaw;
    if (mipLevels > fullChain)
    {
        throw std::runtime_error(
            "DDS mip count " + std::to_string(mipLevels) +
            " exceeds the full chain length " + std::to_string(fullChain));
    }

    const size_t layerBytes = LayerBytes(width, height, mipLevels);
    // DX10 数组按 array element 存完整 mip 链，正好等于 HostImage 的 layer-major 布局。
    const size_t totalBytes = CheckedArrayBytes(layerBytes, arrayLayers);
    if (totalBytes == 0 || totalBytes > size - dataOffset)
    {
        throw std::runtime_error(
            "DDS pixel data is truncated: expected " + std::to_string(totalBytes) +
            " bytes, found " + std::to_string(size - dataOffset));
    }

    // 两种常见写法都接受：块行字节数（texconv 风格）或顶层 mip 总字节数。
    const size_t topMipRowBytes = BlockRowBytes(width);
    const size_t topMipBytes = topMipRowBytes * BlockRowCount(height);
    if (pitchOrLinearSize != 0 &&
        pitchOrLinearSize != topMipRowBytes &&
        pitchOrLinearSize != topMipBytes)
    {
        throw std::runtime_error(
            "DDS pitchOrLinearSize does not match the BC top mip size");
    }

    HostImage image;
    image.width = width;
    image.height = height;
    image.channels = format == HostImage::PixelFormat::BC5_UNORM ? 2u : 4u;
    image.mipLevels = mipLevels;
    image.arrayLayers = arrayLayers;
    image.format = format;
    image.semantic = semantic;
    // 块数据必须紧凑；0 表示按块行自动计算跨度。
    image.rowStrideBytes = 0;
    image.data.assign(bytes + dataOffset, bytes + dataOffset + totalBytes);
    image.ValidateStorage();
    return image;
}
