#include "hair/tressfxProxySkinning.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>
#include <set>
#include <stdexcept>
#include <string>
#include <unordered_map>

namespace VL::Hair
{
namespace
{
// 空间哈希单元。身体与代理都在同一绑定空间的厘米坐标下，单元边长取
// maxNearestDistanceCm；正式代理由此只需检查半径 1 的邻域。
struct ProxySkinCell
{
    int32_t x = 0;
    int32_t y = 0;
    int32_t z = 0;
};

bool operator==(const ProxySkinCell& left, const ProxySkinCell& right)
{
    return left.x == right.x && left.y == right.y && left.z == right.z;
}

void MixHash(uint64_t& hash, uint32_t value)
{
    // FNV-1a 混合；单元坐标是整数，不需要处理字符串/浮点身份。
    hash ^= value;
    hash *= 1099511628211ull;
}

struct ProxySkinCellHash
{
    size_t operator()(const ProxySkinCell& cell) const
    {
        uint64_t hash = 1469598103934665603ull;
        MixHash(hash, static_cast<uint32_t>(cell.x));
        MixHash(hash, static_cast<uint32_t>(cell.y));
        MixHash(hash, static_cast<uint32_t>(cell.z));
        return static_cast<size_t>(hash);
    }
};

ProxySkinCell CellOf(const Eigen::Vector3f& position, float cellSize)
{
    return ProxySkinCell{
        static_cast<int32_t>(std::floor(position.x() / cellSize)),
        static_cast<int32_t>(std::floor(position.y() / cellSize)),
        static_cast<int32_t>(std::floor(position.z() / cellSize))};
}

using ProxySkinGrid =
    std::unordered_map<ProxySkinCell, std::vector<uint32_t>, ProxySkinCellHash>;

// 绑定姿态身体顶点只建一次哈希网格；查询按 Chebyshev 环扩展：查完半径 r 后，
// 任何未访问单元的最近可能距离是 r*cellSize，因此 best < r*cellSize 时可以停止。
// 等距时取全局下标更小的顶点，保证输出与哈希桶顺序无关。
// 返回 false 表示 maxDistanceCm 内没有身体顶点；调用方按策略决定抛错还是记录。
bool FindNearestBodyVertex(
    const ProxySkinGrid& grid,
    const SdfProxySkinSource& body,
    float cellSize,
    float maxDistanceCm,
    const Eigen::Vector3f& query,
    uint32_t& outIndex,
    float& outDistanceCm)
{
    const ProxySkinCell center = CellOf(query, cellSize);
    const int maxRing = static_cast<int>(std::ceil(maxDistanceCm / cellSize)) + 2;
    float bestSquared = std::numeric_limits<float>::infinity();
    uint32_t bestIndex = std::numeric_limits<uint32_t>::max();
    for (int ring = 0; ring <= maxRing; ++ring)
    {
        for (int dx = -ring; dx <= ring; ++dx)
        {
            for (int dy = -ring; dy <= ring; ++dy)
            {
                for (int dz = -ring; dz <= ring; ++dz)
                {
                    if (std::max({std::abs(dx), std::abs(dy), std::abs(dz)}) != ring)
                        continue;
                    const ProxySkinCell cell{center.x + dx, center.y + dy,
                        center.z + dz};
                    const auto found = grid.find(cell);
                    if (found == grid.end())
                        continue;
                    for (uint32_t index : found->second)
                    {
                        const float squared =
                            (body.positions[index] - query).squaredNorm();
                        if (squared < bestSquared ||
                            (squared == bestSquared && index < bestIndex))
                        {
                            bestSquared = squared;
                            bestIndex = index;
                        }
                    }
                }
            }
        }
        // 未访问的环半径 >= ring+1 时距离至少是 ring*cellSize；用严格小于
        // 保留等距候选，避免漏掉下标更小的同距顶点。
        if (bestIndex != std::numeric_limits<uint32_t>::max() &&
            bestSquared < static_cast<float>(ring * ring) * cellSize * cellSize)
            break;
    }
    if (bestIndex == std::numeric_limits<uint32_t>::max())
        return false;
    outIndex = bestIndex;
    outDistanceCm = std::sqrt(bestSquared);
    return true;
}
} // namespace

SdfProxySkinTransfer BuildTressfxProxySkinTransfer(
    const std::vector<Eigen::Vector3f>& proxyPositions,
    const SdfProxySkinSource& body,
    float maxNearestDistanceCm,
    TressfxSkinTransferPolicy policy)
{
    if (proxyPositions.empty())
        throw std::runtime_error("TressFX collision proxy has no vertices");
    if (body.positions == nullptr || body.positionCount == 0 ||
        body.influenceOffsets == nullptr || body.influences == nullptr)
        throw std::runtime_error("TressFX collision proxy skin source is empty");
    if (!std::isfinite(maxNearestDistanceCm) || !(maxNearestDistanceCm > 0.0f))
        throw std::runtime_error("TressFX collision proxy transfer radius must be positive");
    for (size_t index = 0; index < body.positionCount; ++index)
    {
        if (!body.positions[index].allFinite())
            throw std::runtime_error(
                "TressFX collision proxy skin source has a non-finite vertex");
        if (body.influenceOffsets[index] > body.influenceOffsets[index + 1])
            throw std::runtime_error(
                "TressFX collision proxy skin source has an invalid influence range");
    }

    // 每个代理顶点先记录归一化后的影响；最后一次性展开成 CSR word 流，避免
    // 在 2,572 个顶点上反复搬移前缀。
    struct ProxyInfluences
    {
        uint32_t nearestBodyVertex = 0;
        std::vector<SdfProxySkinSourceInfluence> influences;
    };
    // 身体顶点只建一次哈希网格：2,572 个代理顶点 × 253k 个身体顶点若逐点重建
    // 会是 O(n·m)，这里保持一次构建、每点邻域查询。
    ProxySkinGrid grid;
    grid.reserve(body.positionCount);
    for (size_t index = 0; index < body.positionCount; ++index)
        grid[CellOf(body.positions[index], maxNearestDistanceCm)].push_back(
            static_cast<uint32_t>(index));

    std::vector<ProxyInfluences> perVertex(proxyPositions.size());
    std::vector<uint32_t> unmatchedVertices;
    std::set<uint32_t> usedBodyVertices;
    float maxDistance = 0.0f;
    uint32_t maxInfluences = 0;
    uint32_t maxJoint = 0;

    for (size_t vertex = 0; vertex < proxyPositions.size(); ++vertex)
    {
        if (!proxyPositions[vertex].allFinite())
            throw std::runtime_error(
                "TressFX collision proxy has a non-finite vertex");
        uint32_t nearest = 0;
        float distanceCm = 0.0f;
        const bool found = FindNearestBodyVertex(grid, body,
            maxNearestDistanceCm, maxNearestDistanceCm, proxyPositions[vertex],
            nearest, distanceCm);
        // 未匹配只有两种可解释原因：查询半径内确实没有身体顶点，或最近顶点的
        // 权重和不为正。Reject 路径（碰撞代理）把两者都当错空间硬错误；Record
        // 路径（发丝根点）交给调用方按根回退。
        if (!found || distanceCm > maxNearestDistanceCm)
        {
            if (policy == TressfxSkinTransferPolicy::RecordUnmatched)
            {
                unmatchedVertices.push_back(static_cast<uint32_t>(vertex));
                continue;
            }
            throw std::runtime_error(
                "TressFX collision proxy vertex " + std::to_string(vertex) +
                (found ? " is " + std::to_string(distanceCm) +
                    " cm from the nearest body vertex (limit " +
                    std::to_string(maxNearestDistanceCm) + " cm)"
                    : " has no nearest body vertex within the transfer radius"));
        }
        maxDistance = std::max(maxDistance, distanceCm);
        usedBodyVertices.insert(nearest);
        perVertex[vertex].nearestBodyVertex = nearest;

        const uint32_t begin = body.influenceOffsets[nearest];
        const uint32_t end = body.influenceOffsets[nearest + 1];
        float weightSum = 0.0f;
        for (uint32_t influence = begin; influence < end; ++influence)
        {
            const SdfProxySkinSourceInfluence& source = body.influences[influence];
            if (!std::isfinite(source.weight))
                throw std::runtime_error(
                    "TressFX collision proxy skin source has a non-finite weight");
            if (body.jointCount > 0 && source.joint >= body.jointCount)
                throw std::runtime_error(
                    "TressFX collision proxy skin source references a joint outside the skeleton");
            if (source.weight <= 0.0f)
                continue;
            perVertex[vertex].influences.push_back(source);
            weightSum += source.weight;
            maxJoint = std::max(maxJoint, source.joint);
        }
        if (perVertex[vertex].influences.empty() || !(weightSum > 0.0f))
        {
            if (policy == TressfxSkinTransferPolicy::RecordUnmatched)
            {
                perVertex[vertex].influences.clear();
                unmatchedVertices.push_back(static_cast<uint32_t>(vertex));
                continue;
            }
            throw std::runtime_error(
                "TressFX collision proxy nearest body vertex " +
                std::to_string(nearest) + " has no positive skin influence");
        }
        // 源权重和可能带 1e-8 级误差；这里归一化后再交给 GPU，保证 LBS 是
        // 凸组合。影响数量保留全部（CSR 布局没有 4 骨上限）。
        for (SdfProxySkinSourceInfluence& influence : perVertex[vertex].influences)
            influence.weight /= weightSum;
        maxInfluences = std::max(maxInfluences,
            static_cast<uint32_t>(perVertex[vertex].influences.size()));
    }

    SdfProxySkinTransfer transfer;
    transfer.vertexCount = static_cast<uint32_t>(proxyPositions.size());
    transfer.maxInfluences = maxInfluences;
    transfer.jointCount = body.jointCount > 0 ? body.jointCount : maxJoint + 1;
    transfer.maxNearestDistanceCm = maxDistance;
    transfer.uniqueBodyVertices = static_cast<uint32_t>(usedBodyVertices.size());
    transfer.unmatchedVertices = std::move(unmatchedVertices);

    const size_t header = proxyPositions.size() + 1;
    size_t pairCount = 0;
    for (const ProxyInfluences& entry : perVertex)
        pairCount += entry.influences.size();
    transfer.words.reserve(header + pairCount * 2);
    // CSR 头存绝对 word 下标，与 Animation::PackSkinInfluences 的布局一致。
    size_t cursor = 0;
    for (const ProxyInfluences& entry : perVertex)
    {
        transfer.words.push_back(static_cast<uint32_t>(header + 2 * cursor));
        cursor += entry.influences.size();
    }
    transfer.words.push_back(static_cast<uint32_t>(header + 2 * cursor));
    for (const ProxyInfluences& entry : perVertex)
    {
        for (const SdfProxySkinSourceInfluence& influence : entry.influences)
        {
            transfer.words.push_back(influence.joint);
            uint32_t weightBits = 0;
            static_assert(sizeof(weightBits) == sizeof(influence.weight));
            std::memcpy(&weightBits, &influence.weight, sizeof(weightBits));
            transfer.words.push_back(weightBits);
        }
    }
    return transfer;
}

Eigen::Vector3f TressfxProxySkinnedPosition(
    const SdfProxySkinTransfer& transfer,
    uint32_t vertex,
    const std::vector<Eigen::Matrix4f>& jointMatrices,
    const Eigen::Vector3f& bindPosition)
{
    if (vertex + 1 >= transfer.words.size() ||
        transfer.words[vertex] > transfer.words[vertex + 1])
        throw std::out_of_range("TressFX collision proxy skin transfer is out of range");
    const uint32_t begin = transfer.words[vertex];
    const uint32_t end = transfer.words[vertex + 1];
    Eigen::Vector3f skinned = Eigen::Vector3f::Zero();
    for (uint32_t word = begin; word + 1 < end; word += 2)
    {
        const uint32_t joint = transfer.words[word];
        if (joint >= jointMatrices.size())
            throw std::out_of_range(
                "TressFX collision proxy references a joint outside the skin pose");
        float weight = 0.0f;
        const uint32_t weightBits = transfer.words[word + 1];
        std::memcpy(&weight, &weightBits, sizeof(weight));
        skinned += weight *
            (jointMatrices[joint] * bindPosition.homogeneous()).head<3>();
    }
    return skinned;
}

} // namespace VL::Hair
