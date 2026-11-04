#include "hair/collisionProxy.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <fstream>
#include <iomanip>
#include <limits>
#include <locale>
#include <sstream>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace VL::Hair
{
namespace
{
// 聚类后三角形面积（叉积长度，厘米²）低于该值视为零面积。与 SdfBuilder 的
// 退化法线阈值同量级：远小于正式资产的最小面，也不会把浮点噪声留下。
constexpr float kDegenerateCrossLength = 1.0e-12f;
// floor(p / cellSize) 必须落在 int64 安全范围内；超出说明格距相对坐标过小。
constexpr double kMaxCellCoordinate = 1.0e15;

struct CellKey
{
    std::int64_t x = 0;
    std::int64_t y = 0;
    std::int64_t z = 0;
};

bool operator==(const CellKey& lhs, const CellKey& rhs)
{
    return lhs.x == rhs.x && lhs.y == rhs.y && lhs.z == rhs.z;
}

std::uint64_t MixBits(std::uint64_t value)
{
    // splitmix64 的收尾混合：让相邻格坐标散开到不同桶，避免链式退化。
    value += 0x9e3779b97f4a7c15ull;
    value = (value ^ (value >> 30)) * 0xbf58476d1ce4e5b9ull;
    value = (value ^ (value >> 27)) * 0x94d049bb133111ebull;
    return value ^ (value >> 31);
}

struct CellKeyHash
{
    std::size_t operator()(const CellKey& key) const noexcept
    {
        // 哈希只决定桶位置，不参与输出排序，因此不同 size_t 宽度不影响结果。
        const std::uint64_t hash = MixBits(static_cast<std::uint64_t>(key.x)) ^
            MixBits(static_cast<std::uint64_t>(key.y) + 0x517cc1b727220a95ull) ^
            MixBits(static_cast<std::uint64_t>(key.z) + 0x6eed0e9da4d94a4full);
        return static_cast<std::size_t>(hash);
    }
};

// 三角形的精确顶点集合，统一排序后再比较：同集合的重复面（无论绕序）只保留首次出现。
struct TriangleKey
{
    std::uint32_t a = 0;
    std::uint32_t b = 0;
    std::uint32_t c = 0;
};

bool operator==(const TriangleKey& lhs, const TriangleKey& rhs)
{
    return lhs.a == rhs.a && lhs.b == rhs.b && lhs.c == rhs.c;
}

struct TriangleKeyHash
{
    std::size_t operator()(const TriangleKey& key) const noexcept
    {
        std::uint64_t hash = MixBits(static_cast<std::uint64_t>(key.a));
        hash ^= MixBits(static_cast<std::uint64_t>(key.b) + 0x517cc1b727220a95ull);
        hash ^= MixBits(static_cast<std::uint64_t>(key.c) + 0x6eed0e9da4d94a4full);
        return static_cast<std::size_t>(hash);
    }
};

RuntimeError ProxyError(std::string code, std::string message, std::string sourcePath = {})
{
    return MakeRuntimeError(std::move(code), std::move(message), std::move(sourcePath));
}

bool FiniteVector(const Eigen::Vector3f& value)
{
    return std::isfinite(value.x()) && std::isfinite(value.y()) && std::isfinite(value.z());
}

bool InsideRegion(const Eigen::Vector3f& position, const Eigen::Vector3f& minimum,
    const Eigen::Vector3f& maximum)
{
    // 含边界：顶点恰好落在区域面上仍算区域内的表面，不会被裁掉。
    return position.x() >= minimum.x() && position.x() <= maximum.x() &&
        position.y() >= minimum.y() && position.y() <= maximum.y() &&
        position.z() >= minimum.z() && position.z() <= maximum.z();
}

bool CellOf(const Eigen::Vector3f& position, float cellSize, CellKey& key)
{
    const double cell = static_cast<double>(cellSize);
    std::int64_t coordinates[3] = {0, 0, 0};
    for (int axis = 0; axis < 3; ++axis)
    {
        const double scaled = static_cast<double>(position[axis]) / cell;
        if (!(scaled >= -kMaxCellCoordinate) || !(scaled <= kMaxCellCoordinate))
            return false;
        coordinates[axis] = static_cast<std::int64_t>(std::floor(scaled));
    }
    key = CellKey{coordinates[0], coordinates[1], coordinates[2]};
    return true;
}

std::string BoundsText(const Eigen::Vector3f& minimum, const Eigen::Vector3f& maximum)
{
    std::ostringstream text;
    text.imbue(std::locale::classic());
    text << "x [" << minimum.x() << ", " << maximum.x() << "], y ["
         << minimum.y() << ", " << maximum.y() << "], z ["
         << minimum.z() << ", " << maximum.z() << "]";
    return text.str();
}
}

RuntimeResult<CollisionProxy> BuildCollisionProxy(
    const SdfTriangleMesh& input, const CollisionProxyOptions& options)
{
    if (input.positions.empty())
    {
        return RuntimeResult<CollisionProxy>::Failure(
            ProxyError("Hair.CollisionProxyInput", "collision proxy input has no vertices"));
    }
    if (input.indices.empty() || input.indices.size() % 3 != 0)
    {
        return RuntimeResult<CollisionProxy>::Failure(ProxyError(
            "Hair.CollisionProxyInput", "collision proxy input indices must be a non-empty triangle list"));
    }
    if (input.positions.size() > std::numeric_limits<std::uint32_t>::max())
    {
        return RuntimeResult<CollisionProxy>::Failure(ProxyError(
            "Hair.CollisionProxyBudget", "collision proxy input has too many vertices for 32-bit indices"));
    }
    if (!std::isfinite(options.cellSize) || !(options.cellSize > 0.0f))
    {
        return RuntimeResult<CollisionProxy>::Failure(ProxyError(
            "Hair.CollisionProxyDomain", "collision proxy cell size must be finite and positive"));
    }
    if (options.hasRegion)
    {
        if (!FiniteVector(options.regionMin) || !FiniteVector(options.regionMax))
        {
            return RuntimeResult<CollisionProxy>::Failure(ProxyError(
                "Hair.CollisionProxyDomain", "collision proxy region bounds are not finite"));
        }
        if (!(options.regionMin.x() < options.regionMax.x() &&
                options.regionMin.y() < options.regionMax.y() &&
                options.regionMin.z() < options.regionMax.z()))
        {
            return RuntimeResult<CollisionProxy>::Failure(ProxyError(
                "Hair.CollisionProxyDomain",
                "collision proxy region must have positive volume (min < max on every axis)"));
        }
    }

    CollisionProxy proxy;
    CollisionProxyReport& report = proxy.report;
    report.inputVertexCount = input.positions.size();
    report.inputTriangleCount = input.indices.size() / 3;

    bool hasInputBounds = false;
    for (std::size_t vertex = 0; vertex < input.positions.size(); ++vertex)
    {
        const Eigen::Vector3f& position = input.positions[vertex];
        if (!FiniteVector(position))
        {
            return RuntimeResult<CollisionProxy>::Failure(ProxyError("Hair.CollisionProxyInput",
                "collision proxy vertex " + std::to_string(vertex) + " is not finite"));
        }
        if (!hasInputBounds)
        {
            report.inputBoundsMin = position;
            report.inputBoundsMax = position;
            hasInputBounds = true;
        }
        else
        {
            report.inputBoundsMin = report.inputBoundsMin.cwiseMin(position);
            report.inputBoundsMax = report.inputBoundsMax.cwiseMax(position);
        }
    }

    // 索引校验与区域过滤共用一次遍历；越界索引必须在聚类前拒绝，避免后面用错误索引
    // 静默丢面。被拒绝的三角形完全不参与顶点聚类。
    std::vector<std::uint32_t> keptTriangles;
    keptTriangles.reserve(report.inputTriangleCount);
    std::vector<std::uint8_t> referenced(input.positions.size(), 0);
    for (std::size_t triangle = 0; triangle < report.inputTriangleCount; ++triangle)
    {
        const std::uint32_t i0 = input.indices[triangle * 3 + 0];
        const std::uint32_t i1 = input.indices[triangle * 3 + 1];
        const std::uint32_t i2 = input.indices[triangle * 3 + 2];
        if (i0 >= input.positions.size() || i1 >= input.positions.size() ||
            i2 >= input.positions.size())
        {
            return RuntimeResult<CollisionProxy>::Failure(ProxyError("Hair.CollisionProxyInput",
                "collision proxy triangle " + std::to_string(triangle) +
                    " references a vertex outside the position list"));
        }
        if (options.hasRegion &&
            !(InsideRegion(input.positions[i0], options.regionMin, options.regionMax) &&
                InsideRegion(input.positions[i1], options.regionMin, options.regionMax) &&
                InsideRegion(input.positions[i2], options.regionMin, options.regionMax)))
        {
            ++report.regionRejectedTriangleCount;
            continue;
        }
        keptTriangles.push_back(static_cast<std::uint32_t>(triangle));
        referenced[i0] = 1;
        referenced[i1] = 1;
        referenced[i2] = 1;
    }
    if (keptTriangles.empty())
    {
        return RuntimeResult<CollisionProxy>::Failure(ProxyError("Hair.CollisionProxyRegion",
            "collision proxy region rejects every triangle; region " +
                BoundsText(options.regionMin, options.regionMax) + " contains no complete triangle"));
    }

    // 顶点聚类。簇号按顶点首次出现顺序分配；中心在 double 中累加，同一输入下
    // 累加顺序固定，因此结果不依赖哈希表的遍历顺序。
    std::vector<std::uint32_t> vertexCluster(input.positions.size(),
        std::numeric_limits<std::uint32_t>::max());
    std::unordered_map<CellKey, std::uint32_t, CellKeyHash> clusters;
    clusters.reserve(static_cast<std::size_t>(keptTriangles.size()) * 3);
    std::vector<Eigen::Vector3d> clusterSums;
    std::vector<std::uint32_t> clusterCounts;
    for (std::size_t vertex = 0; vertex < input.positions.size(); ++vertex)
    {
        if (referenced[vertex] == 0)
            continue;
        CellKey key;
        if (!CellOf(input.positions[vertex], options.cellSize, key))
        {
            return RuntimeResult<CollisionProxy>::Failure(ProxyError("Hair.CollisionProxyDomain",
                "collision proxy vertex " + std::to_string(vertex) +
                    " is too far from the origin for the requested cell size"));
        }
        std::uint32_t cluster = 0;
        const auto found = clusters.find(key);
        if (found == clusters.end())
        {
            cluster = static_cast<std::uint32_t>(clusterSums.size());
            clusters.emplace(key, cluster);
            clusterSums.push_back(Eigen::Vector3d::Zero());
            clusterCounts.push_back(0);
        }
        else
        {
            cluster = found->second;
        }
        vertexCluster[vertex] = cluster;
        const Eigen::Vector3f& position = input.positions[vertex];
        clusterSums[cluster] += Eigen::Vector3d(
            static_cast<double>(position.x()), static_cast<double>(position.y()),
            static_cast<double>(position.z()));
        ++clusterCounts[cluster];
    }
    report.clusterCount = clusterSums.size();

    std::vector<Eigen::Vector3f> clusterCentroids(clusterSums.size());
    for (std::size_t cluster = 0; cluster < clusterSums.size(); ++cluster)
    {
        const double inverseCount = 1.0 / static_cast<double>(clusterCounts[cluster]);
        clusterCentroids[cluster] = Eigen::Vector3f(
            static_cast<float>(clusterSums[cluster].x() * inverseCount),
            static_cast<float>(clusterSums[cluster].y() * inverseCount),
            static_cast<float>(clusterSums[cluster].z() * inverseCount));
    }

    // 重映射并保留输入绕序。先丢弃折叠（同一簇）与零面积三角形，再按顶点集合去重；
    // 顺序影响“保留哪一个”因此固定为输入三角形顺序。
    std::vector<std::uint32_t> remapped;
    remapped.reserve(keptTriangles.size() * 3);
    std::unordered_set<TriangleKey, TriangleKeyHash> seenTriangles;
    seenTriangles.reserve(keptTriangles.size());
    for (std::uint32_t triangle : keptTriangles)
    {
        const std::uint32_t c0 = vertexCluster[input.indices[triangle * 3 + 0]];
        const std::uint32_t c1 = vertexCluster[input.indices[triangle * 3 + 1]];
        const std::uint32_t c2 = vertexCluster[input.indices[triangle * 3 + 2]];
        if (c0 == c1 || c1 == c2 || c2 == c0)
        {
            ++report.droppedDegenerateTriangleCount;
            continue;
        }
        const Eigen::Vector3f& a = clusterCentroids[c0];
        const Eigen::Vector3f& b = clusterCentroids[c1];
        const Eigen::Vector3f& c = clusterCentroids[c2];
        const Eigen::Vector3f cross = (b - a).cross(c - a);
        if (!(cross.norm() > kDegenerateCrossLength))
        {
            ++report.droppedDegenerateTriangleCount;
            continue;
        }
        std::array<std::uint32_t, 3> sorted{c0, c1, c2};
        std::sort(sorted.begin(), sorted.end());
        const TriangleKey key{sorted[0], sorted[1], sorted[2]};
        if (!seenTriangles.insert(key).second)
        {
            ++report.droppedDuplicateTriangleCount;
            continue;
        }
        remapped.push_back(c0);
        remapped.push_back(c1);
        remapped.push_back(c2);
    }
    if (remapped.empty())
    {
        return RuntimeResult<CollisionProxy>::Failure(ProxyError("Hair.CollisionProxyEmpty",
            "collision proxy left no triangles after clustering (" +
                std::to_string(report.droppedDegenerateTriangleCount) + " degenerate, " +
                std::to_string(report.droppedDuplicateTriangleCount) + " duplicate); " +
                "decrease the cell size or widen the region"));
    }

    // 压缩：按输出三角形首次引用顺序重排顶点，删除未被使用的簇。
    std::vector<std::int64_t> outputIndexOfCluster(clusterCentroids.size(), -1);
    proxy.mesh.indices.reserve(remapped.size());
    proxy.mesh.positions.reserve(clusterCentroids.size());
    for (std::uint32_t cluster : remapped)
    {
        if (outputIndexOfCluster[cluster] < 0)
        {
            outputIndexOfCluster[cluster] =
                static_cast<std::int64_t>(proxy.mesh.positions.size());
            proxy.mesh.positions.push_back(clusterCentroids[cluster]);
        }
        proxy.mesh.indices.push_back(
            static_cast<std::uint32_t>(outputIndexOfCluster[cluster]));
    }
    report.outputVertexCount = proxy.mesh.positions.size();
    report.outputTriangleCount = proxy.mesh.indices.size() / 3;

    if (report.outputTriangleCount < options.minimumTriangleCount)
    {
        return RuntimeResult<CollisionProxy>::Failure(ProxyError("Hair.CollisionProxyBudget",
            "collision proxy output has " + std::to_string(report.outputTriangleCount) +
                " triangles, below the required minimum of " +
                std::to_string(options.minimumTriangleCount) +
                "; decrease the cell size or widen the region"));
    }

    report.outputBoundsMin = proxy.mesh.positions.front();
    report.outputBoundsMax = proxy.mesh.positions.front();
    for (const Eigen::Vector3f& position : proxy.mesh.positions)
    {
        report.outputBoundsMin = report.outputBoundsMin.cwiseMin(position);
        report.outputBoundsMax = report.outputBoundsMax.cwiseMax(position);
    }

    // 与 SdfBuilder 共用的无向边统计：只被一个面使用 = 开放边，三个及以上 = 非流形边。
    const SdfMeshReport topology = CountMeshTopology(proxy.mesh);
    report.openEdgeCount = topology.openEdgeCount;
    report.nonManifoldEdgeCount = topology.nonManifoldEdgeCount;

    return RuntimeResult<CollisionProxy>::Success(std::move(proxy));
}

std::string SerializeCollisionProxyObj(const CollisionProxy& proxy,
    const std::string& objectName, const std::vector<std::string>& commentLines)
{
    std::ostringstream output;
    output.imbue(std::locale::classic());
    for (const std::string& line : commentLines)
        output << "# " << line << '\n';
    output << "o " << objectName << '\n';
    // float 的 max_digits10 是 9：9 位有效数字可以无损往返 float32。
    output << std::setprecision(9);
    for (const Eigen::Vector3f& position : proxy.mesh.positions)
    {
        output << "v " << position.x() << ' ' << position.y() << ' ' << position.z() << '\n';
    }
    for (std::size_t triangle = 0; triangle < proxy.mesh.indices.size() / 3; ++triangle)
    {
        // OBJ 索引从 1 开始。
        output << "f " << (proxy.mesh.indices[triangle * 3 + 0] + 1) << ' '
               << (proxy.mesh.indices[triangle * 3 + 1] + 1) << ' '
               << (proxy.mesh.indices[triangle * 3 + 2] + 1) << '\n';
    }
    return output.str();
}

RuntimeResult<void> WriteCollisionProxyObj(const CollisionProxy& proxy,
    const std::filesystem::path& path, const std::string& objectName,
    const std::vector<std::string>& commentLines)
{
    const std::string text = SerializeCollisionProxyObj(proxy, objectName, commentLines);
    std::ofstream outputFile(path, std::ios::binary | std::ios::trunc);
    if (!outputFile)
    {
        return RuntimeResult<void>::Failure(ProxyError("Hair.CollisionProxyWrite",
            "cannot open collision proxy OBJ output file", path.u8string()));
    }
    outputFile.write(text.data(), static_cast<std::streamsize>(text.size()));
    if (!outputFile)
    {
        return RuntimeResult<void>::Failure(ProxyError("Hair.CollisionProxyWrite",
            "cannot write the complete collision proxy OBJ file", path.u8string()));
    }
    return RuntimeResult<void>::Success();
}
}
