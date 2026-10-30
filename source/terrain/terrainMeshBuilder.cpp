#include "terrain/terrainMeshBuilder.h"

#include <algorithm>
#include <cmath>
#include <stdexcept>

#include "terrain/terrainHeightmap.h"

namespace VL
{

namespace
{

// 把本 chunk 的边界环按顺时针（从 +Y 俯视、引擎的 front face 约定）排好，
// 追加一圈向下平移的副本并连接成裙边。skirt 顶点排在基础顶点之后，
// 基础顶点/索引顺序因此保持不变。
void AppendChunkSkirt(
    TerrainMeshChunk& chunk,
    uint32_t quadsPerChunk,
    uint32_t verticesPerRow,
    float skirtDepthCm)
{
    if (quadsPerChunk == 0)
        return;

    std::vector<uint32_t> ring;
    ring.reserve(static_cast<size_t>(quadsPerChunk) * 4);
    // 底边 (+X)、右边 (+Z)、顶边 (-X)、左边 (-Z)，四条边首尾相接、
    // 每个角点只出现一次。
    for (uint32_t x = 0; x < quadsPerChunk; ++x)
        ring.push_back(x);
    for (uint32_t z = 0; z < quadsPerChunk; ++z)
        ring.push_back(z * verticesPerRow + quadsPerChunk);
    for (uint32_t x = quadsPerChunk; x > 0; --x)
        ring.push_back(quadsPerChunk * verticesPerRow + x);
    for (uint32_t z = quadsPerChunk; z > 0; --z)
        ring.push_back(z * verticesPerRow);

    const uint32_t skirtBaseIndex = static_cast<uint32_t>(chunk.vertices.size());
    for (uint32_t index = 0; index < ring.size(); ++index)
    {
        Vertex skirtVertex = chunk.vertices[ring[index]];
        // 只做竖直下移：沿法线外扩会在陡坡上把裙边推离地形，反而露出缝隙。
        // 法线沿用边界顶点，裙边与地表同色同光照。
        skirtVertex.position.y() -= skirtDepthCm;
        chunk.boundsMin = chunk.boundsMin.cwiseMin(skirtVertex.position);
        chunk.boundsMax = chunk.boundsMax.cwiseMax(skirtVertex.position);
        chunk.vertices.push_back(skirtVertex);
    }

    const uint32_t ringCount = static_cast<uint32_t>(ring.size());
    for (uint32_t index = 0; index < ringCount; ++index)
    {
        const uint32_t next = (index + 1) % ringCount;
        const uint32_t baseCurrent = ring[index];
        const uint32_t baseNext = ring[next];
        const uint32_t skirtCurrent = skirtBaseIndex + index;
        const uint32_t skirtNext = skirtBaseIndex + next;
        // 引擎 front face 是顺时针：这两个三角形的几何法线朝内，
        // 于是从地形外侧看是正面，Back culling 不会把裙边剔掉。
        chunk.indices.push_back(baseCurrent);
        chunk.indices.push_back(skirtCurrent);
        chunk.indices.push_back(skirtNext);
        chunk.indices.push_back(baseCurrent);
        chunk.indices.push_back(skirtNext);
        chunk.indices.push_back(baseNext);
    }
}

} // namespace

float TerrainChunkSkirtDepthCm(const TerrainAssetDesc& desc)
{
    const float scale =
        desc.heightScale < 0.0f ? -desc.heightScale : desc.heightScale;
    const float depth = scale * 0.1f;
    return depth > 1.0f ? depth : 1.0f;
}

std::string TerrainChunkResourceKey(
    const TerrainBuildPlan& plan,
    uint32_t chunkX,
    uint32_t chunkZ,
    uint32_t lodLevel)
{
    if (lodLevel >= TerrainMaxLodLevels)
        throw std::invalid_argument("Terrain chunk resource key lodLevel is out of range");
    const TerrainAssetDesc& desc = plan.desc;
    return "placement=" + TerrainPlacementIdentity(plan) +
        "|asset=" + desc.assetPath +
        "|heightmap=" + desc.heightmapPath +
        "|size=" + std::to_string(desc.size.x()) + "x" + std::to_string(desc.size.y()) +
        "|heightScale=" + std::to_string(desc.heightScale) +
        "|heightOffset=" + std::to_string(desc.heightOffset) +
        "|grid=" + std::to_string(desc.gridResolution) +
        "|chunkQuads=" + std::to_string(desc.chunkQuads) +
        // pageOrigin/pageTilesPerSide 决定 texCoord1，也属于顶点数据输入。
        "|pageOrigin=" + std::to_string(desc.pageOrigin.x()) + "," +
        std::to_string(desc.pageOrigin.y()) +
        "|pageTiles=" + std::to_string(desc.pageTilesPerSide) +
        // lod 决定合并步长与是否带 skirt，必须进入 key；lodDistances
        // 只影响运行时选择，不影响单级网格。LOD0 的 skirt 由 lodLevels>1
        // 决定，也要进入 key，否则单级与多级资产的 LOD0 会错误共享缓冲。
        "|lod=" + std::to_string(lodLevel) +
        "|lodStep=" + std::to_string(TerrainLodStepQuads(lodLevel)) +
        "|lod0Skirt=" +
            (lodLevel == 0 && desc.lodLevels > 1 ? "1" : "0") +
        "|chunk=" + std::to_string(chunkX) + "," + std::to_string(chunkZ);
}

std::vector<TerrainMeshChunk> TerrainMeshBuilder::BuildChunks(
    const TerrainBuildPlan& plan,
    uint32_t lodLevel,
    std::optional<bool> buildSkirtOverride)
{
    if (!plan.heightmap)
        throw std::runtime_error("Terrain mesh build requires a decoded heightmap");
    const TerrainAssetDesc& desc = plan.desc;
    if (desc.gridResolution == 0 || desc.chunkQuads == 0 ||
        desc.gridResolution % desc.chunkQuads != 0)
        throw std::runtime_error("Terrain mesh build requires gridResolution divisible by chunkQuads");
    if (desc.size.x() <= 0.0f || desc.size.y() <= 0.0f)
        throw std::runtime_error("Terrain mesh build requires positive size");
    if (lodLevel >= TerrainMaxLodLevels)
        throw std::runtime_error("Terrain mesh build requires lodLevel < TerrainMaxLodLevels");
    const uint32_t lodStep = TerrainLodStepQuads(lodLevel);
    if (desc.chunkQuads % lodStep != 0)
        throw std::runtime_error("Terrain mesh build requires chunkQuads divisible by the LOD step");

    const uint32_t chunkCount = desc.gridResolution / desc.chunkQuads;
    // 每个 chunk 有 chunkQuads 个 LOD0 格子；第 lodLevel 级把它们按
    // lodStep 合并，得到 quadsPerChunk 个格子。
    const uint32_t quadsPerChunk = desc.chunkQuads / lodStep;
    const uint32_t verticesPerRow = quadsPerChunk + 1;
    const float grid = static_cast<float>(desc.gridResolution);
    // 采样步长跟着 LOD 走；UV 仍是全局 [0,1]，位置与 LOD0 的同名网格点重合。
    const float du = static_cast<float>(lodStep) / grid;
    const float dv = static_cast<float>(lodStep) / grid;
    const float worldDx = 2.0f * du * desc.size.x();
    const float worldDz = 2.0f * dv * desc.size.y();
    const float skirtDepthCm = TerrainChunkSkirtDepthCm(desc);
    const bool buildSkirt =
        buildSkirtOverride.value_or(lodLevel > 0);
    // 全局页 UV 是 [0,1] 的归一化坐标；最后一列/行的顶点恰好落在页边界上，
    // 直接采样会落进邻居页。按半个页 texel 内缩，内部顶点保持精确值。
    const float pageEdgeInset =
        0.5f / (static_cast<float>(TerrainPageResolution) *
            static_cast<float>(TerrainPageTableSize));

    std::vector<TerrainMeshChunk> chunks;
    chunks.reserve(static_cast<size_t>(chunkCount) * chunkCount);
    for (uint32_t chunkZ = 0; chunkZ < chunkCount; ++chunkZ)
    {
        for (uint32_t chunkX = 0; chunkX < chunkCount; ++chunkX)
        {
            TerrainMeshChunk chunk;
            chunk.chunkX = chunkX;
            chunk.chunkZ = chunkZ;
            chunk.lodLevel = lodLevel;
            chunk.vertices.reserve(
                static_cast<size_t>(verticesPerRow) * verticesPerRow +
                (buildSkirt ? static_cast<size_t>(quadsPerChunk) * 4 : 0));
            chunk.indices.reserve(
                static_cast<size_t>(quadsPerChunk) * quadsPerChunk * 6 +
                (buildSkirt ? static_cast<size_t>(quadsPerChunk) * 4 * 6 : 0));

            for (uint32_t localZ = 0; localZ <= quadsPerChunk; ++localZ)
            {
                for (uint32_t localX = 0; localX <= quadsPerChunk; ++localX)
                {
                    const uint32_t globalX =
                        chunkX * desc.chunkQuads + localX * lodStep;
                    const uint32_t globalZ =
                        chunkZ * desc.chunkQuads + localZ * lodStep;
                    const float u = static_cast<float>(globalX) / grid;
                    const float v = static_cast<float>(globalZ) / grid;
                    const float height = TerrainHeightmap::SampleHeightCm(
                        desc, *plan.heightmap, u, v);
                    const float left = TerrainHeightmap::SampleHeightCm(
                        desc, *plan.heightmap, u - du, v);
                    const float right = TerrainHeightmap::SampleHeightCm(
                        desc, *plan.heightmap, u + du, v);
                    const float down = TerrainHeightmap::SampleHeightCm(
                        desc, *plan.heightmap, u, v - dv);
                    const float up = TerrainHeightmap::SampleHeightCm(
                        desc, *plan.heightmap, u, v + dv);

                    // u 对应 +X、v 对应 +Z；梯度法线取 (-dh/dx, 1, -dh/dz)。
                    Eigen::Vector3f normal(
                        -(right - left) / worldDx,
                        1.0f,
                        -(up - down) / worldDz);
                    normal.normalize();

                    Vertex vertex;
                    vertex.position = Eigen::Vector3f(
                        (u - 0.5f) * desc.size.x(),
                        height,
                        (v - 0.5f) * desc.size.y());
                    vertex.normal = normal;
                    vertex.color = Eigen::Vector4f(1.0f, 1.0f, 1.0f, 1.0f);
                    vertex.texCoord = Eigen::Vector2f(u, v);
                    // 第二套 UV 是全局 page-table 坐标；多地形通过各自 pageOrigin 共享同一张页表。
                    Eigen::Vector2f pageUv(
                        (static_cast<float>(desc.pageOrigin.x()) + u * static_cast<float>(desc.pageTilesPerSide)) /
                            static_cast<float>(TerrainPageTableSize),
                        (static_cast<float>(desc.pageOrigin.y()) + v * static_cast<float>(desc.pageTilesPerSide)) /
                            static_cast<float>(TerrainPageTableSize));
                    if (globalX == desc.gridResolution)
                        pageUv.x() -= pageEdgeInset;
                    if (globalZ == desc.gridResolution)
                        pageUv.y() -= pageEdgeInset;
                    vertex.texCoord1 = pageUv;
                    vertex.tangent = Eigen::Vector4f(1.0f, 0.0f, 0.0f, 1.0f);
                    if (chunk.vertices.empty())
                    {
                        chunk.boundsMin = vertex.position;
                        chunk.boundsMax = vertex.position;
                    }
                    else
                    {
                        chunk.boundsMin = chunk.boundsMin.cwiseMin(vertex.position);
                        chunk.boundsMax = chunk.boundsMax.cwiseMax(vertex.position);
                    }
                    chunk.vertices.push_back(vertex);
                }
            }

            for (uint32_t localZ = 0; localZ < quadsPerChunk; ++localZ)
            {
                for (uint32_t localX = 0; localX < quadsPerChunk; ++localX)
                {
                    const uint32_t a = localZ * verticesPerRow + localX;
                    const uint32_t b = a + 1;
                    const uint32_t c = a + verticesPerRow;
                    const uint32_t d = c + 1;
                    // 引擎管线使用 Clockwise front face；从 +Y 上方看，这组索引
                    // 在屏幕空间是顺时针，和现有导入网格的绕序一致。
                    chunk.indices.push_back(a);
                    chunk.indices.push_back(b);
                    chunk.indices.push_back(c);
                    chunk.indices.push_back(b);
                    chunk.indices.push_back(d);
                    chunk.indices.push_back(c);
                }
            }

            // 只有更粗的 LOD 需要裙边；LOD0 保持历史顶点/索引逐字节不变。
            // 相邻 LOD 的高差由更粗一侧的裙边遮挡，因此 LOD0 不需要裙边。
            if (buildSkirt)
                AppendChunkSkirt(chunk, quadsPerChunk, verticesPerRow, skirtDepthCm);

            chunks.push_back(std::move(chunk));
        }
    }
    return chunks;
}

} // namespace VL
