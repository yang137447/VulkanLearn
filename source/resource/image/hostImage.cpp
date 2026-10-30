#include "hostImage.h"

#include <algorithm>
#include <limits>
#include <stdexcept>
#include <utility>

namespace
{
    size_t CheckedMultiply(size_t lhs, size_t rhs, const char* message)
    {
        if (rhs != 0 && lhs > std::numeric_limits<size_t>::max() / rhs)
            throw std::length_error(message);
        return lhs * rhs;
    }

    size_t CheckedAdd(size_t lhs, size_t rhs, const char* message)
    {
        if (lhs > std::numeric_limits<size_t>::max() - rhs)
            throw std::length_error(message);
        return lhs + rhs;
    }

    size_t BytesPerElement(HostImage::PixelFormat format)
    {
        switch (format)
        {
        case HostImage::PixelFormat::R8_UNORM:
        case HostImage::PixelFormat::R8_SRGB:
            return 1;
        case HostImage::PixelFormat::RG8_UNORM:
        case HostImage::PixelFormat::RG8_SRGB:
            return 2;
        case HostImage::PixelFormat::RGBA8_UNORM:
        case HostImage::PixelFormat::RGBA8_SRGB:
            return 4;
        case HostImage::PixelFormat::R16_FLOAT:
            return 2;
        case HostImage::PixelFormat::RG16_FLOAT:
            return 4;
        case HostImage::PixelFormat::RGBA16_FLOAT:
            return 8;
        case HostImage::PixelFormat::R32_FLOAT:
            return 4;
        case HostImage::PixelFormat::RGBA32_FLOAT:
            return 16;
        case HostImage::PixelFormat::BC7_UNORM:
        case HostImage::PixelFormat::BC7_SRGB:
        case HostImage::PixelFormat::BC5_UNORM:
            return HostImage::TexelBlockBytes;
        default:
            throw std::runtime_error("Unsupported HostImage::PixelFormat");
        }
    }

    bool IsMipIndexValid(const HostImage& image, uint32_t mip)
    {
        return mip < image.mipLevels;
    }

    bool SameSwizzle(const HostImage::ChannelSwizzle& lhs, const HostImage::ChannelSwizzle& rhs)
    {
        return lhs.r == rhs.r && lhs.g == rhs.g && lhs.b == rhs.b && lhs.a == rhs.a;
    }

    uint32_t ShiftedMipDimension(uint32_t base, uint32_t mip)
    {
        // mip 数上限在 ValidateStorage 里限制到 32，避免移位越界。
        return mip == 0 ? base : std::max(1u, base >> mip);
    }
}

bool HostImage::IsBlockCompressed(PixelFormat format)
{
    return format == PixelFormat::BC7_UNORM || format == PixelFormat::BC7_SRGB ||
        format == PixelFormat::BC5_UNORM;
}

bool HostImage::IsSRGBFormat(PixelFormat format)
{
    return format == PixelFormat::R8_SRGB || format == PixelFormat::RG8_SRGB ||
        format == PixelFormat::RGBA8_SRGB || format == PixelFormat::BC7_SRGB;
}

uint32_t HostImage::BytesPerTexelOrBlock(PixelFormat format)
{
    return static_cast<uint32_t>(BytesPerElement(format));
}

size_t HostImage::GetSize() const
{
    return data.size();
}

size_t HostImage::GetPixelSize() const
{
    if (IsBlockCompressed(format))
    {
        // 块格式的“像素”不是一个纹素，按 width*16 计算会越界读写。
        throw std::runtime_error(
            "HostImage::GetPixelSize is undefined for block-compressed formats; "
            "use BytesPerTexelOrBlock/GetMipRowStride");
    }
    return BytesPerElement(format);
}

uint32_t HostImage::GetMipWidth(uint32_t mip) const
{
    if (!IsMipIndexValid(*this, mip))
        throw std::invalid_argument("HostImage mip level is out of range");
    return ShiftedMipDimension(width, mip);
}

uint32_t HostImage::GetMipHeight(uint32_t mip) const
{
    if (!IsMipIndexValid(*this, mip))
        throw std::invalid_argument("HostImage mip level is out of range");
    return ShiftedMipDimension(height, mip);
}

uint32_t HostImage::GetMipRowStride(uint32_t mip) const
{
    const uint32_t mipWidth = GetMipWidth(mip);
    if (mipWidth == 0)
        throw std::invalid_argument("Invalid HostImage dimensions or row stride");

    if (IsBlockCompressed(format))
    {
        // 块行跨度：每 4 个纹素一个 16 字节块；BC 数据必须紧凑，mip0 的
        // rowStrideBytes 只允许为 0 或等于紧凑值。
        const uint32_t blocksPerRow = (mipWidth + TexelBlockSize - 1) / TexelBlockSize;
        const uint32_t tightRowBytes = blocksPerRow * TexelBlockBytes;
        if (mip == 0 && rowStrideBytes != 0 && rowStrideBytes != tightRowBytes)
        {
            throw std::invalid_argument(
                "Block-compressed HostImage requires a tightly packed row stride");
        }
        return tightRowBytes;
    }

    // 非块格式：mip0 允许沿用解码器给的行填充，其余 mip 按紧凑行处理。
    const uint32_t tightRowBytes = mipWidth * BytesPerTexelOrBlock(format);
    if (mip == 0 && rowStrideBytes != 0)
    {
        if (rowStrideBytes < tightRowBytes)
            throw std::invalid_argument("Invalid HostImage dimensions or row stride");
        return rowStrideBytes;
    }
    return tightRowBytes;
}

