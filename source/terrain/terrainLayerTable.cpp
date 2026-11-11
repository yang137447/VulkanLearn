#include "terrain/terrainLayerTable.h"

#include <cmath>
#include <stdexcept>

namespace VL
{

std::vector<TerrainLayerCatalogEntry> TerrainLayerTable::BuildEntries(
    const std::vector<TerrainLayerDesc>& layers)
{
    if (layers.empty() || layers.size() > MaxLayers)
    {
        throw std::runtime_error(
            "M_terrain layer table requires 1..256 layers; got " +
            std::to_string(layers.size()));
    }

    std::vector<TerrainLayerCatalogEntry> entries(MaxLayers);
    uint32_t pageTableBase = 0;
    const uint32_t layerCount = static_cast<uint32_t>(layers.size());
    for (uint32_t layer = 0; layer < layerCount; ++layer)
    {
        const TerrainLayerDesc& source = layers[layer];
        if (!IsSupportedTerrainLayerSourceResolution(source.sourceResolution))
        {
            throw std::runtime_error(
                "Terrain layer sourceResolution must be 512, 1024 or 2048; got " +
                std::to_string(source.sourceResolution));
        }

        TerrainLayerCatalogEntry& entry = entries[layer];
        entry.heightMin = source.heightMin;
        entry.heightMax = source.heightMax;
        entry.slopeMin = source.slopeMin;
        entry.slopeMax = source.slopeMax;
        entry.noise = source.noise;
        entry.blendEdge = DefaultBlendEdge;
        entry.uvScale = source.uvScale;
        entry.uvRotationRadians =
            source.uvRotation * 3.14159265358979323846f / 180.0f;
        // row2 的 pagesPerSide 固定写 mip0；逐 mip 的页数由
        // TerrainLayerPageManager 的静态 helper 推导。
        entry.pagesPerSide = TerrainLayerPagesPerSide(source.sourceResolution, 0);
        entry.pageTableBase = pageTableBase;
        entry.sourceSet = source.sourceSet;
        entry.flags = source.flags;
        entry.avgColor = source.avgColor;
        entry.displacementM = source.displacementM;
        entry.sourceResolution = source.sourceResolution;
        pageTableBase += TerrainLayerTotalPages(source.sourceResolution);
    }
    // 未使用层保持默认构造的全零：shader 若意外读到越界 layerId，
    // 会得到零高度窗口/零 UV 参数，而不是上一条作者数据的残留。
    return entries;
}

HostImage TerrainLayerTable::BuildImage(const std::vector<TerrainLayerDesc>& layers)
{
    const std::vector<TerrainLayerCatalogEntry> entries = BuildEntries(layers);

    HostImage image;
    image.width = MaxLayers;
    image.height = Rows;
    image.channels = 4;
    image.format = HostImage::PixelFormat::RGBA32_FLOAT;
    image.semantic = HostImage::TextureSemantic::Data;
    image.rowStrideBytes = image.width * image.GetPixelSize();
    image.data.resize(static_cast<size_t>(image.rowStrideBytes) * image.height);
    float* pixels = reinterpret_cast<float*>(image.data.data());
    for (uint32_t layer = 0; layer < MaxLayers; ++layer)
    {
        const TerrainLayerCatalogEntry& entry = entries[layer];
        // 行优先：每行 256 个 texel，每个 texel 4 个 float。
        const size_t offset = static_cast<size_t>(layer) * 4;
        const size_t row0 = offset;
        const size_t row1 = static_cast<size_t>(MaxLayers) * 4 + offset;
        const size_t row2 = static_cast<size_t>(MaxLayers) * 8 + offset;
        const size_t row3 = static_cast<size_t>(MaxLayers) * 12 + offset;
        pixels[row0 + 0] = entry.heightMin;
        pixels[row0 + 1] = entry.heightMax;
        pixels[row0 + 2] = entry.slopeMin;
        pixels[row0 + 3] = entry.slopeMax;
        pixels[row1 + 0] = entry.noise;
        pixels[row1 + 1] = entry.blendEdge;
        pixels[row1 + 2] = entry.uvScale;
        pixels[row1 + 3] = entry.uvRotationRadians;
        // 页号/基址都是整数；float32 的 24-bit 尾数足以精确表示 0..87296。
        pixels[row2 + 0] = static_cast<float>(entry.pagesPerSide);
        pixels[row2 + 1] = static_cast<float>(entry.pageTableBase);
        pixels[row2 + 2] = static_cast<float>(entry.sourceSet);
        pixels[row2 + 3] = static_cast<float>(entry.flags);
        pixels[row3 + 0] = entry.avgColor.x();
        pixels[row3 + 1] = entry.avgColor.y();
        pixels[row3 + 2] = entry.avgColor.z();
        pixels[row3 + 3] = entry.displacementM;
    }
    return image;
}

TerrainLayerCatalogEntry TerrainLayerTable::ReadLayer(
    const HostImage& image,
    uint32_t layerId)
{
    if (image.width != MaxLayers || image.height != Rows ||
        image.format != HostImage::PixelFormat::RGBA32_FLOAT)
    {
        throw std::runtime_error(
            "Terrain layer catalog image must be 256x4 RGBA32F");
    }
    if (layerId >= MaxLayers)
        throw std::out_of_range("Terrain layer id must be < 256");

    const float* pixels = reinterpret_cast<const float*>(image.data.data());
    const size_t offset = static_cast<size_t>(layerId) * 4;
    const size_t row0 = offset;
    const size_t row1 = static_cast<size_t>(MaxLayers) * 4 + offset;
    const size_t row2 = static_cast<size_t>(MaxLayers) * 8 + offset;
    const size_t row3 = static_cast<size_t>(MaxLayers) * 12 + offset;

    TerrainLayerCatalogEntry entry;
    entry.heightMin = pixels[row0 + 0];
    entry.heightMax = pixels[row0 + 1];
    entry.slopeMin = pixels[row0 + 2];
    entry.slopeMax = pixels[row0 + 3];
    entry.noise = pixels[row1 + 0];
    entry.blendEdge = pixels[row1 + 1];
    entry.uvScale = pixels[row1 + 2];
    entry.uvRotationRadians = pixels[row1 + 3];
    entry.pagesPerSide = static_cast<uint32_t>(std::lround(pixels[row2 + 0]));
    entry.pageTableBase = static_cast<uint32_t>(std::lround(pixels[row2 + 1]));
    entry.sourceSet = static_cast<uint32_t>(std::lround(pixels[row2 + 2]));
    entry.flags = static_cast<uint32_t>(std::lround(pixels[row2 + 3]));
    entry.avgColor = Eigen::Vector3f(
        pixels[row3 + 0], pixels[row3 + 1], pixels[row3 + 2]);
    entry.displacementM = pixels[row3 + 3];
    // pagesPerSide 是唯一能区分“已使用层”的字段（合法值 4/8/16）。
    entry.sourceResolution = entry.pagesPerSide * TerrainPageResolution;
    return entry;
}

bool TerrainLayerTable::AreLayersIdentical(
    const std::vector<TerrainLayerDesc>& lhs,
    const std::vector<TerrainLayerDesc>& rhs)
{
    if (lhs.size() != rhs.size()) return false;
    for (size_t index = 0; index < lhs.size(); ++index)
    {
        const TerrainLayerDesc& a = lhs[index];
        const TerrainLayerDesc& b = rhs[index];
        if (a.name != b.name ||
            std::abs(a.heightMin - b.heightMin) > 1.0e-6f ||
            std::abs(a.heightMax - b.heightMax) > 1.0e-6f ||
            std::abs(a.slopeMin - b.slopeMin) > 1.0e-6f ||
            std::abs(a.slopeMax - b.slopeMax) > 1.0e-6f ||
            std::abs(a.noise - b.noise) > 1.0e-6f ||
            std::abs(a.uvScale - b.uvScale) > 1.0e-6f ||
            std::abs(a.uvRotation - b.uvRotation) > 1.0e-6f ||
            a.sourceResolution != b.sourceResolution ||
            a.sourceSet != b.sourceSet ||
            a.flags != b.flags ||
            a.hasHeight != b.hasHeight ||
            std::abs(a.displacementM - b.displacementM) > 1.0e-6f ||
            (a.avgColor - b.avgColor).cwiseAbs().maxCoeff() > 1.0e-6f)
            return false;
    }
    return true;
}

} // namespace VL
