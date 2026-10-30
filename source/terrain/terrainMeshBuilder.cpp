#include "terrain/terrainMeshBuilder.h"

#include <algorithm>
#include <cmath>
#include <stdexcept>

#include "terrain/terrainHeightmap.h"

namespace VL
{

std::string TerrainChunkResourceKey(
    const TerrainBuildPlan& plan,
    uint32_t chunkX,
    uint32_t chunkZ)
{
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
        "|chunk=" + std::to_string(chunkX) + "," + std::to_string(chunkZ);
}

std::vector<TerrainMeshChunk> TerrainMeshBuilder::BuildChunks(const TerrainBuildPlan& plan)
{
    if (!plan.heightmap)
        throw std::runtime_error("Terrain mesh build requires a decoded heightmap");
    const TerrainAssetDesc& desc = plan.desc;
    if (desc.gridResolution == 0 || desc.chunkQuads == 0 ||
        desc.gridResolution % desc.chunkQuads != 0)
        throw std::runtime_error("Terrain mesh build requires gridResolution divisible by chunkQuads");
    if (desc.size.x() <= 0.0f || desc.size.y() <= 0.0f)
        throw std::runtime_error("Terrain mesh build requires positive size");

    const uint32_t chunkCount = desc.gridResolution / desc.chunkQuads;
    const uint32_t verticesPerRow = desc.chunkQuads + 1;
    const float grid = static_cast<float>(desc.gridResolution);
    const float du = 1.0f / grid;
    const float dv = 1.0f / grid;
    const float worldDx = 2.0f * du * desc.size.x();
    const float worldDz = 2.0f * dv * desc.size.y();
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
            chunk.vertices.reserve(static_cast<size_t>(verticesPerRow) * verticesPerRow);
            chunk.indices.reserve(static_cast<size_t>(desc.chunkQuads) * desc.chunkQuads * 6);

            for (uint32_t localZ = 0; localZ <= desc.chunkQuads; ++localZ)
            {
                for (uint32_t localX = 0; localX <= desc.chunkQuads; ++localX)
                {
                    const uint32_t globalX = chunkX * desc.chunkQuads + localX;
                    const uint32_t globalZ = chunkZ * desc.chunkQuads + localZ;
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

            for (uint32_t localZ = 0; localZ < desc.chunkQuads; ++localZ)
            {
                for (uint32_t localX = 0; localX < desc.chunkQuads; ++localX)
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
            chunks.push_back(std::move(chunk));
        }
    }
    return chunks;
}

} // namespace VL
