#include "terrain/terrainWeightMap.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <stdexcept>

#include "terrain/terrainHeightmap.h"

namespace VL
{
namespace
{

constexpr uint32_t TopLayerCount = 4;
constexpr float HeightEdge = 0.05f;
constexpr float SlopeEdge = 6.0f;

float Fract(float value)
{
    return value - std::floor(value);
}

float Hash(float x, float y)
{
    return Fract(std::sin(x * 127.1f + y * 311.7f) * 43758.5453123f);
}

float ValueNoise(float x, float y)
{
    const float cellX = std::floor(x);
    const float cellY = std::floor(y);
    float fractionX = Fract(x);
    float fractionY = Fract(y);
    fractionX = fractionX * fractionX * (3.0f - 2.0f * fractionX);
    fractionY = fractionY * fractionY * (3.0f - 2.0f * fractionY);
    const float a = Hash(cellX, cellY);
    const float b = Hash(cellX + 1.0f, cellY);
    const float c = Hash(cellX, cellY + 1.0f);
    const float d = Hash(cellX + 1.0f, cellY + 1.0f);
    const float top = a + (b - a) * fractionX;
    const float bottom = c + (d - c) * fractionX;
    return top + (bottom - top) * fractionY;
}

float LowerWindow(float value, float minValue, float edge)
{
    if (minValue <= 0.0f) return 1.0f;
    const float t = std::clamp((value - (minValue - edge)) / (2.0f * edge), 0.0f, 1.0f);
    return t * t * (3.0f - 2.0f * t);
}

float UpperWindow(float value, float maxValue, float upperBound, float edge)
{
    if (maxValue >= upperBound) return 1.0f;
    const float t = std::clamp((value - (maxValue - edge)) / (2.0f * edge), 0.0f, 1.0f);
    return 1.0f - t * t * (3.0f - 2.0f * t);
}

struct SampleState
{
    float height = 0.0f;
    float slope = 0.0f;
    Eigen::Vector2f noiseUv = Eigen::Vector2f::Zero();
};

// 局部层号 -> World 全局层号。空映射是恒等映射（单 TR/旧调用点）；
// 非空时必须覆盖每个局部层，否则说明 registry 与 plan 不同步。
uint8_t GlobalLayerId(
    const std::vector<uint8_t>& layerIdMap,
    size_t localLayer,
    size_t localLayerCount)
{
    if (layerIdMap.empty())
        return static_cast<uint8_t>(localLayer);
    if (layerIdMap.size() != localLayerCount)
        throw std::runtime_error(
            "Terrain layer id map must cover every local layer");
    return layerIdMap[localLayer];
}

SampleState SampleAt(
    const TerrainBuildPlan& plan,
    float u,
    float v,
    uint32_t resolution)
{
    const TerrainAssetDesc& desc = plan.desc;
    const HostImage& image = *plan.heightmap;
    const float height = TerrainHeightmap::SampleHeightCm(desc, image, u, v);
    const float du = 1.0f / static_cast<float>(resolution);
    const float dv = du;
    const float left = TerrainHeightmap::SampleHeightCm(desc, image, u - du, v);
    const float right = TerrainHeightmap::SampleHeightCm(desc, image, u + du, v);
    const float down = TerrainHeightmap::SampleHeightCm(desc, image, u, v - dv);
    const float up = TerrainHeightmap::SampleHeightCm(desc, image, u, v + dv);
    const float worldDx = 2.0f * du * desc.size.x();
    const float worldDz = 2.0f * dv * desc.size.y();
    const Eigen::Vector3f normal = Eigen::Vector3f(
        -(right - left) / worldDx,
        1.0f,
        -(up - down) / worldDz).normalized();
    SampleState state;
    // 平地（heightScale≈0）没有可用的垂直尺度：归一化高度固定为 0.5，
    // 让覆盖 [0,1] 的层仍按中间高度参与。负 scale 表示高度翻转，结果仍
    // 钳制到 [0,1]；否则所有层的 heightRange 都可能不命中而静默退化为 layer 0。
    if (std::abs(desc.heightScale) < 1.0e-6f)
        state.height = 0.5f;
    else
        state.height = std::clamp(
            (height - desc.heightOffset) / desc.heightScale,
            0.0f,
            1.0f);
    state.slope = std::acos(std::clamp(normal.y(), -1.0f, 1.0f)) * 180.0f / 3.14159265358979323846f;
    state.noiseUv = Eigen::Vector2f(
        (u - 0.5f) * desc.size.x(),
        (v - 0.5f) * desc.size.y()) * 0.002f;
    return state;
}

float LayerWeight(
    const TerrainLayerDesc& layer,
    uint8_t layerId,
    const SampleState& state)
{
    const float heightWindow =
        LowerWindow(state.height, layer.heightMin, HeightEdge) *
        UpperWindow(state.height, layer.heightMax, 1.0f, HeightEdge);
    const float slopeWindow =
        LowerWindow(state.slope, layer.slopeMin, SlopeEdge) *
        UpperWindow(state.slope, layer.slopeMax, 90.0f, SlopeEdge);
    const float noise = ValueNoise(
        state.noiseUv.x() + static_cast<float>(layerId) * 17.0f,
        state.noiseUv.y() + static_cast<float>(layerId) * 9.0f);
    return std::max(
        0.0f,
        heightWindow * slopeWindow *
            (1.0f + (noise * 2.0f - 1.0f) * layer.noise * 0.5f));
}

void SelectTop4(
    const std::vector<float>& weights,
    const std::vector<uint8_t>& layerIds,
    uint8_t* outIds,
    uint8_t* outWeights)
{
    float topWeights[TopLayerCount] = {0.0f, 0.0f, 0.0f, 0.0f};
    int topIndices[TopLayerCount] = {0, 0, 0, 0};
    for (size_t index = 0; index < weights.size(); ++index)
    {
        const float weight = weights[index];
        for (uint32_t slot = 0; slot < TopLayerCount; ++slot)
        {
            if (weight > topWeights[slot])
            {
                for (uint32_t move = TopLayerCount - 1; move > slot; --move)
                {
                    topWeights[move] = topWeights[move - 1];
                    topIndices[move] = topIndices[move - 1];
                }
                topWeights[slot] = weight;
                topIndices[slot] = static_cast<int>(index);
                break;
            }
        }
    }
    float sum = 0.0f;
    for (float weight : topWeights) sum += weight;
    if (sum < 1.0e-4f)
    {
        outIds[0] = layerIds[0];
        outWeights[0] = 255;
        for (uint32_t slot = 1; slot < TopLayerCount; ++slot)
        {
            outIds[slot] = 0;
            outWeights[slot] = 0;
        }
        return;
    }
    for (uint32_t slot = 0; slot < TopLayerCount; ++slot)
    {
        outIds[slot] = layerIds[static_cast<size_t>(topIndices[slot])];
        outWeights[slot] = static_cast<uint8_t>(std::clamp(
            static_cast<int>(std::round(topWeights[slot] / sum * 255.0f)),
            0,
            255));
    }
}

// tile 候选层由高度/坡度的 16x16 采样范围决定；Build 和 BuildPage 共用同一规则。
TerrainTileCandidateList BuildTileCandidates(
    const TerrainBuildPlan& plan,
    uint32_t resolution,
    uint32_t tilesPerSide,
    uint32_t tileX,
    uint32_t tileZ)
{
    const uint32_t tileResolution = resolution / tilesPerSide;
    float minHeight = 1.0f;
    float maxHeight = 0.0f;
    float minSlope = 90.0f;
    float maxSlope = 0.0f;
    constexpr uint32_t CandidateSamples = 16;
    for (uint32_t sampleZ = 0; sampleZ < CandidateSamples; ++sampleZ)
    {
        for (uint32_t sampleX = 0; sampleX < CandidateSamples; ++sampleX)
        {
            const float u =
                (static_cast<float>(tileX * tileResolution) +
                 (static_cast<float>(sampleX) + 0.5f) *
                     static_cast<float>(tileResolution) / CandidateSamples) /
                static_cast<float>(resolution);
            const float v =
                (static_cast<float>(tileZ * tileResolution) +
                 (static_cast<float>(sampleZ) + 0.5f) *
                     static_cast<float>(tileResolution) / CandidateSamples) /
                static_cast<float>(resolution);
            const SampleState state = SampleAt(plan, u, v, resolution);
            minHeight = std::min(minHeight, state.height);
            maxHeight = std::max(maxHeight, state.height);
            minSlope = std::min(minSlope, state.slope);
            maxSlope = std::max(maxSlope, state.slope);
        }
    }

    TerrainTileCandidateList tile;
    tile.tileX = tileX;
    tile.tileZ = tileZ;
    for (size_t layer = 0; layer < plan.desc.layers.size(); ++layer)
    {
        const TerrainLayerDesc& desc = plan.desc.layers[layer];
        const bool heightOverlaps = desc.heightMax >= minHeight && desc.heightMin <= maxHeight;
        const bool slopeOverlaps = desc.slopeMax >= minSlope && desc.slopeMin <= maxSlope;
        if (heightOverlaps && slopeOverlaps)
            tile.layerIds.push_back(static_cast<uint8_t>(layer));
    }
    if (tile.layerIds.empty())
        tile.layerIds.push_back(0);
    return tile;
}

// 输出缓冲允许不同的行跨度：Build 写整张权重图，BuildPage 只写一个 page。
void BakeTile(
    const TerrainBuildPlan& plan,
    uint32_t resolution,
    uint32_t tilesPerSide,
    uint32_t tileX,
    uint32_t tileZ,
    const TerrainTileCandidateList& tile,
    const std::vector<uint8_t>& layerIdMap,
    uint32_t outputRowStrideTexels,
    uint32_t outputStartX,
    uint32_t outputStartY,
    uint8_t* outIds,
    uint8_t* outWeights)
{
    const uint32_t tileResolution = resolution / tilesPerSide;
    std::vector<float> weights;
    weights.reserve(tile.layerIds.size());
    for (uint32_t y = 0; y < tileResolution; ++y)
    {
        for (uint32_t x = 0; x < tileResolution; ++x)
        {
            const uint32_t globalX = tileX * tileResolution + x;
            const uint32_t globalY = tileZ * tileResolution + y;
            const float u =
                (static_cast<float>(globalX) + 0.5f) / static_cast<float>(resolution);
            const float v =
                (static_cast<float>(globalY) + 0.5f) / static_cast<float>(resolution);
            const SampleState state = SampleAt(plan, u, v, resolution);

            weights.clear();
            for (uint8_t layerId : tile.layerIds)
                weights.push_back(LayerWeight(plan.desc.layers[layerId], layerId, state));
            uint8_t ids[TopLayerCount];
            uint8_t texelWeights[TopLayerCount];
            SelectTop4(weights, tile.layerIds, ids, texelWeights);
            const size_t offset =
                ((static_cast<size_t>(outputStartY + y) * outputRowStrideTexels) +
                 static_cast<size_t>(outputStartX + x)) * 4;
            for (uint32_t slot = 0; slot < TopLayerCount; ++slot)
            {
                // 权重/排序按局部层计算（层参数与噪声种子都是局部语义），
                // 写进 ID 图的是全局层号；空映射时二者相同，旧输出不变。
                outIds[offset + slot] = GlobalLayerId(
                    layerIdMap,
                    ids[slot],
                    plan.desc.layers.size());
                outWeights[offset + slot] = texelWeights[slot];
            }
        }
    }
}

// 暴露给调用方的候选层列表也用全局层号，避免调试工具把局部下标当全局 ID。
void RemapCandidateList(
    TerrainTileCandidateList& tile,
    const std::vector<uint8_t>& layerIdMap,
    size_t localLayerCount)
{
    for (uint8_t& layerId : tile.layerIds)
        layerId = GlobalLayerId(layerIdMap, layerId, localLayerCount);
}

} // namespace

TerrainWeightMap TerrainWeightMapBuilder::Build(
    const TerrainBuildPlan& plan,
    uint32_t resolution,
    uint32_t tilesPerSide,
    const std::vector<uint8_t>& layerIdMap)
{
    if (!plan.heightmap || plan.desc.layers.empty() || plan.desc.layers.size() > 256)
        throw std::runtime_error("Terrain weight map requires a heightmap and 1..256 layers");
    if (resolution == 0 || tilesPerSide == 0 || resolution % tilesPerSide != 0)
        throw std::runtime_error("Terrain weight map resolution must divide by tilesPerSide");

    TerrainWeightMap result;
    result.width = resolution;
    result.height = resolution;
    result.ids.resize(static_cast<size_t>(resolution) * resolution * 4);
    result.weights.resize(static_cast<size_t>(resolution) * resolution * 4);
    const uint32_t tileResolution = resolution / tilesPerSide;
    result.tiles.reserve(static_cast<size_t>(tilesPerSide) * tilesPerSide);

    for (uint32_t tileZ = 0; tileZ < tilesPerSide; ++tileZ)
    {
        for (uint32_t tileX = 0; tileX < tilesPerSide; ++tileX)
        {
            result.tiles.push_back(
                BuildTileCandidates(plan, resolution, tilesPerSide, tileX, tileZ));
        }
    }

    for (uint32_t tileZ = 0; tileZ < tilesPerSide; ++tileZ)
    {
        for (uint32_t tileX = 0; tileX < tilesPerSide; ++tileX)
        {
            const TerrainTileCandidateList& tile =
                result.tiles[static_cast<size_t>(tileZ) * tilesPerSide + tileX];
            BakeTile(
                plan,
                resolution,
                tilesPerSide,
                tileX,
                tileZ,
                tile,
                layerIdMap,
                resolution,
                tileX * tileResolution,
                tileZ * tileResolution,
                result.ids.data(),
                result.weights.data());
        }
    }
    for (TerrainTileCandidateList& tile : result.tiles)
        RemapCandidateList(tile, layerIdMap, plan.desc.layers.size());
    return result;
}

TerrainPageData TerrainWeightMapBuilder::BuildPage(
    const TerrainBuildPlan& plan,
    uint32_t tilesPerSide,
    uint32_t pageResolution,
    uint32_t tileX,
    uint32_t tileZ,
    const std::vector<uint8_t>& layerIdMap)
{
    if (!plan.heightmap || plan.desc.layers.empty() || plan.desc.layers.size() > 256)
        throw std::runtime_error("Terrain page requires a heightmap and 1..256 layers");
    if (tilesPerSide == 0 || pageResolution == 0 || tileX >= tilesPerSide || tileZ >= tilesPerSide)
        throw std::runtime_error("Terrain page tile is outside the terrain");
    const uint64_t resolution64 =
        static_cast<uint64_t>(tilesPerSide) * static_cast<uint64_t>(pageResolution);
    // page 烘焙仍然按“整张地形权重图”的 UV 采样；这个上限只防异常尺寸撑爆内存。
    if (resolution64 > 8192)
        throw std::runtime_error("Terrain page resolution is too large");

    const uint32_t resolution = static_cast<uint32_t>(resolution64);
    TerrainPageData result;
    result.pageResolution = pageResolution;
    result.ids.assign(static_cast<size_t>(pageResolution) * pageResolution * 4, 0);
    result.weights.assign(result.ids.size(), 0);
    result.candidates =
        BuildTileCandidates(plan, resolution, tilesPerSide, tileX, tileZ);
    BakeTile(
        plan,
        resolution,
        tilesPerSide,
        tileX,
        tileZ,
        result.candidates,
        layerIdMap,
        pageResolution,
        0,
        0,
        result.ids.data(),
        result.weights.data());
    RemapCandidateList(
        result.candidates,
        layerIdMap,
        plan.desc.layers.size());
    return result;
}

TerrainVirtualTexture TerrainWeightMapBuilder::BuildVirtualTexture(
    const TerrainBuildPlan& plan,
    uint32_t tilesPerSide,
    uint32_t pageResolution,
    uint32_t pageTableSize,
    const std::vector<uint8_t>& layerIdMap)
{
    if (tilesPerSide == 0 || pageResolution == 0 || tilesPerSide > 32 ||
        pageTableSize == 0 || pageTableSize > 32)
        throw std::runtime_error("Terrain virtual texture requires 1..32 tiles and positive page resolution");
    if (plan.desc.pageOrigin.x() < 0 || plan.desc.pageOrigin.y() < 0 ||
        plan.desc.pageOrigin.x() + static_cast<int>(tilesPerSide) > static_cast<int>(pageTableSize) ||
        plan.desc.pageOrigin.y() + static_cast<int>(tilesPerSide) > static_cast<int>(pageTableSize))
        throw std::runtime_error("Terrain page origin exceeds the global page table");

    TerrainVirtualTexture result;
    result.pageTableSize = pageTableSize;
    result.tilesPerSide = tilesPerSide;
    result.pageResolution = pageResolution;
    result.atlasResolution = pageTableSize * pageResolution;
    result.ids.assign(static_cast<size_t>(result.atlasResolution) * result.atlasResolution * 4, 0);
    result.weights.assign(result.ids.size(), 0);
    result.pageTable.assign(static_cast<size_t>(pageTableSize) * pageTableSize * 4, 0);
    result.tiles.reserve(static_cast<size_t>(tilesPerSide) * tilesPerSide);
    for (uint32_t tileZ = 0; tileZ < tilesPerSide; ++tileZ)
    {
        for (uint32_t tileX = 0; tileX < tilesPerSide; ++tileX)
        {
            // 逐 tile 烘焙与 BuildPage 走同一条路径，保证运行时按需加载和预烘焙结果一致。
            const TerrainPageData page = BuildPage(
                plan, tilesPerSide, pageResolution, tileX, tileZ, layerIdMap);
            result.tiles.push_back(page.candidates);

            const uint32_t globalX = static_cast<uint32_t>(plan.desc.pageOrigin.x()) + tileX;
            const uint32_t globalY = static_cast<uint32_t>(plan.desc.pageOrigin.y()) + tileZ;
            const size_t pageEntry =
                (static_cast<size_t>(globalY) * pageTableSize + globalX) * 4;
            result.pageTable[pageEntry + 0] = static_cast<uint8_t>(globalX);
            result.pageTable[pageEntry + 1] = static_cast<uint8_t>(globalY);
            result.pageTable[pageEntry + 2] = 0;
            result.pageTable[pageEntry + 3] = 255;

            for (uint32_t y = 0; y < pageResolution; ++y)
            {
                const size_t srcOffset = static_cast<size_t>(y) * pageResolution * 4;
                const size_t dstOffset =
                    ((static_cast<size_t>(globalY) * pageResolution + y) * result.atlasResolution +
                     static_cast<size_t>(globalX) * pageResolution) * 4;
                std::memcpy(
                    result.ids.data() + dstOffset,
                    page.ids.data() + srcOffset,
                    static_cast<size_t>(pageResolution) * 4);
                std::memcpy(
                    result.weights.data() + dstOffset,
                    page.weights.data() + srcOffset,
                    static_cast<size_t>(pageResolution) * 4);
            }
        }
    }
    return result;
}

} // namespace VL
