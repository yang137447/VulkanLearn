#include "hostImage.h"

#include <limits>
#include <stdexcept>
#include <utility>

namespace
{
    size_t PixelSize(HostImage::PixelFormat format)
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
        default:
            throw std::runtime_error("Unsupported HostImage::PixelFormat");
        }
    }

    size_t LayerSize(const HostImage& image)
    {
        const size_t tightRowBytes = static_cast<size_t>(image.width) * image.GetPixelSize();
        const size_t rowBytes = image.rowStrideBytes == 0 ? tightRowBytes : image.rowStrideBytes;
        if (image.width == 0 || image.height == 0 || rowBytes < tightRowBytes ||
            rowBytes > std::numeric_limits<size_t>::max() / image.height)
        {
            throw std::invalid_argument("Invalid HostImage dimensions or row stride");
        }
        return rowBytes * image.height;
    }

    bool SameSwizzle(const HostImage::ChannelSwizzle& lhs, const HostImage::ChannelSwizzle& rhs)
    {
        return lhs.r == rhs.r && lhs.g == rhs.g && lhs.b == rhs.b && lhs.a == rhs.a;
    }
}

size_t HostImage::GetSize() const
{
    return data.size();
}

size_t HostImage::GetPixelSize() const
{
    return PixelSize(format);
}

void* HostImage::GetMipData(uint32_t mip, uint32_t layer)
{
    return const_cast<void*>(static_cast<const HostImage*>(this)->GetMipData(mip, layer));
}

const void* HostImage::GetMipData(uint32_t mip, uint32_t layer) const
{
    if (mip != 0 || mipLevels != 1)
    {
        throw std::invalid_argument("HostImage stores only mip 0");
    }
    if (layer >= arrayLayers)
        throw std::out_of_range("HostImage array layer is out of range");
    if (data.empty())
    {
        return nullptr;
    }
    const size_t layerBytes = LayerSize(*this);
    if (arrayLayers > data.size() / layerBytes)
        throw std::invalid_argument("HostImage array pixel data is truncated");
    return data.data() + static_cast<size_t>(layer) * layerBytes;
}

HostImage HostImage::CombineArrayLayers(std::vector<HostImage> layers)
{
    if (layers.empty() || layers.size() > std::numeric_limits<uint32_t>::max())
        throw std::invalid_argument("HostImage array requires a non-empty uint32 layer count");

    const HostImage& first = layers.front();
    const size_t layerBytes = LayerSize(first);
    if (layerBytes > first.data.max_size() / layers.size())
        throw std::length_error("HostImage array exceeds CPU storage limit");

    // 在发布完整数组前校验全部层，禁止把嵌套数组或 CPU mip 链误当成一个 layer。
    for (const HostImage& layer : layers)
    {
        if (layer.width != first.width || layer.height != first.height ||
            layer.channels != first.channels || layer.format != first.format ||
            layer.semantic != first.semantic || !SameSwizzle(layer.swizzle, first.swizzle) ||
            layer.arrayLayers != 1 || layer.mipLevels != 1 ||
            LayerSize(layer) != layerBytes || layer.data.size() != layerBytes)
        {
            throw std::invalid_argument(
                "HostImage array layers must have matching dimensions, format and pixel layout");
        }
    }

    HostImage result = std::move(layers.front());
    result.arrayLayers = static_cast<uint32_t>(layers.size());
    result.data.reserve(layerBytes * layers.size());
    for (size_t layer = 1; layer < layers.size(); ++layer)
        result.data.insert(result.data.end(), layers[layer].data.begin(), layers[layer].data.end());
    return result;
}