size_t HostImage::GetMipRowCount(uint32_t mip) const
{
    const uint32_t mipHeight = GetMipHeight(mip);
    if (IsBlockCompressed(format))
        return (mipHeight + TexelBlockSize - 1) / TexelBlockSize;
    return mipHeight;
}

size_t HostImage::GetMipLayerBytes(uint32_t mip) const
{
    return CheckedMultiply(
        GetMipRowStride(mip),
        GetMipRowCount(mip),
        "HostImage mip exceeds CPU storage limit");
}

size_t HostImage::GetLayerBytes() const
{
    if (mipLevels == 0)
        throw std::invalid_argument("HostImage requires at least one mip level");
    size_t bytes = 0;
    for (uint32_t mip = 0; mip < mipLevels; ++mip)
    {
        bytes = CheckedAdd(
            bytes,
            GetMipLayerBytes(mip),
            "HostImage mip chain exceeds CPU storage limit");
    }
    return bytes;
}

size_t HostImage::GetTotalBytes() const
{
    if (arrayLayers == 0)
        throw std::invalid_argument("HostImage requires at least one array layer");
    return CheckedMultiply(
        GetLayerBytes(),
        arrayLayers,
        "HostImage array exceeds CPU storage limit");
}

size_t HostImage::GetMipOffset(uint32_t mip, uint32_t layer) const
{
    if (!IsMipIndexValid(*this, mip))
        throw std::invalid_argument("HostImage mip level is out of range");
    if (layer >= arrayLayers)
        throw std::out_of_range("HostImage array layer is out of range");
    ValidateStorage();

    size_t offset = CheckedMultiply(
        GetLayerBytes(),
        static_cast<size_t>(layer),
        "HostImage array exceeds CPU storage limit");
    for (uint32_t level = 0; level < mip; ++level)
    {
        offset = CheckedAdd(
            offset,
            GetMipLayerBytes(level),
            "HostImage mip chain exceeds CPU storage limit");
    }
    return offset;
}

void HostImage::ValidateStorage() const
{
    if (width == 0 || height == 0)
        throw std::invalid_argument("Invalid HostImage dimensions or row stride");
    if (mipLevels == 0 || mipLevels > 32)
        throw std::invalid_argument("HostImage requires between 1 and 32 mip levels");
    if (arrayLayers == 0)
        throw std::invalid_argument("HostImage requires at least one array layer");

    const size_t totalBytes = GetTotalBytes();
    if (data.size() != totalBytes)
    {
        throw std::invalid_argument(
            "HostImage pixel data does not match its mip/layer layout");
    }
}

void* HostImage::GetMipData(uint32_t mip, uint32_t layer)
{
    return const_cast<void*>(static_cast<const HostImage*>(this)->GetMipData(mip, layer));
}

const void* HostImage::GetMipData(uint32_t mip, uint32_t layer) const
{
    if (!IsMipIndexValid(*this, mip))
        throw std::invalid_argument("HostImage mip level is out of range");
    if (layer >= arrayLayers)
        throw std::out_of_range("HostImage array layer is out of range");
    if (data.empty())
    {
        return nullptr;
    }
    return data.data() + GetMipOffset(mip, layer);
}

HostImage HostImage::CombineArrayLayers(std::vector<HostImage> layers)
{
    if (layers.empty() || layers.size() > std::numeric_limits<uint32_t>::max())
        throw std::invalid_argument("HostImage array requires a non-empty uint32 layer count");

    const HostImage& first = layers.front();
    first.ValidateStorage();
    const size_t layerBytes = first.GetLayerBytes();
    if (layerBytes == 0)
        throw std::invalid_argument("HostImage array requires non-empty layer pixels");
    if (layerBytes > first.data.max_size() / layers.size())
        throw std::length_error("HostImage array exceeds CPU storage limit");

    // 在发布完整数组前校验全部层：mip 链必须逐级同尺寸/同行跨度，
    // 禁止把嵌套数组或多个不同 mip 链拼成一个数组。
    for (const HostImage& layer : layers)
    {
        if (layer.width != first.width || layer.height != first.height ||
            layer.channels != first.channels || layer.format != first.format ||
            layer.semantic != first.semantic || !SameSwizzle(layer.swizzle, first.swizzle) ||
            layer.arrayLayers != 1 || layer.mipLevels != first.mipLevels)
        {
            throw std::invalid_argument(
                "HostImage array layers must have matching dimensions, format and pixel layout");
        }
        layer.ValidateStorage();
        if (layer.GetLayerBytes() != layerBytes)
            throw std::invalid_argument(
                "HostImage array layers must have matching mip row strides");
        for (uint32_t mip = 0; mip < first.mipLevels; ++mip)
        {
            if (layer.GetMipRowStride(mip) != first.GetMipRowStride(mip))
                throw std::invalid_argument(
                    "HostImage array layers must have matching mip row strides");
        }
    }

    HostImage result = std::move(layers.front());
    result.arrayLayers = static_cast<uint32_t>(layers.size());
    result.data.reserve(layerBytes * layers.size());
    for (size_t layer = 1; layer < layers.size(); ++layer)
        result.data.insert(result.data.end(), layers[layer].data.begin(), layers[layer].data.end());
    return result;
}
