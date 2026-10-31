#include "hair/tressfxRootCloud.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <stdexcept>

namespace VL::Hair
{
namespace
{

constexpr float kDirectionEpsilon = 1.0e-8f;
constexpr size_t kNormalNeighborLimit = 24;

Eigen::Vector3f SafeNormalize(const Eigen::Vector3f& value)
{
    const float length = value.norm();
    if (!std::isfinite(length) || length <= kDirectionEpsilon) return Eigen::Vector3f::Zero();
    return value / length;
}

float MaxScale(const Eigen::Matrix4f& localToWorld)
{
    const Eigen::Matrix3f linear = localToWorld.block<3, 3>(0, 0);
    return std::max(1.0e-6f,
        std::max(linear.col(0).norm(), std::max(linear.col(1).norm(), linear.col(2).norm())));
}

} // namespace

std::shared_ptr<const TressfxRootCloud> TressfxRootCloud::Build(
    const std::vector<Eigen::Vector3f>& rootPositions,
    const std::vector<Eigen::Vector3f>& growthDirections)
{
    if (!growthDirections.empty() && growthDirections.size() != rootPositions.size())
        throw std::invalid_argument("TressFX root cloud growth directions must match root count");

    auto cloud = std::shared_ptr<TressfxRootCloud>(new TressfxRootCloud());
    cloud->roots_.reserve(rootPositions.size());
    for (size_t index = 0; index < rootPositions.size(); ++index)
    {
        TressfxRootSample root;
        root.position = rootPositions[index];
        if (!root.position.allFinite())
            throw std::invalid_argument("TressFX root cloud contains a non-finite position");
        if (!growthDirections.empty())
            root.growthDirection = SafeNormalize(growthDirections[index]);
        cloud->roots_.push_back(root);
    }
    cloud->BuildGrid();
    cloud->EstimateNormals();
    return cloud;
}

void TressfxRootCloud::BuildGrid()
{
    if (roots_.empty())
    {
        cellSize_ = 1.0f;
        cellCounts_.setOnes();
        cellOffsets_.assign(2, 0u);
        return;
    }

    boundsMin_ = roots_.front().position;
    boundsMax_ = roots_.front().position;
    centroid_.setZero();
    for (const TressfxRootSample& root : roots_)
    {
        boundsMin_ = boundsMin_.cwiseMin(root.position);
        boundsMax_ = boundsMax_.cwiseMax(root.position);
        centroid_ += root.position;
    }
    centroid_ /= static_cast<float>(roots_.size());

    const Eigen::Vector3f extent = boundsMax_ - boundsMin_;
    const float diagonal = extent.norm();
    // 平均约 1~2 个 root / cell；下限避免极小资产退化成巨量空 cell。
    cellSize_ = std::max(0.01f,
        diagonal * std::cbrt(1.5f / static_cast<float>(roots_.size())));
    if (!std::isfinite(cellSize_) || cellSize_ <= 0.0f) cellSize_ = 1.0f;

    for (int axis = 0; axis < 3; ++axis)
    {
        const int64_t count = static_cast<int64_t>(
            std::floor((boundsMax_[axis] - boundsMin_[axis]) / cellSize_)) + 1;
        cellCounts_[axis] = static_cast<int>(std::clamp<int64_t>(count, 1, 512));
    }

    const size_t cellCount = static_cast<size_t>(cellCounts_.x()) *
        static_cast<size_t>(cellCounts_.y()) * static_cast<size_t>(cellCounts_.z());
    std::vector<uint32_t> counts(cellCount, 0u);
    std::vector<uint32_t> rootCells(roots_.size(), 0u);
    for (size_t index = 0; index < roots_.size(); ++index)
    {
        const uint32_t cell = CellIndex(
            static_cast<int>(CellCoordinate(roots_[index].position, 0)),
            static_cast<int>(CellCoordinate(roots_[index].position, 1)),
            static_cast<int>(CellCoordinate(roots_[index].position, 2)));
        rootCells[index] = cell;
        ++counts[cell];
    }
    cellOffsets_.assign(cellCount + 1, 0u);
    for (size_t cell = 0; cell < cellCount; ++cell)
        cellOffsets_[cell + 1] = cellOffsets_[cell] + counts[cell];
    std::vector<uint32_t> cursor = cellOffsets_;
    cellRoots_.resize(roots_.size());
    for (size_t index = 0; index < roots_.size(); ++index)
        cellRoots_[cursor[rootCells[index]]++] = static_cast<uint32_t>(index);
}

int64_t TressfxRootCloud::CellCoordinate(const Eigen::Vector3f& point, int axis) const
{
    const int64_t coordinate = static_cast<int64_t>(
        std::floor((point[axis] - boundsMin_[axis]) / cellSize_));
    return std::clamp<int64_t>(coordinate, 0, cellCounts_[axis] - 1);
}

uint32_t TressfxRootCloud::CellIndex(int x, int y, int z) const
{
    return static_cast<uint32_t>(
        (static_cast<size_t>(z) * static_cast<size_t>(cellCounts_.y()) +
            static_cast<size_t>(y)) * static_cast<size_t>(cellCounts_.x()) +
        static_cast<size_t>(x));
}

std::vector<uint32_t> TressfxRootCloud::GatherCellRange(
    const Eigen::Vector3f& minimum, const Eigen::Vector3f& maximum) const
{
    std::vector<uint32_t> result;
    if (roots_.empty()) return result;
    const Eigen::Vector3f clampedMin = minimum.cwiseMax(boundsMin_);
    const Eigen::Vector3f clampedMax = maximum.cwiseMin(boundsMax_);
    if ((clampedMin.array() > clampedMax.array()).any()) return result;

    const int minX = static_cast<int>(CellCoordinate(clampedMin, 0));
    const int minY = static_cast<int>(CellCoordinate(clampedMin, 1));
    const int minZ = static_cast<int>(CellCoordinate(clampedMin, 2));
    const int maxX = static_cast<int>(CellCoordinate(clampedMax, 0));
    const int maxY = static_cast<int>(CellCoordinate(clampedMax, 1));
    const int maxZ = static_cast<int>(CellCoordinate(clampedMax, 2));
    for (int z = minZ; z <= maxZ; ++z)
        for (int y = minY; y <= maxY; ++y)
            for (int x = minX; x <= maxX; ++x)
            {
                const uint32_t cell = CellIndex(x, y, z);
                const uint32_t begin = cellOffsets_[cell];
                const uint32_t end = cellOffsets_[cell + 1];
                for (uint32_t slot = begin; slot < end; ++slot)
                {
                    result.push_back(cellRoots_[slot]);
                }
            }
    // 球查询的候选上限在 QuerySphere 完成距离过滤与排序后再应用；任何在过滤前
    // 的截断都会让 40k guide 只保留网格顺序最前面的根点。
    return result;
}

std::vector<uint32_t> TressfxRootCloud::QueryRay(
    const Eigen::Vector3f& worldOrigin,
    const Eigen::Vector3f& worldDirection,
    float maxDistanceWorld,
    float radiusWorld,
    const Eigen::Matrix4f& localToWorld,
    size_t maxCandidates) const
{
    if (roots_.empty()) return {};
    const Eigen::Matrix4f worldToLocal = localToWorld.inverse();
    const float scale = MaxScale(localToWorld);
    const Eigen::Vector3f origin = (worldToLocal * Eigen::Vector4f(
        worldOrigin.x(), worldOrigin.y(), worldOrigin.z(), 1.0f)).head<3>();
    const Eigen::Vector3f direction = SafeNormalize(
        worldToLocal.block<3, 3>(0, 0) * worldDirection);
    if (direction.isZero()) return {};
    const float maxDistance = std::min(
        maxDistanceWorld > 0.0f ? maxDistanceWorld / scale
                                : std::numeric_limits<float>::max(),
        1.0e7f);
    const float radius = std::max(0.0f, radiusWorld / scale);

    // 先把射线裁剪到点云 AABB（再向外扩一个 tube 半径），避免
    // maxRayDistanceWorld=100000 时把整份点云都当成候选。
    float tNear = 0.0f;
    float tFar = maxDistance;
    for (int axis = 0; axis < 3; ++axis)
    {
        const float minimum = boundsMin_[axis] - radius;
        const float maximum = boundsMax_[axis] + radius;
        if (std::abs(direction[axis]) <= kDirectionEpsilon)
        {
            if (origin[axis] < minimum || origin[axis] > maximum) return {};
            continue;
        }
        float entry = (minimum - origin[axis]) / direction[axis];
        float exit = (maximum - origin[axis]) / direction[axis];
        if (entry > exit) std::swap(entry, exit);
        tNear = std::max(tNear, entry);
        tFar = std::min(tFar, exit);
        if (tNear > tFar) return {};
    }
    if (tFar < 0.0f) return {};

    // 主轴 DDA：沿方向绝对值最大的轴逐 slice 前进，每个 slice 只枚举
    // tube 在该 slice 内的垂直包围矩形。相比整条 ray 的 AABB，这只访问
    // 真正可能与 ray tube 相交的 cell，也不会重复拷贝同一根 root。
    int majorAxis = 0;
    for (int axis = 1; axis < 3; ++axis)
        if (std::abs(direction[axis]) > std::abs(direction[majorAxis]))
            majorAxis = axis;
    const int perpendicularAxisA = (majorAxis + 1) % 3;
    const int perpendicularAxisB = (majorAxis + 2) % 3;

    struct Candidate
    {
        uint32_t index;
        float depth;
        float perpendicularSquared;
    };
    std::vector<Candidate> accepted;
    const float radiusSquared = radius * radius;
    // radius=0 时保留恰好落在射线上的 root；浮点残差不应把它们排除。
    constexpr float kTubeSlack = 1.0e-6f;
    const int firstSlice = static_cast<int>(CellCoordinate(
        origin + direction * tNear, majorAxis));
    const int lastSlice = static_cast<int>(CellCoordinate(
        origin + direction * tFar, majorAxis));
    // tube 沿主轴也有 radius 的投影：中心线只擦到相邻 slice 时，附近的
    // root 仍可能落在 tube 内，因此 slice 范围要向外扩。
    // 上限取主轴 cell 数即可覆盖整个网格，避免极端 radius/cellSize 溢出 int。
    const float expansionCells = std::ceil(radius / cellSize_) + 1.0f;
    const int sliceExpansion = static_cast<int>(std::min(
        expansionCells, static_cast<float>(cellCounts_[majorAxis])));
    const int sliceBegin = std::max(0, std::min(firstSlice, lastSlice) - sliceExpansion);
    const int sliceEnd = std::min(cellCounts_[majorAxis] - 1,
        std::max(firstSlice, lastSlice) + sliceExpansion);
    // root 的深度参数可与其主轴 cell 的切片中心相差 radius / |d_major|；
    // 用扩展后的 t 区间计算 tube 在两个垂直轴上的跨度。
    const float majorPadding = radius /
        std::max(std::abs(direction[majorAxis]), kDirectionEpsilon);
    const int perpendicularCountA = cellCounts_[perpendicularAxisA];
    const int perpendicularCountB = cellCounts_[perpendicularAxisB];

    for (int slice = sliceBegin; slice <= sliceEnd; ++slice)
    {
        const float sliceMin = boundsMin_[majorAxis] + cellSize_ * static_cast<float>(slice);
        const float sliceMax = sliceMin + cellSize_;
        float sliceNear = (sliceMin - origin[majorAxis]) / direction[majorAxis];
        float sliceFar = (sliceMax - origin[majorAxis]) / direction[majorAxis];
        if (sliceNear > sliceFar) std::swap(sliceNear, sliceFar);
        sliceNear -= majorPadding;
        sliceFar += majorPadding;
        sliceNear = std::max(sliceNear, tNear);
        sliceFar = std::min(sliceFar, tFar);
        if (sliceNear > sliceFar) continue;

        const float positionA0 = origin[perpendicularAxisA] +
            direction[perpendicularAxisA] * sliceNear;
        const float positionA1 = origin[perpendicularAxisA] +
            direction[perpendicularAxisA] * sliceFar;
        const float positionB0 = origin[perpendicularAxisB] +
            direction[perpendicularAxisB] * sliceNear;
        const float positionB1 = origin[perpendicularAxisB] +
            direction[perpendicularAxisB] * sliceFar;

        int cellA0 = static_cast<int>(std::floor(
            (std::min(positionA0, positionA1) - radius - boundsMin_[perpendicularAxisA]) /
            cellSize_));
        int cellA1 = static_cast<int>(std::floor(
            (std::max(positionA0, positionA1) + radius - boundsMin_[perpendicularAxisA]) /
            cellSize_));
        int cellB0 = static_cast<int>(std::floor(
            (std::min(positionB0, positionB1) - radius - boundsMin_[perpendicularAxisB]) /
            cellSize_));
        int cellB1 = static_cast<int>(std::floor(
            (std::max(positionB0, positionB1) + radius - boundsMin_[perpendicularAxisB]) /
            cellSize_));
        if (cellA1 < 0 || cellA0 >= perpendicularCountA ||
            cellB1 < 0 || cellB0 >= perpendicularCountB)
            continue;
        cellA0 = std::max(cellA0, 0);
        cellA1 = std::min(cellA1, perpendicularCountA - 1);
        cellB0 = std::max(cellB0, 0);
        cellB1 = std::min(cellB1, perpendicularCountB - 1);

        for (int cellB = cellB0; cellB <= cellB1; ++cellB)
            for (int cellA = cellA0; cellA <= cellA1; ++cellA)
            {
                int coordinate[3] = {0, 0, 0};
                coordinate[majorAxis] = slice;
                coordinate[perpendicularAxisA] = cellA;
                coordinate[perpendicularAxisB] = cellB;
                const uint32_t cell = CellIndex(
                    coordinate[0], coordinate[1], coordinate[2]);
                const uint32_t begin = cellOffsets_[cell];
                const uint32_t end = cellOffsets_[cell + 1];
                for (uint32_t slot = begin; slot < end; ++slot)
                {
                    const uint32_t index = cellRoots_[slot];
                    const Eigen::Vector3f offset = roots_[index].position - origin;
                    const float depth = offset.dot(direction);
                    if (depth < 0.0f || depth > maxDistance) continue;
                    const float perpendicularSquared =
                        std::max(0.0f, offset.squaredNorm() - depth * depth);
                    if (perpendicularSquared > radiusSquared + kTubeSlack) continue;
                    accepted.push_back({index, depth, perpendicularSquared});
                }
            }
    }
    std::sort(accepted.begin(), accepted.end(), [](const Candidate& left, const Candidate& right)
    {
        if (left.depth != right.depth) return left.depth < right.depth;
        return left.index < right.index;
    });
    if (maxCandidates != 0 && accepted.size() > maxCandidates)
        accepted.resize(maxCandidates);
    std::vector<uint32_t> result;
    result.reserve(accepted.size());
    for (const Candidate& candidate : accepted) result.push_back(candidate.index);
    return result;
}

std::vector<uint32_t> TressfxRootCloud::QuerySphere(
    const Eigen::Vector3f& worldCenter,
    float radiusWorld,
    const Eigen::Matrix4f& localToWorld,
    size_t maxCandidates) const
{
    if (roots_.empty() || radiusWorld <= 0.0f) return {};
    const Eigen::Matrix4f worldToLocal = localToWorld.inverse();
    const float scale = MaxScale(localToWorld);
    const Eigen::Vector3f center = (worldToLocal * Eigen::Vector4f(
        worldCenter.x(), worldCenter.y(), worldCenter.z(), 1.0f)).head<3>();
    const float radius = radiusWorld / scale;
    const float radiusSquared = radius * radius;
    const std::vector<uint32_t> candidates = GatherCellRange(
        center - Eigen::Vector3f::Constant(radius),
        center + Eigen::Vector3f::Constant(radius));

    struct Candidate
    {
        uint32_t index;
        float distanceSquared;
    };
    std::vector<Candidate> accepted;
    accepted.reserve(candidates.size());
    for (uint32_t index : candidates)
    {
        const float distanceSquared = (roots_[index].position - center).squaredNorm();
        if (distanceSquared > radiusSquared) continue;
        accepted.push_back({index, distanceSquared});
    }
    std::sort(accepted.begin(), accepted.end(), [](const Candidate& left, const Candidate& right)
    {
        if (left.distanceSquared != right.distanceSquared)
            return left.distanceSquared < right.distanceSquared;
        return left.index < right.index;
    });
    if (maxCandidates != 0 && accepted.size() > maxCandidates)
        accepted.resize(maxCandidates);
    std::vector<uint32_t> result;
    result.reserve(accepted.size());
    for (const Candidate& candidate : accepted) result.push_back(candidate.index);
    return result;
}

void TressfxRootCloud::EstimateNormals()
{
    if (roots_.empty()) return;
    const float neighborRadius = cellSize_ * 2.5f;
    for (TressfxRootSample& root : roots_)
    {
        const std::vector<uint32_t> candidates = QuerySphere(
            root.position, neighborRadius, Eigen::Matrix4f::Identity().eval(),
            kNormalNeighborLimit * 4);
        // QuerySphere 接收局部点云，但这里已经是局部空间；直接按距离筛选并保留最近邻。
        std::vector<std::pair<float, uint32_t>> nearest;
        nearest.reserve(candidates.size());
        for (uint32_t index : candidates)
        {
            const float distance = (roots_[index].position - root.position).squaredNorm();
            nearest.emplace_back(distance, index);
        }
        std::sort(nearest.begin(), nearest.end());
        if (nearest.size() > kNormalNeighborLimit) nearest.resize(kNormalNeighborLimit);
        if (nearest.size() < 3) continue;

        Eigen::Vector3f mean = Eigen::Vector3f::Zero();
        for (const auto& entry : nearest) mean += roots_[entry.second].position;
        mean /= static_cast<float>(nearest.size());
        Eigen::Matrix3f covariance = Eigen::Matrix3f::Zero();
        for (const auto& entry : nearest)
        {
            const Eigen::Vector3f delta = roots_[entry.second].position - mean;
            covariance += delta * delta.transpose();
        }
        covariance /= static_cast<float>(nearest.size());
        Eigen::SelfAdjointEigenSolver<Eigen::Matrix3f> solver(covariance);
        if (solver.info() != Eigen::Success) continue;
        const Eigen::Vector3f eigenvalues = solver.eigenvalues();
        if (!eigenvalues.allFinite()) continue;
        const float scale = std::max(1.0e-12f, std::abs(eigenvalues[2]));
        // 共线根点没有可定义的头皮法线，留给选择器走 -viewDir 回退。
        if (std::abs(eigenvalues[1]) <= 1.0e-6f * scale) continue;
        Eigen::Vector3f normal = solver.eigenvectors().col(0);
        if (!normal.allFinite() || normal.norm() <= kDirectionEpsilon) continue;
        normal.normalize();
        // 局部根点云质心只用于消除 PCA 法线的符号二义性；凹陷区域仍是近似。
        if (normal.dot(root.position - centroid_) < 0.0f) normal = -normal;
        root.normal = normal;
        root.hasNormal = true;
    }
}

} // namespace VL::Hair
