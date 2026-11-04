#include "terrain/terrainMeshBuilder.h"

#include <algorithm>
#include <cmath>
#include <stdexcept>

#include "terrain/terrainHeightmap.h"

namespace VL
{

namespace
{

} // namespace

bool TerrainChunkBuildFilter::Accepts(
    uint32_t chunkX,
    uint32_t chunkZ,
    uint32_t chunksPerSide) const
{
    if (chunkMask.empty())
        return true;
    const size_t index =
        static_cast<size_t>(chunkZ) * chunksPerSide + chunkX;
    // 长度不匹配属于调用方错误：宁可整块不生成，也不要越界读 mask。
    return index < chunkMask.size() && chunkMask[index] != 0;
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
        // lod 决定合并步长与是否带边界缝合带，必须进入 key；lodDistances
        // 只影响运行时选择，不影响单级网格。LOD0 的几何与 lodLevels 无关，
        // 因此单级与多级资产的 LOD0 可以共享同一个缓冲。
        "|lod=" + std::to_string(lodLevel) +
        "|lodStep=" + std::to_string(TerrainLodStepQuads(lodLevel)) +
        "|chunk=" + std::to_string(chunkX) + "," + std::to_string(chunkZ);
}

std::vector<TerrainMeshChunk> TerrainMeshBuilder::BuildChunks(
    const TerrainBuildPlan& plan,
    uint32_t lodLevel,
    const TerrainChunkBuildFilter* filter)
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
            // 加载期驻留窗口：窗口外的细 LOD 连 CPU 网格都不生成。
            // 顺序（chunkZ 外层）必须与 ComputeChunkBounds 完全一致。
            if (filter != nullptr &&
                !filter->Accepts(chunkX, chunkZ, chunkCount))
            {
                continue;
            }

            TerrainMeshChunk chunk;
            chunk.chunkX = chunkX;
            chunk.chunkZ = chunkZ;
            chunk.lodLevel = lodLevel;
            // LOD0 走历史路径（顶点/索引逐字节不变）；更粗的 LOD 追加一圈按
            // LOD0 间距采样的边界环，用它和粗网格之间的一圈过渡三角形带补缝。
            const uint32_t fineEdgeVertices =
                lodStep > 1 ? desc.chunkQuads * 4u : 0u;
            chunk.vertices.reserve(
                static_cast<size_t>(verticesPerRow) * verticesPerRow +
                fineEdgeVertices);
            chunk.indices.reserve(
                static_cast<size_t>(quadsPerChunk) * quadsPerChunk * 6 +
                static_cast<size_t>(lodStep > 1 ? desc.chunkQuads * 4 * 2 : 0));

            auto appendVertex = [&](uint32_t globalX, uint32_t globalZ,
                                    uint32_t gradientStep) {
                const float u = static_cast<float>(globalX) / grid;
                const float v = static_cast<float>(globalZ) / grid;
                const float stepUv =
                    static_cast<float>(gradientStep) / grid;
                const float stepDx = 2.0f * stepUv * desc.size.x();
                const float stepDz = 2.0f * stepUv * desc.size.y();
                const float height = TerrainHeightmap::SampleHeightCm(
                    desc, *plan.heightmap, u, v);
                const float left = TerrainHeightmap::SampleHeightCm(
                    desc, *plan.heightmap, u - stepUv, v);
                const float right = TerrainHeightmap::SampleHeightCm(
                    desc, *plan.heightmap, u + stepUv, v);
                const float down = TerrainHeightmap::SampleHeightCm(
                    desc, *plan.heightmap, u, v - stepUv);
                const float up = TerrainHeightmap::SampleHeightCm(
                    desc, *plan.heightmap, u, v + stepUv);

                // u 对应 +X、v 对应 +Z；梯度法线取 (-dh/dx, 1, -dh/dz)。
                Eigen::Vector3f normal(
                    -(right - left) / stepDx,
                    1.0f,
                    -(up - down) / stepDz);
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
            };

            for (uint32_t localZ = 0; localZ <= quadsPerChunk; ++localZ)
            {
                for (uint32_t localX = 0; localX <= quadsPerChunk; ++localX)
                {
                    const uint32_t globalX =
                        chunkX * desc.chunkQuads + localX * lodStep;
                    const uint32_t globalZ =
                        chunkZ * desc.chunkQuads + localZ * lodStep;
                    appendVertex(globalX, globalZ, lodStep);
                }
            }

