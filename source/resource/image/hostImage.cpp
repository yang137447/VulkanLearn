#include "hostImage.h"

#include <stdexcept>

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
    if (mip != 0 || layer != 0)
    {
        throw std::runtime_error("GetMipData currently supports only mip 0 and layer 0");
    }
    if (data.empty())
    {
        return nullptr;
    }
    return data.data();
}
