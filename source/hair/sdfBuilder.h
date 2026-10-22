#pragma once

#include "core/runtimeResult.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <vector>

// Eigen 的 cross 定义在 Geometry 模块；头文件公开使用 Eigen::Vector3f，调用方
// （测试/工具）也会对向量做叉积，因此这里引入完整定义而不是只引 Core。
#include <Eigen/Geometry>

namespace VL::Hair
{
// 离线有符号距离场（SDF）生成器。纯 CPU、无 Vulkan 依赖，可在工具与模块测试中直接调用。
// 距离单位与输入网格一致（本项目女性身体 OBJ 是厘米），负值在网格内部、正值在外部。
//
// 算法要点：
// 1. 对所有三角形建立 AABB 层次包围盒（BVH），体素查询是 O(log T) 的最近点搜索，
//    不做“三角形数 × 体素总数”的暴力循环；
// 2. 最近点取自最近三角形，符号用 Bærentzen & Aanæs 的角度加权伪法线
//    （face 内部用面法线、边上用相邻面法线之和、顶点用入射面法线按角加权和），
//    不使用单一面法线或射线奇偶性。伪法线在查询时按最近点落在 face/edge/vertex
//    重新取得，因此近边、近顶点不会因为“恰好选到背面三角形”而翻符号；
// 3. 结果按 band 截断：stored = clamp(signedDistance, -band, +band)。读取与采样必须
//    遵守同一规则，见 SampleSdf 的注释；
// 4. 单线程、固定遍历顺序（体素 x→y→z，BVH 中序），同一输入与同一二进制产生
//    逐字节相同的文件，不做与调度相关的归约。
//
// 文件格式（little-endian、无 padding；Windows x64 即本仓库目标平台）：
//   offset 0   char[8]  magic "VLSDF\0\0\0"
//   offset 8   u32      version = 1
//   offset 12  u32      headerBytes = 64
//   offset 16  u32      dims[3]（x, y, z）
//   offset 28  f32      origin[3]（网格最小角，即 voxel(0,0,0) 中心 - 0.5*cellSize）
//   offset 40  f32      cellSize（厘米）
//   offset 44  f32      band（截断距离，厘米）
//   offset 48  u32      payloadFormat = 0（float32）
//   offset 52  u32      payloadBytes = dims.x*dims.y*dims.z*4
//   offset 56  u32      payloadChecksum（payload 字节的 FNV-1a 32）
//   offset 60  u32      reserved = 0
//   offset 64  f32[]    payload：x 变化最快，其次 y，最后 z
// 文件总长必须精确等于 headerBytes + payloadBytes。截断/多余字节、魔数、版本、
// 格式、维度乘积溢出、非有限 header 值、checksum 不符都会让读取失败。
//
// 体素中心：p = origin + cellSize * (index + 0.5)。CLI 会先把网格 AABB 每侧外扩
// band 再建域，因此域外点必然距网格超过 band；SampleSdf 对域外返回 +band。

// 三角形汤。indices 每 3 个一组，顶点索引必须可表示为 uint32。
struct SdfTriangleMesh
{
    std::vector<Eigen::Vector3f> positions;
    std::vector<std::uint32_t> indices;
};

// 体素域。minCorner 是最小角，dims 是每轴体素数，band 是截断半径（厘米）。
// 三者与 cellSize 一起决定采样规则，必须由调用方显式给出，生成器不猜网格范围。
// threadCount：0/1 表示单线程；>1 时按 z 切片分给多个 worker，每个体素只由一个
// worker 计算并只写自己的槽位，因此输出与线程数/调度无关，逐字节确定。
struct SdfGridSpec
{
    Eigen::Vector3f minCorner = Eigen::Vector3f::Zero();
    float cellSize = 1.0f;
    std::array<std::uint32_t, 3> dims{0, 0, 0};
    float band = 4.0f;
    std::uint32_t threadCount = 0;
};

// 输入网格的水密性报告，供调用方判断符号是否可信。
// openEdge：只被一个三角形使用的无向边（网格边界/孔洞）。
// nonManifoldEdge：被三个及以上三角形共享的边，伪法线在这些边上没有唯一法锥。
struct SdfMeshReport
{
    std::uint64_t vertexCount = 0;
    std::uint64_t triangleCount = 0;
    std::uint64_t openEdgeCount = 0;
    std::uint64_t nonManifoldEdgeCount = 0;
    std::uint64_t degenerateTriangleCount = 0;
};

struct SdfField
{
    std::array<std::uint32_t, 3> dims{0, 0, 0};
    Eigen::Vector3f origin = Eigen::Vector3f::Zero();
    float cellSize = 0.0f;
    float band = 0.0f;
    // 与 dims 同序：x 最快、z 最慢；值已截断到 [-band, +band]。
    // 注意：从 .sdf 读回的字段没有输入网格拓扑统计（文件格式不保存报告），
    // mesh 各计数为 0；只有 BuildSdf 返回的字段带真实报告。
    std::vector<float> distances;
    SdfMeshReport mesh;

    std::size_t VoxelIndex(std::uint32_t x, std::uint32_t y, std::uint32_t z) const
    {
        return static_cast<std::size_t>(x) +
            static_cast<std::size_t>(dims[0]) *
                (static_cast<std::size_t>(y) + static_cast<std::size_t>(dims[1]) * z);
    }

    Eigen::Vector3f VoxelCenter(std::uint32_t x, std::uint32_t y, std::uint32_t z) const
    {
        return origin + cellSize * Eigen::Vector3f(
            static_cast<float>(x) + 0.5f,
            static_cast<float>(y) + 0.5f,
            static_cast<float>(z) + 0.5f);
    }

    float Distance(std::uint32_t x, std::uint32_t y, std::uint32_t z) const
    {
        return distances[VoxelIndex(x, y, z)];
    }
};

inline constexpr std::uint32_t SdfFileVersion = 1;
inline constexpr std::size_t SdfFileHeaderBytes = 64;
inline constexpr std::array<char, 8> SdfFileMagic{'V', 'L', 'S', 'D', 'F', '\0', '\0', '\0'};
// 与 CLI 默认 --max-voxels 无关的库级硬上限，只用于拒绝明显溢出/不可能分配的值。
inline constexpr std::uint64_t SdfMaxVoxelCount = 256ull * 1024ull * 1024ull;

// 构建 SDF。失败（空网格、越界索引、非有限坐标、退化域、体素数超上限）
// 返回 RuntimeResult 错误，不发布部分结果。
RuntimeResult<SdfField> BuildSdf(const SdfTriangleMesh& mesh, const SdfGridSpec& grid);

// 三线性采样。域外返回 +band（调用方必须按 band 外扩域，CLI 即如此）。
// 只有 8 个角点都未被截断时插值才等于真实距离；跨 band 边界时插值是
// 截断场的插值，会低估真实距离，这是截断存储的已知代价。
float SampleSdf(const SdfField& field, const Eigen::Vector3f& position);

// 序列化为完整文件字节（header + payload）。用于测试与确定性比较。
std::vector<std::uint8_t> SerializeSdf(const SdfField& field);
RuntimeResult<SdfField> ParseSdf(
    const std::vector<std::uint8_t>& bytes, const std::string& sourcePath = {});
RuntimeResult<void> WriteSdfFile(const SdfField& field, const std::filesystem::path& path);
RuntimeResult<SdfField> LoadSdfFile(const std::filesystem::path& path);
}