            // 粗 LOD 最外一圈格子不铺常规四边形，改由边界环 + 过渡带填充。
            const uint32_t cellStart = lodStep > 1 ? 1u : 0u;
            const uint32_t cellEnd =
                lodStep > 1 ? (quadsPerChunk > 1 ? quadsPerChunk - 1 : 0u)
                            : quadsPerChunk;
            for (uint32_t localZ = cellStart; localZ < cellEnd; ++localZ)
            {
                for (uint32_t localX = cellStart; localX < cellEnd; ++localX)
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

            // 边界缝合：四条边一律按 LOD0 间距采样，再与第一条粗网格线之间
            // 铺一圈过渡三角形。因为任何级别的 chunk 边界都落在同一组 LOD0
            // 采样点上，相邻 chunk 无论 LOD 差几级都是水密的，不需要运行时
            // 知道邻居级别，也不需要 shader 里 morph。
            if (lodStep > 1)
            {
                const uint32_t edge = desc.chunkQuads;
                const uint32_t baseX = chunkX * edge;
                const uint32_t baseZ = chunkZ * edge;
                const uint32_t ringBase =
                    static_cast<uint32_t>(chunk.vertices.size());
                for (uint32_t j = 0; j < edge; ++j)
                    appendVertex(baseX + j, baseZ, 1);
                for (uint32_t j = 0; j < edge; ++j)
                    appendVertex(baseX + edge, baseZ + j, 1);
                for (uint32_t j = 0; j < edge; ++j)
                    appendVertex(baseX + edge - j, baseZ + edge, 1);
                for (uint32_t j = 0; j < edge; ++j)
                    appendVertex(baseX, baseZ + edge - j, 1);

                // 角点归下一条边所有，j == edge 时跳到下一条边的起点。
                auto fineIndex = [&](uint32_t side, uint32_t j) -> uint32_t {
                    return j >= edge ? ringBase + ((side + 1) % 4) * edge
                                     : ringBase + side * edge + j;
                };
                // 每条边的第一条粗网格线，索引方向与边界环一致。
                auto innerIndex = [&](uint32_t side, uint32_t k) -> uint32_t {
                    switch (side)
                    {
                    case 0:
                        return verticesPerRow + k;
                    case 1:
                        return k * verticesPerRow + (quadsPerChunk - 1);
                    case 2:
                        return (quadsPerChunk - 1) * verticesPerRow +
                               (quadsPerChunk - k);
                    default:
                        return (quadsPerChunk - k) * verticesPerRow + 1;
                    }
                };

                for (uint32_t side = 0; side < 4; ++side)
                {
                    for (uint32_t k = 0; k < quadsPerChunk; ++k)
                    {
                        const uint32_t first = k * lodStep;
                        const uint32_t last = first + lodStep;
                        const uint32_t innerFirst = innerIndex(side, k);
                        const uint32_t innerLast = innerIndex(side, k + 1);
                        for (uint32_t j = first; j < last; ++j)
                        {
                            chunk.indices.push_back(fineIndex(side, j));
                            chunk.indices.push_back(fineIndex(side, j + 1));
                            chunk.indices.push_back(innerFirst);
                        }
                        chunk.indices.push_back(fineIndex(side, last));
                        chunk.indices.push_back(innerLast);
                        chunk.indices.push_back(innerFirst);
                    }
                }
            }

            chunks.push_back(std::move(chunk));
        }
    }
    return chunks;
}

std::vector<TerrainChunkBounds> TerrainMeshBuilder::ComputeChunkBounds(
    const TerrainBuildPlan& plan)
{
    if (!plan.heightmap)
        throw std::runtime_error("Terrain chunk bounds require a decoded heightmap");
    const TerrainAssetDesc& desc = plan.desc;
    if (desc.gridResolution == 0 || desc.chunkQuads == 0 ||
        desc.gridResolution % desc.chunkQuads != 0)
        throw std::runtime_error(
            "Terrain chunk bounds require gridResolution divisible by chunkQuads");
    if (desc.size.x() <= 0.0f || desc.size.y() <= 0.0f)
        throw std::runtime_error("Terrain chunk bounds require positive size");

    const uint32_t chunkCount = desc.gridResolution / desc.chunkQuads;
    const float grid = static_cast<float>(desc.gridResolution);

    std::vector<TerrainChunkBounds> bounds;
    bounds.reserve(static_cast<size_t>(chunkCount) * chunkCount);
    for (uint32_t chunkZ = 0; chunkZ < chunkCount; ++chunkZ)
    {
        for (uint32_t chunkX = 0; chunkX < chunkCount; ++chunkX)
        {
            TerrainChunkBounds chunk;
            chunk.chunkX = chunkX;
            chunk.chunkZ = chunkZ;
            bool first = true;
            // 只需要高度范围与水平范围：不算法线，也不写顶点/索引。
            // LOD0 的顶点位置由同一 u/v 网格采样，因此这里与 BuildChunks(0)
            // 的 AABB 完全一致（更粗一级的边界缝合带与地表同高，不改变范围）。
            // 包络，不参与 LOD 组距离）。
            for (uint32_t localZ = 0; localZ <= desc.chunkQuads; ++localZ)
            {
                for (uint32_t localX = 0; localX <= desc.chunkQuads; ++localX)
                {
                    const uint32_t globalX =
                        chunkX * desc.chunkQuads + localX;
                    const uint32_t globalZ =
                        chunkZ * desc.chunkQuads + localZ;
                    const float u = static_cast<float>(globalX) / grid;
                    const float v = static_cast<float>(globalZ) / grid;
                    const Eigen::Vector3f position(
                        (u - 0.5f) * desc.size.x(),
                        TerrainHeightmap::SampleHeightCm(
                            desc, *plan.heightmap, u, v),
                        (v - 0.5f) * desc.size.y());
                    if (first)
                    {
                        chunk.boundsMin = position;
                        chunk.boundsMax = position;
                        first = false;
                    }
                    else
                    {
                        chunk.boundsMin = chunk.boundsMin.cwiseMin(position);
                        chunk.boundsMax = chunk.boundsMax.cwiseMax(position);
                    }
                }
            }
            bounds.push_back(std::move(chunk));
        }
    }
    return bounds;
}

} // namespace VL
