#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <vector>

#include <Eigen/Dense>

namespace VL::Hair
{

// 每根 guide 一条的只读发根缓存（model/head-local 空间），只含几何派生量。
// normal 由导入期邻域 PCA 估计并按“远离根点云质心”定向；失败时保持零向量，
// 由选择器回退到 -viewDir，绝不把 growthDirection 当作头皮法线。
// 分组号不在点云里：组编辑只替换 TressfxGroupBinding，几何缓存保持同一份。
struct TressfxRootSample
{
    Eigen::Vector3f position = Eigen::Vector3f::Zero();
    Eigen::Vector3f growthDirection = Eigen::Vector3f::Zero();
    Eigen::Vector3f normal = Eigen::Vector3f::Zero();
    bool hasNormal = false;
};

// 发根点云的 3D uniform grid。导入/预览准备期构建一次，笔刷每次采样只查询
// ray tube / sphere 邻域，不在采样时重算法线或全量扫描 guide。
class TressfxRootCloud
{
public:
    static std::shared_ptr<const TressfxRootCloud> Build(
        const std::vector<Eigen::Vector3f>& rootPositions,
        const std::vector<Eigen::Vector3f>& growthDirections);

    const std::vector<TressfxRootSample>& Roots() const { return roots_; }
    size_t RootCount() const { return roots_.size(); }
    // 局部空间网格边长（cm）。选择器的自动薄层厚度/查询半径以它为单位。
    float CellSize() const { return cellSize_; }

    // 与射线管相交的候选 guide；按射线参数 depth 升序。localToWorld 只用于
    // 把世界空间 ray/radius 映射回点云局部空间，点云本身不被逐帧复制。
    // 遍历沿主轴的 grid DDA 走，只枚举与 ray tube 相交的 cell；maxCandidates
    // 只裁剪过滤/排序后的结果，不参与候选收集。
    std::vector<uint32_t> QueryRay(
        const Eigen::Vector3f& worldOrigin,
        const Eigen::Vector3f& worldDirection,
        float maxDistanceWorld,
        float radiusWorld,
        const Eigen::Matrix4f& localToWorld,
        size_t maxCandidates) const;

    // 世界空间球查询；用于选择器在笔刷平面附近取候选。
    std::vector<uint32_t> QuerySphere(
        const Eigen::Vector3f& worldCenter,
        float radiusWorld,
        const Eigen::Matrix4f& localToWorld,
        size_t maxCandidates) const;

private:
    struct Cell
    {
        uint32_t begin = 0;
        uint32_t count = 0;
    };

    void BuildGrid();
    void EstimateNormals();
    int64_t CellCoordinate(const Eigen::Vector3f& point, int axis) const;
    uint32_t CellIndex(int x, int y, int z) const;
    std::vector<uint32_t> GatherCellRange(
        const Eigen::Vector3f& min, const Eigen::Vector3f& max) const;

    std::vector<TressfxRootSample> roots_;
    Eigen::Vector3f boundsMin_ = Eigen::Vector3f::Zero();
    Eigen::Vector3f boundsMax_ = Eigen::Vector3f::Zero();
    Eigen::Vector3f centroid_ = Eigen::Vector3f::Zero();
    Eigen::Vector3i cellCounts_ = Eigen::Vector3i::Zero();
    float cellSize_ = 1.0f;
    // 计数排序：cellOffsets_ 长度是 cell 数 + 1；cellRoots_ 是 root 索引。
    std::vector<uint32_t> cellOffsets_;
    std::vector<uint32_t> cellRoots_;
};

} // namespace VL::Hair
