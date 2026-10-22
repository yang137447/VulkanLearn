#include "hair/sdfBuilder.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <fstream>
#include <functional>
#include <limits>
#include <new>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace VL::Hair
{
namespace
{
// 面积（厘米²）低于该值的三角形视为退化：没有可用法线，也不进入 BVH 与伪法线邻接。
// 这些三角形仍计入拓扑报告的三角形总数，因此水密性统计不会丢失输入面。
constexpr float kDegenerateNormalLength = 1.0e-12f;
// 重心坐标小于该值就认为落在边/顶点特征上。Ericson 的 clamp 会写出精确 0，
// 这里留一点余量避免浮点噪声把边上的点误判为面内部。
constexpr float kFeatureEpsilon = 1.0e-6f;
// 退化伪法线判定阈值（归一化向量的平方长度）。
constexpr float kDegenerateNormalSquared = 1.0e-20f;
constexpr std::uint32_t kBvhLeafSize = 8;

struct Triangle
{
    Eigen::Vector3f a;
    Eigen::Vector3f b;
    Eigen::Vector3f c;
    Eigen::Vector3f normal; // 单位面法线
    std::uint32_t i0 = 0;
    std::uint32_t i1 = 0;
    std::uint32_t i2 = 0;
    // 三个角的内角，供角度加权伪法线使用。
    float angle0 = 0.0f;
    float angle1 = 0.0f;
    float angle2 = 0.0f;
};

struct BvhNode
{
    Eigen::Vector3f min;
    Eigen::Vector3f max;
    std::uint32_t first = 0;
    std::uint32_t count = 0; // > 0 为叶子；否则 right 是右子节点索引，左子节点是 this+1
    std::uint32_t right = 0;
};

struct EdgeNormal
{
    std::uint64_t key = 0;
    Eigen::Vector3f normal = Eigen::Vector3f::Zero();
};

struct ClosestPoint
{
    float squaredDistance = std::numeric_limits<float>::max();
    std::uint32_t triangle = 0;
    Eigen::Vector3f barycentric = Eigen::Vector3f(1.0f, 0.0f, 0.0f);
};

std::uint64_t EdgeKey(std::uint32_t i, std::uint32_t j)
{
    const std::uint32_t low = std::min(i, j);
    const std::uint32_t high = std::max(i, j);
    return (static_cast<std::uint64_t>(low) << 32) | static_cast<std::uint64_t>(high);
}

float CornerAngle(const Eigen::Vector3f& corner, const Eigen::Vector3f& first,
    const Eigen::Vector3f& second)
{
    const Eigen::Vector3f u = first - corner;
    const Eigen::Vector3f v = second - corner;
    const float uu = u.squaredNorm();
    const float vv = v.squaredNorm();
    if (!(uu > 0.0f) || !(vv > 0.0f))
        return 0.0f;
    const float cosine = u.dot(v) / std::sqrt(uu * vv);
    return std::acos(std::max(-1.0f, std::min(1.0f, cosine)));
}

// Ericson《Real-Time Collision Detection》5.1.5 的区域判定：最近点必在面内部、
// 某条边上或某个顶点上，返回值同时给出重心坐标，后续据此选择伪法线特征。
float ClosestPointOnTriangle(const Triangle& tri, const Eigen::Vector3f& p,
    Eigen::Vector3f& barycentric)
{
    const Eigen::Vector3f& a = tri.a;
    const Eigen::Vector3f& b = tri.b;
    const Eigen::Vector3f& c = tri.c;
    const Eigen::Vector3f ab = b - a;
    const Eigen::Vector3f ac = c - a;
    const Eigen::Vector3f ap = p - a;
    const float d1 = ab.dot(ap);
    const float d2 = ac.dot(ap);
    if (d1 <= 0.0f && d2 <= 0.0f)
    {
        barycentric = Eigen::Vector3f(1.0f, 0.0f, 0.0f);
        return (p - a).squaredNorm();
    }

    const Eigen::Vector3f bp = p - b;
    const float d3 = ab.dot(bp);
    const float d4 = ac.dot(bp);
    if (d3 >= 0.0f && d4 <= d3)
    {
        barycentric = Eigen::Vector3f(0.0f, 1.0f, 0.0f);
        return (p - b).squaredNorm();
    }

    const float vc = d1 * d4 - d3 * d2;
    if (vc <= 0.0f && d1 >= 0.0f && d3 <= 0.0f)
    {
        const float v = d1 / (d1 - d3);
        barycentric = Eigen::Vector3f(1.0f - v, v, 0.0f);
        return (p - (a + ab * v)).squaredNorm();
    }

    const Eigen::Vector3f cp = p - c;
    const float d5 = ab.dot(cp);
    const float d6 = ac.dot(cp);
    if (d6 >= 0.0f && d5 <= d6)
    {
        barycentric = Eigen::Vector3f(0.0f, 0.0f, 1.0f);
        return (p - c).squaredNorm();
    }

    const float vb = d5 * d2 - d1 * d6;
    if (vb <= 0.0f && d2 >= 0.0f && d6 <= 0.0f)
    {
        const float w = d2 / (d2 - d6);
        barycentric = Eigen::Vector3f(1.0f - w, 0.0f, w);
        return (p - (a + ac * w)).squaredNorm();
    }

    const float va = d3 * d6 - d5 * d4;
    if (va <= 0.0f && (d4 - d3) >= 0.0f && (d5 - d6) >= 0.0f)
    {
        const float w = (d4 - d3) / ((d4 - d3) + (d5 - d6));
        barycentric = Eigen::Vector3f(0.0f, 1.0f - w, w);
        return (p - (b + (c - b) * w)).squaredNorm();
    }

    const float denominator = 1.0f / (va + vb + vc);
    const float v = vb * denominator;
    const float w = vc * denominator;
    barycentric = Eigen::Vector3f(1.0f - v - w, v, w);
    return (p - (a + ab * v + ac * w)).squaredNorm();
}

float AabbDistanceSquared(const Eigen::Vector3f& min, const Eigen::Vector3f& max,
    const Eigen::Vector3f& p)
{
    const float dx = std::max({min.x() - p.x(), 0.0f, p.x() - max.x()});
    const float dy = std::max({min.y() - p.y(), 0.0f, p.y() - max.y()});
    const float dz = std::max({min.z() - p.z(), 0.0f, p.z() - max.z()});
    return dx * dx + dy * dy + dz * dz;
}

// 中位数分割的 AABB 树。三角形索引在 order 中重排，节点用扁平数组保存，
// 左子节点固定紧跟父节点，只有右子节点需要显式索引。
class TriangleBvh
{
public:
    void Build(std::vector<Triangle> triangles)
    {
        triangles_ = std::move(triangles);
        order_.resize(triangles_.size());
        for (std::size_t i = 0; i < order_.size(); ++i)
            order_[i] = static_cast<std::uint32_t>(i);
        centroids_.resize(triangles_.size());
        for (std::size_t i = 0; i < triangles_.size(); ++i)
            centroids_[i] = (triangles_[i].a + triangles_[i].b + triangles_[i].c) / 3.0f;
        nodes_.clear();
        nodes_.reserve(triangles_.size() * 2 / kBvhLeafSize + 8);
        BuildNode(0, static_cast<std::uint32_t>(order_.size()));
    }

    const std::vector<Triangle>& Triangles() const { return triangles_; }

    ClosestPoint Query(const Eigen::Vector3f& point, std::vector<std::uint32_t>& stack) const
    {
        ClosestPoint result;
        if (nodes_.empty())
            return result;
        stack.clear();
        stack.push_back(0);
        while (!stack.empty())
        {
            const std::uint32_t nodeIndex = stack.back();
            stack.pop_back();
            const BvhNode& node = nodes_[nodeIndex];
            if (AabbDistanceSquared(node.min, node.max, point) >= result.squaredDistance)
                continue;
            if (node.count > 0)
            {
                for (std::uint32_t offset = 0; offset < node.count; ++offset)
                {
                    const std::uint32_t triangleIndex = order_[node.first + offset];
                    Eigen::Vector3f barycentric;
                    const float squared =
                        ClosestPointOnTriangle(triangles_[triangleIndex], point, barycentric);
                    // 严格小于：与遍历顺序共同构成确定性决胜，不会因调度产生不同最近面。
                    if (squared < result.squaredDistance)
                    {
                        result.squaredDistance = squared;
                        result.triangle = triangleIndex;
                        result.barycentric = barycentric;
                    }
                }
                continue;
            }

            const std::uint32_t left = nodeIndex + 1;
            const std::uint32_t right = node.right;
            const float leftDistance =
                AabbDistanceSquared(nodes_[left].min, nodes_[left].max, point);
            const float rightDistance =
                AabbDistanceSquared(nodes_[right].min, nodes_[right].max, point);
            // 后进先出：把更远的子节点先压栈，让更近的一侧先被访问，剪枝更有效。
            if (leftDistance <= rightDistance)
            {
                stack.push_back(right);
                stack.push_back(left);
            }
            else
            {
                stack.push_back(left);
                stack.push_back(right);
            }
        }
        return result;
    }

private:
    std::uint32_t BuildNode(std::uint32_t begin, std::uint32_t end)
    {
        const std::uint32_t nodeIndex = static_cast<std::uint32_t>(nodes_.size());
        nodes_.push_back(BvhNode{});
        Eigen::Vector3f min = triangles_[order_[begin]].a;
        Eigen::Vector3f max = min;
        Eigen::Vector3f centroidMin = centroids_[order_[begin]];
        Eigen::Vector3f centroidMax = centroidMin;
        for (std::uint32_t i = begin; i < end; ++i)
        {
            const Triangle& tri = triangles_[order_[i]];
            min = min.cwiseMin(tri.a).cwiseMin(tri.b).cwiseMin(tri.c);
            max = max.cwiseMax(tri.a).cwiseMax(tri.b).cwiseMax(tri.c);
            centroidMin = centroidMin.cwiseMin(centroids_[order_[i]]);
            centroidMax = centroidMax.cwiseMax(centroids_[order_[i]]);
        }

        const std::uint32_t count = end - begin;
        const Eigen::Vector3f extent = centroidMax - centroidMin;
        int axis = 0;
        if (extent.y() > extent.x() && extent.y() >= extent.z())
            axis = 1;
        else if (extent.z() > extent.x() && extent.z() > extent.y())
            axis = 2;
        if (count <= kBvhLeafSize || extent[axis] <= 0.0f)
        {
            BvhNode& leaf = nodes_[nodeIndex];
            leaf.min = min;
            leaf.max = max;
            leaf.first = begin;
            leaf.count = count;
            return nodeIndex;
        }

        const std::uint32_t middle = begin + count / 2;
        std::nth_element(order_.begin() + begin, order_.begin() + middle, order_.begin() + end,
            [this, axis](std::uint32_t lhs, std::uint32_t rhs) {
                const float left = centroids_[lhs][axis];
                const float right = centroids_[rhs][axis];
                // 索引决胜保证全序，nth_element 的结果与实现细节无关。
                return left < right || (left == right && lhs < rhs);
            });
        // 先建左子树，保证其根节点就是 nodeIndex + 1。
        BuildNode(begin, middle);
        const std::uint32_t right = BuildNode(middle, end);
        BvhNode& node = nodes_[nodeIndex];
        node.min = min;
        node.max = max;
        node.first = 0;
        node.count = 0;
        node.right = right;
        return nodeIndex;
    }

    std::vector<Triangle> triangles_;
    std::vector<std::uint32_t> order_;
    std::vector<Eigen::Vector3f> centroids_;
    std::vector<BvhNode> nodes_;
};

// 最近点所在特征的角度加权伪法线。face 内部直接用面法线；边上用共享该边的
// 面法线之和（任意正系数都落在边的法锥内，见头文件）；顶点用入射面法线按
// 该顶点处的内角加权求和——这正是 Bærentzen & Aanæs 的规则，也是近顶点处
// 单一面法线最容易翻符号的位置。
Eigen::Vector3f Pseudonormal(const ClosestPoint& closest,
    const std::vector<Triangle>& triangles,
    const std::vector<Eigen::Vector3f>& vertexPseudonormals,
    const std::vector<EdgeNormal>& edgePseudonormals)
{
    const Triangle& tri = triangles[closest.triangle];
    int zeroCount = 0;
    int zeroIndex = -1;
    for (int i = 0; i < 3; ++i)
    {
        if (closest.barycentric[i] <= kFeatureEpsilon)
        {
            ++zeroCount;
            zeroIndex = i;
        }
    }

    if (zeroCount == 0)
        return tri.normal;

    if (zeroCount == 1)
    {
        const std::uint32_t vertices[3] = {tri.i0, tri.i1, tri.i2};
        const std::uint32_t first = vertices[(zeroIndex + 1) % 3];
        const std::uint32_t second = vertices[(zeroIndex + 2) % 3];
        const std::uint64_t key = EdgeKey(first, second);
        const auto found = std::lower_bound(edgePseudonormals.begin(), edgePseudonormals.end(), key,
            [](const EdgeNormal& edge, std::uint64_t value) { return edge.key < value; });
        if (found != edgePseudonormals.end() && found->key == key &&
            found->normal.squaredNorm() > kDegenerateNormalSquared)
            return found->normal;
        // 非流形边/全退化邻域：退回最近三角形的面法线，仅此一种回退。
        return tri.normal;
    }

    int vertexCorner = 0;
    for (int i = 1; i < 3; ++i)
    {
        if (closest.barycentric[i] > closest.barycentric[vertexCorner])
            vertexCorner = i;
    }
    const std::uint32_t vertex = vertexCorner == 0 ? tri.i0
        : (vertexCorner == 1 ? tri.i1 : tri.i2);
    if (vertexPseudonormals[vertex].squaredNorm() > kDegenerateNormalSquared)
        return vertexPseudonormals[vertex];
    return tri.normal;
}

RuntimeError SdfError(std::string code, std::string message, const std::string& source = {})
{
    return MakeRuntimeError(std::move(code), std::move(message), source);
}

void WriteU32(std::vector<std::uint8_t>& bytes, std::size_t offset, std::uint32_t value)
{
    bytes[offset + 0] = static_cast<std::uint8_t>(value & 0xffu);
    bytes[offset + 1] = static_cast<std::uint8_t>((value >> 8) & 0xffu);
    bytes[offset + 2] = static_cast<std::uint8_t>((value >> 16) & 0xffu);
    bytes[offset + 3] = static_cast<std::uint8_t>((value >> 24) & 0xffu);
}

std::uint32_t ReadU32(const std::vector<std::uint8_t>& bytes, std::size_t offset)
{
    return static_cast<std::uint32_t>(bytes[offset + 0]) |
        (static_cast<std::uint32_t>(bytes[offset + 1]) << 8) |
        (static_cast<std::uint32_t>(bytes[offset + 2]) << 16) |
        (static_cast<std::uint32_t>(bytes[offset + 3]) << 24);
}

void WriteF32(std::vector<std::uint8_t>& bytes, std::size_t offset, float value)
{
    std::uint32_t bits = 0;
    static_assert(sizeof(float) == sizeof(std::uint32_t), "SDF payload requires 32-bit float");
    std::memcpy(&bits, &value, sizeof(bits));
    WriteU32(bytes, offset, bits);
}

float ReadF32(const std::vector<std::uint8_t>& bytes, std::size_t offset)
{
    const std::uint32_t bits = ReadU32(bytes, offset);
    float value = 0.0f;
    std::memcpy(&value, &bits, sizeof(value));
    return value;
}

// payload 的 FNV-1a 32 校验。只覆盖 payload，header 字段自身由结构校验保证。
std::uint32_t PayloadChecksum(const std::vector<std::uint8_t>& bytes, std::size_t offset,
    std::size_t size)
{
    std::uint32_t hash = 2166136261u;
    for (std::size_t i = 0; i < size; ++i)
    {
        hash ^= static_cast<std::uint32_t>(bytes[offset + i]);
        hash *= 16777619u;
    }
    return hash;
}

bool FiniteVector(const Eigen::Vector3f& value)
{
    return std::isfinite(value.x()) && std::isfinite(value.y()) && std::isfinite(value.z());
}

// 一个 worker 的烘焙范围 [zBegin, zEnd)，按与原单线程完全相同的 z→y→x 顺序写入
// field.distances 中属于自己的槽位。BVH、三角形表与伪法线表在这里只读，因此
// 每个体素的结果只取决于它的位置，与线程数、切片划分和调度无关。
void BakeVoxelRange(const TriangleBvh& bvh, const std::vector<Triangle>& triangles,
    const std::vector<Eigen::Vector3f>& vertexPseudonormals,
    const std::vector<EdgeNormal>& edgePseudonormals, SdfField& field,
    std::uint32_t zBegin, std::uint32_t zEnd)
{
    const std::size_t yStride = field.dims[0];
    const std::size_t zStride = static_cast<std::size_t>(field.dims[0]) * field.dims[1];
    std::vector<std::uint32_t> stack;
    stack.reserve(128);
    for (std::uint32_t z = zBegin; z < zEnd; ++z)
    {
        for (std::uint32_t y = 0; y < field.dims[1]; ++y)
        {
            std::size_t voxel = static_cast<std::size_t>(z) * zStride +
                static_cast<std::size_t>(y) * yStride;
            for (std::uint32_t x = 0; x < field.dims[0]; ++x, ++voxel)
            {
                const Eigen::Vector3f center = field.VoxelCenter(x, y, z);
                const ClosestPoint closest = bvh.Query(center, stack);
                float distance = 0.0f;
                if (!(closest.squaredDistance > 0.0f))
                {
                    // 体素中心恰在表面上：距离 0，符号无定义。
                    distance = 0.0f;
                }
                else
                {
                    const Eigen::Vector3f pseudonormal = Pseudonormal(closest, triangles,
                        vertexPseudonormals, edgePseudonormals);
                    const Eigen::Vector3f delta = center -
                        (triangles[closest.triangle].a * closest.barycentric[0] +
                            triangles[closest.triangle].b * closest.barycentric[1] +
                            triangles[closest.triangle].c * closest.barycentric[2]);
                    distance = std::sqrt(closest.squaredDistance);
                    // 角度加权伪法线是符号的唯一判据：朝外为正、朝内为负。
                    // 点无法定向（伪法线退化到长度 0）时按外部处理，并保持有限值。
                    if (delta.dot(pseudonormal) < 0.0f)
                        distance = -distance;
                }
                field.distances[voxel] = std::max(-field.band, std::min(field.band, distance));
            }
        }
    }
}
}

RuntimeResult<SdfField> BuildSdf(const SdfTriangleMesh& mesh, const SdfGridSpec& grid)
{
    if (mesh.positions.empty())
        return RuntimeResult<SdfField>::Failure(
            SdfError("Hair.SdfMesh", "SDF input has no vertices"));
    if (mesh.indices.empty() || mesh.indices.size() % 3 != 0)
        return RuntimeResult<SdfField>::Failure(
            SdfError("Hair.SdfMesh", "SDF input indices must be a non-empty triangle list"));
    if (!FiniteVector(grid.minCorner))
        return RuntimeResult<SdfField>::Failure(
            SdfError("Hair.SdfDomain", "SDF domain minimum corner is not finite"));
    if (!std::isfinite(grid.cellSize) || !(grid.cellSize > 0.0f))
        return RuntimeResult<SdfField>::Failure(
            SdfError("Hair.SdfDomain", "SDF cell size must be finite and positive"));
    if (!std::isfinite(grid.band) || !(grid.band > 0.0f))
        return RuntimeResult<SdfField>::Failure(
            SdfError("Hair.SdfDomain", "SDF band must be finite and positive"));
    for (int axis = 0; axis < 3; ++axis)
    {
        if (grid.dims[axis] == 0)
            return RuntimeResult<SdfField>::Failure(
                SdfError("Hair.SdfDomain", "SDF domain has a zero voxel dimension"));
    }

    const std::uint64_t voxelCount = static_cast<std::uint64_t>(grid.dims[0]) *
        grid.dims[1] * grid.dims[2];
    if (voxelCount > SdfMaxVoxelCount)
    {
        return RuntimeResult<SdfField>::Failure(SdfError("Hair.SdfBudget",
            "SDF voxel count " + std::to_string(voxelCount) +
                " exceeds the library limit " + std::to_string(SdfMaxVoxelCount)));
    }

    for (std::size_t i = 0; i < mesh.positions.size(); ++i)
    {
        if (!FiniteVector(mesh.positions[i]))
        {
            return RuntimeResult<SdfField>::Failure(SdfError("Hair.SdfMesh",
                "SDF vertex " + std::to_string(i) + " is not finite"));
        }
    }

    SdfMeshReport report;
    report.vertexCount = mesh.positions.size();
    report.triangleCount = mesh.indices.size() / 3;

    std::vector<Triangle> triangles;
    triangles.reserve(report.triangleCount);
    for (std::size_t triangleIndex = 0; triangleIndex < report.triangleCount; ++triangleIndex)
    {
        const std::uint32_t i0 = mesh.indices[triangleIndex * 3 + 0];
        const std::uint32_t i1 = mesh.indices[triangleIndex * 3 + 1];
        const std::uint32_t i2 = mesh.indices[triangleIndex * 3 + 2];
        if (i0 >= mesh.positions.size() || i1 >= mesh.positions.size() ||
            i2 >= mesh.positions.size())
        {
            return RuntimeResult<SdfField>::Failure(SdfError("Hair.SdfMesh",
                "SDF triangle " + std::to_string(triangleIndex) +
                    " references a vertex outside the position list"));
        }
        Triangle tri;
        tri.a = mesh.positions[i0];
        tri.b = mesh.positions[i1];
        tri.c = mesh.positions[i2];
        tri.i0 = i0;
        tri.i1 = i1;
        tri.i2 = i2;
        const Eigen::Vector3f cross = (tri.b - tri.a).cross(tri.c - tri.a);
        const float normalLength = cross.norm();
        if (!(normalLength > kDegenerateNormalLength))
        {
            ++report.degenerateTriangleCount;
            continue;
        }
        tri.normal = cross / normalLength;
        tri.angle0 = CornerAngle(tri.a, tri.b, tri.c);
        tri.angle1 = CornerAngle(tri.b, tri.c, tri.a);
        tri.angle2 = CornerAngle(tri.c, tri.a, tri.b);
        triangles.push_back(tri);
    }
    if (triangles.empty())
    {
        return RuntimeResult<SdfField>::Failure(SdfError("Hair.SdfMesh",
            "SDF input has no triangle with a usable normal (all triangles are degenerate)"));
    }

    // 水密性统计使用全部输入三角形（含退化面），只依赖拓扑，不看面积。
    {
        std::vector<std::uint64_t> edgeKeys;
        edgeKeys.reserve(report.triangleCount * 3);
        for (std::size_t triangleIndex = 0; triangleIndex < report.triangleCount; ++triangleIndex)
        {
            const std::uint32_t i0 = mesh.indices[triangleIndex * 3 + 0];
            const std::uint32_t i1 = mesh.indices[triangleIndex * 3 + 1];
            const std::uint32_t i2 = mesh.indices[triangleIndex * 3 + 2];
            edgeKeys.push_back(EdgeKey(i0, i1));
            edgeKeys.push_back(EdgeKey(i1, i2));
            edgeKeys.push_back(EdgeKey(i2, i0));
        }
        std::sort(edgeKeys.begin(), edgeKeys.end());
        std::size_t runStart = 0;
        while (runStart < edgeKeys.size())
        {
            std::size_t runEnd = runStart + 1;
            while (runEnd < edgeKeys.size() && edgeKeys[runEnd] == edgeKeys[runStart])
                ++runEnd;
            const std::size_t faceCount = runEnd - runStart;
            if (faceCount == 1)
                ++report.openEdgeCount;
            else if (faceCount > 2)
                ++report.nonManifoldEdgeCount;
            runStart = runEnd;
        }
    }

    // 顶点伪法线：入射面法线 × 该顶点内角。累加顺序固定为三角形顺序。
    std::vector<Eigen::Vector3f> vertexPseudonormals(
        mesh.positions.size(), Eigen::Vector3f::Zero());
    for (const Triangle& tri : triangles)
    {
        vertexPseudonormals[tri.i0] += tri.normal * tri.angle0;
        vertexPseudonormals[tri.i1] += tri.normal * tri.angle1;
        vertexPseudonormals[tri.i2] += tri.normal * tri.angle2;
    }
    for (Eigen::Vector3f& normal : vertexPseudonormals)
    {
        const float length = normal.norm();
        if (length > 0.0f)
            normal /= length;
    }

    // 边伪法线：共享该边的面法线之和，按该边两个端点的内角之和加权，
    // 保证权重非负（边的法锥由这两个面法线正张成）。
    std::vector<EdgeNormal> edgePseudonormals;
    {
        struct EdgeFace
        {
            std::uint64_t key;
            std::uint32_t triangle;
        };
        std::vector<EdgeFace> edges;
        edges.reserve(triangles.size() * 3);
        for (std::uint32_t triangleIndex = 0; triangleIndex < triangles.size(); ++triangleIndex)
        {
            const Triangle& tri = triangles[triangleIndex];
            edges.push_back({EdgeKey(tri.i0, tri.i1), triangleIndex});
            edges.push_back({EdgeKey(tri.i1, tri.i2), triangleIndex});
            edges.push_back({EdgeKey(tri.i2, tri.i0), triangleIndex});
        }
        std::sort(edges.begin(), edges.end(), [](const EdgeFace& lhs, const EdgeFace& rhs) {
            if (lhs.key != rhs.key)
                return lhs.key < rhs.key;
            return lhs.triangle < rhs.triangle;
        });
        std::size_t start = 0;
        while (start < edges.size())
        {
            std::size_t end = start + 1;
            while (end < edges.size() && edges[end].key == edges[start].key)
                ++end;
            Eigen::Vector3f sum = Eigen::Vector3f::Zero();
            for (std::size_t i = start; i < end; ++i)
            {
                const Triangle& tri = triangles[edges[i].triangle];
                const std::uint32_t low = static_cast<std::uint32_t>(edges[i].key >> 32);
                const std::uint32_t high = static_cast<std::uint32_t>(edges[i].key & 0xffffffffu);
                float weight = 0.0f;
                const std::uint32_t vertices[3] = {tri.i0, tri.i1, tri.i2};
                const float angles[3] = {tri.angle0, tri.angle1, tri.angle2};
                for (int corner = 0; corner < 3; ++corner)
                {
                    if (vertices[corner] == low || vertices[corner] == high)
                        weight += angles[corner];
                }
                sum += tri.normal * weight;
            }
            const float length = sum.norm();
            EdgeNormal edge;
            edge.key = edges[start].key;
            if (length > 0.0f)
                edge.normal = sum / length;
            else
                edge.normal = Eigen::Vector3f::Zero();
            edgePseudonormals.push_back(edge);
            start = end;
        }
    }

    TriangleBvh bvh;
    bvh.Build(std::move(triangles));
    const std::vector<Triangle>& bvhTriangles = bvh.Triangles();

    SdfField field;
    field.dims = grid.dims;
    field.origin = grid.minCorner;
    field.cellSize = grid.cellSize;
    field.band = grid.band;
    field.mesh = report;
    field.distances.resize(static_cast<std::size_t>(voxelCount));

    // 线程数只影响“谁算哪些 z 切片”，不影响任何体素的距离/符号计算。单线程路径
    // 与加入并行前完全一致；多线程时每个体素只被一个 worker 写一次。
    const std::uint32_t requestedWorkers = grid.threadCount > 1 ? grid.threadCount : 1;
    const std::uint32_t workerCount =
        std::min(requestedWorkers, std::max(1u, grid.dims[2]));
    if (workerCount <= 1)
    {
        BakeVoxelRange(bvh, bvhTriangles, vertexPseudonormals, edgePseudonormals, field,
            0, grid.dims[2]);
    }
    else
    {
        const std::uint32_t sliceCount = grid.dims[2];
        const std::uint32_t sliceChunk = (sliceCount + workerCount - 1) / workerCount;
        std::vector<std::thread> workers;
        workers.reserve(workerCount - 1);
        bool threadStartFailed = false;
        try
        {
            for (std::uint32_t worker = 1; worker < workerCount; ++worker)
            {
                const std::uint32_t zBegin = worker * sliceChunk;
                if (zBegin >= sliceCount)
                    break;
                const std::uint32_t zEnd = std::min(sliceCount, zBegin + sliceChunk);
                workers.emplace_back(BakeVoxelRange, std::cref(bvh), std::cref(bvhTriangles),
                    std::cref(vertexPseudonormals), std::cref(edgePseudonormals),
                    std::ref(field), zBegin, zEnd);
            }
        }
        catch (const std::exception&)
        {
            // 线程创建失败时不发布部分结果；已启动的 worker 必须先 join。
            threadStartFailed = true;
        }
        bool bakeFailed = false;
        try
        {
            BakeVoxelRange(bvh, bvhTriangles, vertexPseudonormals, edgePseudonormals, field,
                0, std::min(sliceCount, sliceChunk));
        }
        catch (const std::exception&)
        {
            bakeFailed = true;
        }
        for (std::thread& worker : workers)
        {
            if (worker.joinable())
                worker.join();
        }
        if (bakeFailed || threadStartFailed)
        {
            return RuntimeResult<SdfField>::Failure(MakeRuntimeError(
                "Hair.SdfThread", "cannot complete the parallel SDF bake"));
        }
    }
    return RuntimeResult<SdfField>::Success(std::move(field));
}

float SampleSdf(const SdfField& field, const Eigen::Vector3f& position)
{
    if (field.distances.empty() || field.dims[0] == 0 || field.dims[1] == 0 ||
        field.dims[2] == 0 || !(field.cellSize > 0.0f))
        return field.band;

    // 域外点：CLI 已按 band 外扩域，因此域外到网格的距离必然超过 band，直接返回 +band。
    const Eigen::Vector3f maximum = field.origin + field.cellSize * Eigen::Vector3f(
        static_cast<float>(field.dims[0]), static_cast<float>(field.dims[1]),
        static_cast<float>(field.dims[2]));
    if (position.x() < field.origin.x() || position.y() < field.origin.y() ||
        position.z() < field.origin.z() || position.x() > maximum.x() ||
        position.y() > maximum.y() || position.z() > maximum.z())
        return field.band;

    // 从“体素中心坐标系”换算到连续索引：体素 0 的中心对应索引 0。
    Eigen::Vector3f grid = (position - field.origin) / field.cellSize -
        Eigen::Vector3f(0.5f, 0.5f, 0.5f);
    std::uint32_t base[3] = {0, 0, 0};
    float fraction[3] = {0.0f, 0.0f, 0.0f};
    for (int axis = 0; axis < 3; ++axis)
    {
        const float coordinate = grid[axis];
        const float last = static_cast<float>(field.dims[axis] - 1);
        if (coordinate <= 0.0f)
        {
            base[axis] = 0;
            fraction[axis] = 0.0f;
        }
        else if (coordinate >= last)
        {
            base[axis] = field.dims[axis] - 1;
            fraction[axis] = 0.0f;
        }
        else
        {
            base[axis] = static_cast<std::uint32_t>(coordinate);
            fraction[axis] = coordinate - static_cast<float>(base[axis]);
        }
    }
    const std::uint32_t high[3] = {
        std::min(base[0] + 1, field.dims[0] - 1),
        std::min(base[1] + 1, field.dims[1] - 1),
        std::min(base[2] + 1, field.dims[2] - 1)};

    const float fx = fraction[0];
    const float fy = fraction[1];
    const float fz = fraction[2];
    float values[2][2][2];
    for (int oz = 0; oz < 2; ++oz)
    {
        for (int oy = 0; oy < 2; ++oy)
        {
            for (int ox = 0; ox < 2; ++ox)
            {
                values[oz][oy][ox] = field.Distance(base[0] + (ox ? high[0] - base[0] : 0),
                    base[1] + (oy ? high[1] - base[1] : 0),
                    base[2] + (oz ? high[2] - base[2] : 0));
            }
        }
    }
    float lower = 0.0f;
    float upper = 0.0f;
    for (int oz = 0; oz < 2; ++oz)
    {
        float sliceLower = 0.0f;
        float sliceUpper = 0.0f;
        for (int oy = 0; oy < 2; ++oy)
        {
            const float row = values[oz][oy][0] * (1.0f - fx) + values[oz][oy][1] * fx;
            if (oy == 0)
                sliceLower = row;
            else
                sliceUpper = row;
        }
        const float slice = sliceLower * (1.0f - fy) + sliceUpper * fy;
        if (oz == 0)
            lower = slice;
        else
            upper = slice;
    }
    // 截断场的插值仍然是截断场：结果必然落在 [-band, +band]。
    const float interpolated = lower * (1.0f - fz) + upper * fz;
    return std::max(-field.band, std::min(field.band, interpolated));
}

std::vector<std::uint8_t> SerializeSdf(const SdfField& field)
{
    const std::size_t payloadBytes = field.distances.size() * sizeof(float);
    std::vector<std::uint8_t> bytes(SdfFileHeaderBytes + payloadBytes, 0);
    std::memcpy(bytes.data(), SdfFileMagic.data(), SdfFileMagic.size());
    WriteU32(bytes, 8, SdfFileVersion);
    WriteU32(bytes, 12, static_cast<std::uint32_t>(SdfFileHeaderBytes));
    WriteU32(bytes, 16, field.dims[0]);
    WriteU32(bytes, 20, field.dims[1]);
    WriteU32(bytes, 24, field.dims[2]);
    WriteF32(bytes, 28, field.origin.x());
    WriteF32(bytes, 32, field.origin.y());
    WriteF32(bytes, 36, field.origin.z());
    WriteF32(bytes, 40, field.cellSize);
    WriteF32(bytes, 44, field.band);
    WriteU32(bytes, 48, 0); // payloadFormat = float32
    WriteU32(bytes, 52, static_cast<std::uint32_t>(payloadBytes));
    WriteU32(bytes, 60, 0); // reserved
    if (payloadBytes > 0)
        std::memcpy(bytes.data() + SdfFileHeaderBytes, field.distances.data(), payloadBytes);
    // 校验和必须在 payload 写入之后计算，否则校验的是零字节。
    WriteU32(bytes, 56, PayloadChecksum(bytes, SdfFileHeaderBytes, payloadBytes));
    return bytes;
}

RuntimeResult<SdfField> ParseSdf(
    const std::vector<std::uint8_t>& bytes, const std::string& sourcePath)
{
    if (bytes.size() < SdfFileHeaderBytes)
    {
        return RuntimeResult<SdfField>::Failure(SdfError("Hair.SdfFormat",
            "SDF file is shorter than its " + std::to_string(SdfFileHeaderBytes) +
                "-byte header (truncated file)", sourcePath));
    }
    if (std::memcmp(bytes.data(), SdfFileMagic.data(), SdfFileMagic.size()) != 0)
        return RuntimeResult<SdfField>::Failure(
            SdfError("Hair.SdfFormat", "SDF magic does not match", sourcePath));
    if (ReadU32(bytes, 8) != SdfFileVersion)
        return RuntimeResult<SdfField>::Failure(SdfError("Hair.SdfFormat",
            "unsupported SDF version " + std::to_string(ReadU32(bytes, 8)), sourcePath));
    if (ReadU32(bytes, 12) != SdfFileHeaderBytes)
        return RuntimeResult<SdfField>::Failure(SdfError("Hair.SdfFormat",
            "SDF header size does not match this version", sourcePath));
    if (ReadU32(bytes, 48) != 0)
        return RuntimeResult<SdfField>::Failure(SdfError("Hair.SdfFormat",
            "unsupported SDF payload format " + std::to_string(ReadU32(bytes, 48)), sourcePath));

    SdfField field;
    field.dims = {ReadU32(bytes, 16), ReadU32(bytes, 20), ReadU32(bytes, 24)};
    if (field.dims[0] == 0 || field.dims[1] == 0 || field.dims[2] == 0)
        return RuntimeResult<SdfField>::Failure(
            SdfError("Hair.SdfFormat", "SDF dimensions must be non-zero", sourcePath));
    field.origin = Eigen::Vector3f(
        ReadF32(bytes, 28), ReadF32(bytes, 32), ReadF32(bytes, 36));
    field.cellSize = ReadF32(bytes, 40);
    field.band = ReadF32(bytes, 44);
    if (!FiniteVector(field.origin) || !std::isfinite(field.cellSize) ||
        !std::isfinite(field.band) || !(field.cellSize > 0.0f) || !(field.band > 0.0f))
        return RuntimeResult<SdfField>::Failure(
            SdfError("Hair.SdfFormat", "SDF header has a non-finite or non-positive domain",
                sourcePath));

    const std::uint64_t voxelCount = static_cast<std::uint64_t>(field.dims[0]) *
        field.dims[1] * field.dims[2];
    if (voxelCount > SdfMaxVoxelCount)
        return RuntimeResult<SdfField>::Failure(SdfError("Hair.SdfBudget",
            "SDF voxel count exceeds the library limit", sourcePath));
    const std::uint64_t expectedPayload = voxelCount * sizeof(float);
    if (ReadU32(bytes, 52) != expectedPayload)
        return RuntimeResult<SdfField>::Failure(SdfError("Hair.SdfFormat",
            "SDF payload size field does not match its dimensions", sourcePath));
    if (bytes.size() != SdfFileHeaderBytes + expectedPayload)
        return RuntimeResult<SdfField>::Failure(SdfError("Hair.SdfFormat",
            "SDF file size does not match header + payload (truncated or trailing bytes)",
            sourcePath));
    const std::uint32_t storedChecksum = ReadU32(bytes, 56);
    if (PayloadChecksum(bytes, SdfFileHeaderBytes, static_cast<std::size_t>(expectedPayload)) !=
        storedChecksum)
        return RuntimeResult<SdfField>::Failure(
            SdfError("Hair.SdfFormat", "SDF payload checksum does not match", sourcePath));

    field.distances.resize(static_cast<std::size_t>(voxelCount));
    const float tolerance = field.band * 1.0e-6f + 1.0e-6f;
    for (std::size_t i = 0; i < field.distances.size(); ++i)
    {
        const float value = ReadF32(bytes, SdfFileHeaderBytes + i * sizeof(float));
        // 读取端与生成端共用截断规则：|value| <= band，且不得是 NaN/Inf。
        if (!std::isfinite(value) || std::fabs(value) > field.band + tolerance)
            return RuntimeResult<SdfField>::Failure(SdfError("Hair.SdfFormat",
                "SDF payload value " + std::to_string(i) +
                    " is not finite or violates the band truncation rule", sourcePath));
        field.distances[i] = value;
    }
    return RuntimeResult<SdfField>::Success(std::move(field));
}

RuntimeResult<void> WriteSdfFile(const SdfField& field, const std::filesystem::path& path)
{
    const std::string source = path.u8string();
    if (field.dims[0] == 0 || field.dims[1] == 0 || field.dims[2] == 0)
        return RuntimeResult<void>::Failure(
            SdfError("Hair.SdfFormat", "refusing to write an SDF with zero dimensions", source));
    const std::uint64_t voxelCount = static_cast<std::uint64_t>(field.dims[0]) *
        field.dims[1] * field.dims[2];
    if (field.distances.size() != voxelCount)
        return RuntimeResult<void>::Failure(SdfError("Hair.SdfFormat",
            "refusing to write an SDF whose payload size does not match its dimensions", source));
    if (!(field.cellSize > 0.0f) || !(field.band > 0.0f))
        return RuntimeResult<void>::Failure(
            SdfError("Hair.SdfFormat", "refusing to write an SDF with an invalid domain", source));
    for (float value : field.distances)
    {
        if (!std::isfinite(value) || std::fabs(value) > field.band)
            return RuntimeResult<void>::Failure(SdfError("Hair.SdfFormat",
                "refusing to write a non-finite or untruncated SDF distance", source));
    }

    const std::vector<std::uint8_t> bytes = SerializeSdf(field);
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    if (!output)
        return RuntimeResult<void>::Failure(
            SdfError("Hair.SdfWrite", "cannot open SDF output file", source));
    output.write(reinterpret_cast<const char*>(bytes.data()),
        static_cast<std::streamsize>(bytes.size()));
    if (!output)
        return RuntimeResult<void>::Failure(
            SdfError("Hair.SdfWrite", "cannot write the complete SDF file", source));
    return RuntimeResult<void>::Success();
}

RuntimeResult<SdfField> LoadSdfFile(const std::filesystem::path& path)
{
    const std::string source = path.u8string();
    std::ifstream input(path, std::ios::binary | std::ios::ate);
    if (!input)
        return RuntimeResult<SdfField>::Failure(
            SdfError("Hair.SdfRead", "cannot open SDF file", source));
    const std::streamoff size = input.tellg();
    if (size < 0)
        return RuntimeResult<SdfField>::Failure(
            SdfError("Hair.SdfRead", "cannot determine SDF file size", source));
    // 上限包含 header，避免超大文件在分配前进入内存。
    const std::uint64_t maximumBytes =
        SdfMaxVoxelCount * sizeof(float) + SdfFileHeaderBytes;
    if (static_cast<std::uint64_t>(size) > maximumBytes)
        return RuntimeResult<SdfField>::Failure(
            SdfError("Hair.SdfBudget", "SDF file exceeds the library size limit", source));
    try
    {
        std::vector<std::uint8_t> bytes(static_cast<std::size_t>(size));
        input.seekg(0);
        if (!bytes.empty() &&
            !input.read(reinterpret_cast<char*>(bytes.data()), static_cast<std::streamsize>(size)))
            return RuntimeResult<SdfField>::Failure(
                SdfError("Hair.SdfRead", "cannot read the complete SDF file", source));
        return ParseSdf(bytes, source);
    }
    catch (const std::bad_alloc&)
    {
        return RuntimeResult<SdfField>::Failure(
            SdfError("Hair.SdfBudget", "cannot allocate SDF input bytes", source));
    }
}
}
