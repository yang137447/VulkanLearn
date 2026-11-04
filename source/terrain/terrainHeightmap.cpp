#include "terrain/terrainHeightmap.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <stdexcept>

namespace VL
{
namespace TerrainHeightmap
{
namespace
{

size_t RowStride(const HostImage& image)
{
    return image.rowStrideBytes != 0
        ? image.rowStrideBytes
        : static_cast<size_t>(image.width) * image.GetPixelSize();
}

float ReadPixel(const HostImage& image, uint32_t x, uint32_t y)
{
    const size_t stride = RowStride(image);
    const uint8_t* row = image.data.data() + static_cast<size_t>(y) * stride;
    switch (image.format)
    {
    case HostImage::PixelFormat::R8_UNORM:
    case HostImage::PixelFormat::R8_SRGB:
        return static_cast<float>(row[x]) / 255.0f;
    case HostImage::PixelFormat::RG8_UNORM:
    case HostImage::PixelFormat::RG8_SRGB:
        return static_cast<float>(row[static_cast<size_t>(x) * 2]) / 255.0f;
    case HostImage::PixelFormat::RGBA8_UNORM:
    case HostImage::PixelFormat::RGBA8_SRGB:
        return static_cast<float>(row[static_cast<size_t>(x) * 4]) / 255.0f;
    case HostImage::PixelFormat::R32_FLOAT:
        return reinterpret_cast<const float*>(row)[x];
    case HostImage::PixelFormat::RGBA32_FLOAT:
        return reinterpret_cast<const float*>(row)[static_cast<size_t>(x) * 4];
    default:
        throw std::runtime_error(
            "Terrain heightmap pixel format is not supported; use 16-bit "
            "grayscale PNG, RGBA EXR/HDR float, or 8-bit grayscale/RGBA");
    }
}

} // namespace

float SampleNormalized(const HostImage& image, float u, float v)
{
    if (image.width < 2 || image.height < 2 || image.data.empty())
    {
        throw std::runtime_error("Terrain heightmap requires at least 2x2 pixels");
    }
    const float clampedU = std::clamp(u, 0.0f, 1.0f);
    const float clampedV = std::clamp(v, 0.0f, 1.0f);
    const float x = clampedU * static_cast<float>(image.width - 1);
    const float y = clampedV * static_cast<float>(image.height - 1);
    const uint32_t x0 = static_cast<uint32_t>(std::floor(x));
    const uint32_t y0 = static_cast<uint32_t>(std::floor(y));
    const uint32_t x1 = std::min(x0 + 1, image.width - 1);
    const uint32_t y1 = std::min(y0 + 1, image.height - 1);
    const float fx = x - static_cast<float>(x0);
    const float fy = y - static_cast<float>(y0);

    const float h00 = ReadPixel(image, x0, y0);
    const float h10 = ReadPixel(image, x1, y0);
    const float h01 = ReadPixel(image, x0, y1);
    const float h11 = ReadPixel(image, x1, y1);
    if (!std::isfinite(h00) || !std::isfinite(h10) ||
        !std::isfinite(h01) || !std::isfinite(h11))
        throw std::runtime_error("Terrain heightmap contains non-finite samples");
    const float top = h00 + (h10 - h00) * fx;
    const float bottom = h01 + (h11 - h01) * fx;
    return top + (bottom - top) * fy;
}

float SampleHeightCm(
    const TerrainAssetDesc& desc,
    const HostImage& image,
    float u,
    float v)
{
    return SampleNormalized(image, u, v) * desc.heightScale + desc.heightOffset;
}

} // namespace TerrainHeightmap
} // namespace VL
