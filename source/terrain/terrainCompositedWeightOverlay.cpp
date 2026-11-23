#include "terrain/terrainCompositedWeightOverlay.h"
#include "terrain/terrainHeightmap.h"

#include <cmath>
#include <cstring>
#include <set>
#include <stdexcept>

namespace VL
{
namespace
{
void Require(bool condition, const char* message)
{
    if (!condition)
        throw std::runtime_error(std::string("Terrain composited weight overlay: ") + message);
}

float ReadWeight(const HostImage& image, uint32_t layer, uint32_t pixelX, uint32_t pixelY)
{
    const auto* pixel = static_cast<const uint8_t*>(image.GetMipData(0, layer)) +
        size_t(pixelY) * image.GetMipRowStride(0) + size_t(pixelX) * image.GetPixelSize();
    switch (image.format)
    {
    case HostImage::PixelFormat::R8_UNORM:
    case HostImage::PixelFormat::RG8_UNORM:
    case HostImage::PixelFormat::RGBA8_UNORM:
        return pixel[0] / 255.f;
    case HostImage::PixelFormat::R16_UNORM:
    {
        uint16_t value;
        std::memcpy(&value, pixel, sizeof(value));
        return value / 65535.f;
    }
    case HostImage::PixelFormat::R32_FLOAT:
    case HostImage::PixelFormat::RGBA32_FLOAT:
    {
        float value;
        std::memcpy(&value, pixel, sizeof(value));
        return value;
    }
    default:
        throw std::runtime_error("Terrain composited weight overlay: unsupported linear weight format");
    }
}
}

HostImage BuildTerrainCompositedWeightOverlay(
    const TerrainBuildPlan& plan,
    const HostImage& importedWeights,
    const std::vector<uint8_t>& localToGlobal)
{
    const auto layerCount = plan.desc.layers.size();
    Require(layerCount > 0 && layerCount <= 256 && localToGlobal.size() == layerCount &&
        std::set<uint8_t>(localToGlobal.begin(), localToGlobal.end()).size() == layerCount,
        "invalid local/global layer mapping");
    Require(plan.desc.heightmapMargin == 1, "local weight UV requires heightmapMargin=1");
    Require(importedWeights.width >= 4 && importedWeights.width <= 4096 &&
        importedWeights.height == importedWeights.width && importedWeights.mipLevels == 1 &&
        importedWeights.arrayLayers == layerCount, "invalid weight array dimensions/layers/mips");
    Require(importedWeights.swizzle.r == 0, "weight red-channel swizzle is unsupported");
    Require(uint64_t(importedWeights.width) * importedWeights.height * layerCount * sizeof(float) <=
        64ull * 1024 * 1024, "weight overlay exceeds 64 MiB workspace");
    Require(importedWeights.format == HostImage::PixelFormat::R8_UNORM ||
        importedWeights.format == HostImage::PixelFormat::RG8_UNORM ||
        importedWeights.format == HostImage::PixelFormat::RGBA8_UNORM ||
        importedWeights.format == HostImage::PixelFormat::R16_UNORM ||
        importedWeights.format == HostImage::PixelFormat::R32_FLOAT ||
        importedWeights.format == HostImage::PixelFormat::RGBA32_FLOAT,
        "unsupported linear weight format");
    importedWeights.ValidateStorage();
    Require(!importedWeights.data.empty() &&
        importedWeights.data.size() == importedWeights.GetTotalBytes(), "incomplete weight storage");
    const auto* paint = plan.layerPaint.get();
    TerrainHeightmap::MeshUvTransform paintUvTransform;
    if (paint)
    {
        Require(!paint->Empty() && paint->layerCount == layerCount && paint->width == paint->height &&
            paint->width >= 2 && paint->width <= 4096, "invalid Paint snapshot storage/dimensions");
        for (const auto weight : paint->weights)
            Require(std::isfinite(weight) && weight >= 0 && weight <= 1, "invalid Paint weight");
        Require(bool(plan.heightmap), "Paint UV requires the mesh heightmap");
        paintUvTransform = TerrainHeightmap::MeshUvTransformFor(plan.desc, *plan.heightmap);
    }
    HostImage result;
    result.width = importedWeights.width;
    result.height = importedWeights.height;
    result.arrayLayers = importedWeights.arrayLayers;
    result.channels = 1;
    result.format = HostImage::PixelFormat::R32_FLOAT;
    result.semantic = HostImage::TextureSemantic::Data;
    result.data.resize(size_t(result.width) * result.height * result.arrayLayers * sizeof(float));
    for (uint32_t layer = 0; layer < result.arrayLayers; ++layer)
        for (uint32_t pixelY = 0; pixelY < result.height; ++pixelY)
            for (uint32_t pixelX = 0; pixelX < result.width; ++pixelX)
            {
                const float base = ReadWeight(importedWeights, layer, pixelX, pixelY);
                Require(std::isfinite(base) && base >= 0 && base <= 1, "invalid imported weight");
                const auto surfaceUv = [&](uint32_t pixel)
                {
                    const float span = float(result.width - 1);
                    return ((float(pixel) + 0.5f) / float(result.width) * span - 1.f) / (span - 2.f);
                };
                const auto paintUv = paintUvTransform.Apply(surfaceUv(pixelX), surfaceUv(pixelY));
                const float weight = paint
                    ? paint->Blend(layer, paintUv.x(), paintUv.y(), base) : base;
                const size_t offset = ((size_t(layer) * result.height + pixelY) * result.width + pixelX) *
                    sizeof(float);
                std::memcpy(result.data.data() + offset, &weight, sizeof(weight));
            }
    return result;
}
}
