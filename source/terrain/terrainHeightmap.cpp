#include "terrain/terrainHeightmap.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>
#include <stdexcept>
#include <string>

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

float HalfToFloat(uint16_t value)
{
    const float sign = (value & 0x8000u) != 0 ? -1.0f : 1.0f;
    const int exponent = static_cast<int>((value >> 10u) & 0x1fu);
    const int mantissa = static_cast<int>(value & 0x03ffu);
    if (exponent == 0)
        return sign * std::ldexp(static_cast<float>(mantissa), -24);
    if (exponent == 31)
    {
        return mantissa == 0
            ? sign * std::numeric_limits<float>::infinity()
            : std::numeric_limits<float>::quiet_NaN();
    }
    return sign * std::ldexp(static_cast<float>(1024 + mantissa), exponent - 25);
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
    case HostImage::PixelFormat::R16_UNORM:
        // Terrain layer heights keep their native 16-bit precision; truncating
        // to 8 bits here would quantize the CPU sampler differently from the
        // GPU texture array.
        return static_cast<float>(
            reinterpret_cast<const uint16_t*>(row)[x]) / 65535.0f;
    case HostImage::PixelFormat::R16_FLOAT:
        return HalfToFloat(reinterpret_cast<const uint16_t*>(row)[x]);
    case HostImage::PixelFormat::RG16_FLOAT:
        return HalfToFloat(
            reinterpret_cast<const uint16_t*>(row)[static_cast<size_t>(x) * 2]);
    case HostImage::PixelFormat::RGBA16_FLOAT:
        return HalfToFloat(
            reinterpret_cast<const uint16_t*>(row)[static_cast<size_t>(x) * 4]);
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

MeshUvTransform MeshUvTransformFor(
    const TerrainAssetDesc& desc,
    const HostImage& image)
{
    MeshUvTransform transform;
    if (desc.heightmapMargin == 0)
        return transform;
    if (image.width < 2 || image.height < 2)
        throw std::runtime_error("Terrain heightmap requires at least 2x2 pixels");
    const uint32_t expectedWidth =
        desc.gridResolution + 2 * desc.heightmapMargin + 1;
    const uint32_t expectedHeight =
        desc.gridResolution + 2 * desc.heightmapMargin + 1;
    if (image.width != expectedWidth || image.height != expectedHeight)
    {
        throw std::runtime_error(
            "Terrain heightmap with margin requires exactly "
            "gridResolution + 2*heightmapMargin + 1 samples: expected " +
            std::to_string(expectedWidth) + "x" +
            std::to_string(expectedHeight) + ", got " +
            std::to_string(image.width) + "x" +
            std::to_string(image.height));
    }
    const float margin = static_cast<float>(desc.heightmapMargin);
    const float grid = static_cast<float>(desc.gridResolution);
    const float spanU = static_cast<float>(image.width - 1);
    const float spanV = static_cast<float>(image.height - 1);
    transform.offsetU = margin / spanU;
    transform.offsetV = margin / spanV;
    transform.scaleU = grid / spanU;
    transform.scaleV = grid / spanV;
    return transform;
}

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

float ReadNormalizedPixel(const HostImage& image, uint32_t x, uint32_t y)
{
    if (x >= image.width || y >= image.height || image.data.empty())
        throw std::runtime_error("Terrain heightmap pixel is out of bounds");
    return ReadPixel(image, x, y);
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
