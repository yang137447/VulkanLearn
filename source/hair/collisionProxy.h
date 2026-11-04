#pragma once

#include "core/runtimeResult.h"
#include "hair/sdfBuilder.h"

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

#include <Eigen/Core>

namespace VL::Hair
{
// 离线碰撞代理（vertex clustering）。纯 CPU、无 Vulkan 依赖，可在工具与模块测试中直接调用。
//
// 存在理由：TressFX 每帧从一个小型 tfxmesh 重建碰撞 SDF，但本仓库没有 .tfxmesh，
// 渲染身体有 45 万+ 三角形，逐帧体素化不可行。本生成器把真实身体网格压成低多边形
// 代理，保持输入网格的局部空间与单位（本项目女性身体是厘米），输出仍是三角形汤，
// 与 SdfBuilder 的输入类型相同。
//
// 算法（单线程、固定遍历顺序）：
// 1. 可选区域过滤：三个顶点都落在轴对齐盒内（含边界）的三角形才保留。跨边界的
//    三角形整块丢弃，因此裁剪处必然留下开放边，报告如实给出计数，不补洞。
// 2. 顶点聚类：格原点固定在世界原点，cell = floor(p / cellSize)。只聚类被保留三角形
//    引用到的顶点；簇按顶点首次出现顺序编号，中心用 double 累加、除以计数后再收紧到
//    float。固定顺序 + double 累加使相同输入与参数得到逐字节相同的输出。
// 3. 重映射三角形并保留输入绕序：两个或三个顶点落入同一簇、聚类后面积为零的三角形
//    被丢弃；顶点集合相同（忽略顺序和绕序）的重复三角形只保留第一次出现。
// 4. 压缩：按输出三角形首次引用的顺序重排顶点，未被输出三角形使用的簇被删除。
//
// 代理只有在闭合流形上才有可信的内外符号；区域裁剪与聚类都可能产生开放边或非流形边，
// 调用方必须读取报告中的 openEdgeCount / nonManifoldEdgeCount 判断符号可信度。

struct CollisionProxyOptions
{
    // 聚类格边长（厘米）。必须有限且大于 0。
    float cellSize = 1.5f;
    // 可选轴对齐区域，与输入同一局部空间与单位（厘米）。仅当 hasRegion 为 true 时生效。
    bool hasRegion = false;
    Eigen::Vector3f regionMin = Eigen::Vector3f::Zero();
    Eigen::Vector3f regionMax = Eigen::Vector3f::Zero();
    // 输出三角形数下限。低于该值直接失败，不自动放粗格距。
    std::uint32_t minimumTriangleCount = 0;
};

struct CollisionProxyReport
{
    std::uint64_t inputVertexCount = 0;
    std::uint64_t inputTriangleCount = 0;
    // 因区域过滤整块丢弃的三角形数。
    std::uint64_t regionRejectedTriangleCount = 0;
    // 被保留三角形引用到的不同聚类数，包含最终未被任何输出三角形使用的簇。
    std::uint64_t clusterCount = 0;
    std::uint64_t outputVertexCount = 0;
    std::uint64_t outputTriangleCount = 0;
    std::uint64_t droppedDegenerateTriangleCount = 0;
    std::uint64_t droppedDuplicateTriangleCount = 0;
    // 输出网格的拓扑统计，规则与 SdfBuilder 的 CountMeshTopology 相同。
    std::uint64_t openEdgeCount = 0;
    std::uint64_t nonManifoldEdgeCount = 0;
    Eigen::Vector3f inputBoundsMin = Eigen::Vector3f::Zero();
    Eigen::Vector3f inputBoundsMax = Eigen::Vector3f::Zero();
    Eigen::Vector3f outputBoundsMin = Eigen::Vector3f::Zero();
    Eigen::Vector3f outputBoundsMax = Eigen::Vector3f::Zero();
};

struct CollisionProxy
{
    // 与 SdfBuilder 共用的三角形汤类型：positions + indices，每 3 个索引一个三角形。
    SdfTriangleMesh mesh;
    CollisionProxyReport report;
};

// 执行聚类并返回代理与报告。输入为空、索引越界、坐标非有限、格距非法、区域非法、
// 区域拒绝全部三角形、聚类后无三角形、或输出三角形数低于 minimumTriangleCount 都
// 返回 RuntimeResult 失败，不发布部分结果。
RuntimeResult<CollisionProxy> BuildCollisionProxy(
    const SdfTriangleMesh& input, const CollisionProxyOptions& options);

// 确定性 OBJ 文本：只写注释头、"o"、"v"、"f" 行；float 用 9 位有效数字（float 的
// 往返精度），使用 classic locale，不写时间戳。commentLines 每项写成一行 "# <text>"，
// 调用方负责让每项保持单行。
std::string SerializeCollisionProxyObj(const CollisionProxy& proxy,
    const std::string& objectName, const std::vector<std::string>& commentLines);

RuntimeResult<void> WriteCollisionProxyObj(const CollisionProxy& proxy,
    const std::filesystem::path& path, const std::string& objectName,
    const std::vector<std::string>& commentLines);
}
