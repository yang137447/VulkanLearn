#include "hair/tressfxSdfGrid.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <stdexcept>

namespace VL::Hair
{
namespace
{
bool FiniteVector(const Eigen::Vector3f& value)
{
    return std::isfinite(value.x()) && std::isfinite(value.y()) && std::isfinite(value.z());
}

RuntimeError GridError(std::string code, std::string message)
{
    return MakeRuntimeError(std::move(code), std::move(message));
}

std::uint32_t CellFromGridCoordinate(float value, std::uint32_t count)
{
    // 先在 float 里 clamp 再转整数：OBJ 里可能有大到 long 无法表示的有限坐标，
    // 直接 static_cast 是未定义行为。域外的线段在上层已经提前跳过。
    const float floored = std::floor(value);
    if (!(floored > 0.0f))
        return 0;
    const float last = static_cast<float>(count - 1);
    if (floored >= last)
        return count - 1;
    return static_cast<std::uint32_t>(floored);
}

// splat 的顶点变换只有一个入口：有逐顶点权重时走 LBS，否则复用根骨矩阵。
// GPU tressfxSimSdfSplat.comp 用 sdf.counts.y 做同一个分叉。
Eigen::Vector3f TransformSdfProxyVertex(const Eigen::Matrix4f& skinning,
    const SdfProxySkinTransfer* proxySkin,
    const std::vector<Eigen::Matrix4f>* jointMatrices,
    std::uint32_t index, const Eigen::Vector3f& bindPosition)
{
    if (proxySkin != nullptr && jointMatrices != nullptr)
        return TressfxProxySkinnedPosition(*proxySkin, index, *jointMatrices,
            bindPosition);
    return (skinning * bindPosition.homogeneous()).head<3>();
}
}

std::uint32_t TressfxSdfResolutionForCellSize(float cellSize, float bindMaxExtent)
{
    if (!std::isfinite(cellSize) || !(cellSize > 0.0f) ||
        !std::isfinite(bindMaxExtent) || !(bindMaxExtent > 0.0f))
        return TressfxSdfMaxResolution;
    const double requested = std::round(static_cast<double>(bindMaxExtent) / cellSize);
    if (!(requested > static_cast<double>(TressfxSdfMinResolution)))
        return TressfxSdfMinResolution;
    if (requested >= static_cast<double>(TressfxSdfMaxResolution))
        return TressfxSdfMaxResolution;
    return static_cast<std::uint32_t>(requested);
}

RuntimeResult<TressfxSdfLayout> MakeTressfxSdfDomain(const Eigen::Vector3f& boundsMin,
    const Eigen::Vector3f& boundsMax, float margin, std::uint32_t resolution)
{
    if (!FiniteVector(boundsMin) || !FiniteVector(boundsMax))
        return RuntimeResult<TressfxSdfLayout>::Failure(
            GridError("Hair.SdfGridDomain", "SDF domain bounds are not finite"));
    if (!std::isfinite(margin) || !(margin > 0.0f))
        return RuntimeResult<TressfxSdfLayout>::Failure(
            GridError("Hair.SdfGridDomain", "SDF collision margin must be finite and positive"));
    if (resolution < TressfxSdfMinResolution || resolution > TressfxSdfMaxResolution)
        return RuntimeResult<TressfxSdfLayout>::Failure(GridError("Hair.SdfGridDomain",
            "SDF grid resolution must be in [" + std::to_string(TressfxSdfMinResolution) +
                ", " + std::to_string(TressfxSdfMaxResolution) + "]"));

    const Eigen::Vector3f extent = boundsMax - boundsMin;
    // 退化包围盒（所有蒙皮顶点重合，或浮点舍入成负 extent）按最小边长处理：
    // 继续求 cellSize 会除以 0 或得到负的格子尺寸。
    float maxExtent = std::max(extent.x(), std::max(extent.y(), extent.z()));
    if (!(maxExtent > 0.0f))
        maxExtent = TressfxSdfMinDomainExtentCm;

    // 立方体边长要同时覆盖包围盒和 truncation = margin + 2*cellSize，
    // 而 cellSize = side/resolution，因此 side = (maxExtent + 2*margin) /
    // (1 - 4/resolution)。resolution >= 48 时分母 >= 0.9167，不会退化。
    const float denominator = 1.0f - 4.0f / static_cast<float>(resolution);
    const float side = (maxExtent + 2.0f * margin) / denominator;
    if (!std::isfinite(side) || !(side > 0.0f))
        return RuntimeResult<TressfxSdfLayout>::Failure(
            GridError("Hair.SdfGridDomain", "SDF domain extent is not finite"));

    TressfxSdfLayout layout;
    layout.boundsMin = boundsMin;
    layout.boundsMax = boundsMax;
    layout.margin = margin;
    layout.resolution = resolution;
    layout.dims = {resolution, resolution, resolution};
    layout.cellSize = side / static_cast<float>(resolution);
    layout.truncation = margin + 2.0f * layout.cellSize;
    layout.origin = (boundsMin + boundsMax) * 0.5f -
        Eigen::Vector3f::Constant(0.5f * side);
    return RuntimeResult<TressfxSdfLayout>::Success(std::move(layout));
}

RuntimeResult<TressfxSdfLayout> MakeTressfxSdfLayout(const SdfTriangleMesh& proxy,
    float cellSize, float margin)
{
    if (proxy.positions.empty())
        return RuntimeResult<TressfxSdfLayout>::Failure(
            GridError("Hair.SdfGridMesh", "collision proxy has no vertices"));
    if (proxy.indices.empty() || proxy.indices.size() % 3 != 0)
        return RuntimeResult<TressfxSdfLayout>::Failure(
            GridError("Hair.SdfGridMesh", "collision proxy indices must be a non-empty triangle list"));
    if (!std::isfinite(cellSize) || !(cellSize > 0.0f))
        return RuntimeResult<TressfxSdfLayout>::Failure(
            GridError("Hair.SdfGridDomain", "SDF target cell size must be finite and positive"));

    Eigen::Vector3f boundsMin = proxy.positions.front();
    Eigen::Vector3f boundsMax = proxy.positions.front();
    for (std::size_t index = 0; index < proxy.positions.size(); ++index)
    {
        const Eigen::Vector3f& position = proxy.positions[index];
        if (!FiniteVector(position))
            return RuntimeResult<TressfxSdfLayout>::Failure(GridError("Hair.SdfGridMesh",
                "collision proxy vertex " + std::to_string(index) + " is not finite"));
        boundsMin = boundsMin.cwiseMin(position);
        boundsMax = boundsMax.cwiseMax(position);
    }
    for (std::size_t index = 0; index < proxy.indices.size(); ++index)
    {
        if (proxy.indices[index] >= proxy.positions.size())
            return RuntimeResult<TressfxSdfLayout>::Failure(GridError("Hair.SdfGridMesh",
                "collision proxy triangle references a vertex outside the position list"));
    }

    const Eigen::Vector3f extent = boundsMax - boundsMin;
    const float maxExtent = std::max(extent.x(), std::max(extent.y(), extent.z()));
    const std::uint32_t resolution = TressfxSdfResolutionForCellSize(cellSize, maxExtent);
    return MakeTressfxSdfDomain(boundsMin, boundsMax, margin, resolution);
}

TressfxSdfParams MakeTressfxSdfParams(
    const TressfxSdfLayout& layout, std::uint32_t triangleCount, bool proxySkinning)
{
    TressfxSdfParams params;
    params.originCell = Eigen::Vector4f(layout.origin.x(), layout.origin.y(), layout.origin.z(),
        layout.cellSize);
    params.dims = {layout.dims[0], layout.dims[1], layout.dims[2], layout.CellCount()};
    // counts.y 是 splat 的分支开关：1 = 逐顶点多骨 LBS，0 = 单骨根矩阵回退。
    params.counts = {triangleCount, proxySkinning ? 1u : 0u, 0, 0};
    params.range = Eigen::Vector4f(layout.margin, layout.truncation, 0.0f, 0.0f);
    return params;
}

float SignedDistancePointToTriangle(const Eigen::Vector3f& point, const Eigen::Vector3f& a,
    const Eigen::Vector3f& b, const Eigen::Vector3f& c)
{
    // 与 sdfBuilder 共用同一份 Ericson 最近点分支；符号沿用 TressFX
    // SignedDistancePointToTriangle 的判定：dot(p - x0, cross(x1-x0, x2-x0)) < 0
    // 为负。代理保留身体网格绕序，因此外侧为正、内侧为负。
    const float distance = DistancePointToTriangle(point, a, b, c);
    const Eigen::Vector3f normal = (b - a).cross(c - a);
    return (point - a).dot(normal) < 0.0f ? -distance : distance;
}

void ClearTressfxSdfGrid(const TressfxSdfLayout& layout, std::vector<std::uint32_t>& cells)
{
    cells.assign(layout.CellCount(), TressfxSdfInitialBits);
}

void SplatTressfxSdfGrid(const TressfxSdfLayout& layout, const SdfTriangleMesh& proxy,
    const Eigen::Matrix4f& skinning, std::vector<std::uint32_t>& cells,
    const SdfProxySkinTransfer* proxySkin,
    const std::vector<Eigen::Matrix4f>* jointMatrices)
{
    if (cells.size() != layout.CellCount())
        throw std::invalid_argument("SDF grid splat requires a cleared grid of the layout size");
    if (proxy.indices.size() % 3 != 0)
        throw std::invalid_argument("SDF grid splat requires a triangle index list");
    // 逐顶点多骨路径要求两份数据同时存在；只有其中一份时按单骨刚性矩阵处理，
    // 与 shader 的 counts.y 门控一致。
    const bool useProxySkin = proxySkin != nullptr && jointMatrices != nullptr;
    if (useProxySkin && proxySkin->vertexCount != proxy.positions.size())
        throw std::invalid_argument(
            "SDF grid proxy skin transfer does not match the proxy vertex count");

    const std::uint32_t dimsX = layout.dims[0];
    const std::uint32_t dimsY = layout.dims[1];
    const std::uint32_t dimsZ = layout.dims[2];
    const float truncation = layout.truncation;
    const Eigen::Vector3f expansion = Eigen::Vector3f::Constant(truncation);
    for (std::size_t triangle = 0; triangle + 2 < proxy.indices.size(); triangle += 3)
    {
        const std::uint32_t i0 = proxy.indices[triangle + 0];
        const std::uint32_t i1 = proxy.indices[triangle + 1];
        const std::uint32_t i2 = proxy.indices[triangle + 2];
        if (i0 >= proxy.positions.size() || i1 >= proxy.positions.size() ||
            i2 >= proxy.positions.size())
            throw std::invalid_argument("SDF grid splat triangle index is out of range");
        // 与 splat shader 相同：先按骨骼变换代理三角形。逐顶点路径使用
        // 转移后的 LBS 权重，未声明时保持根点同一矩阵。
        const Eigen::Vector3f a = TransformSdfProxyVertex(skinning, proxySkin,
            jointMatrices, i0, proxy.positions[i0]);
        const Eigen::Vector3f b = TransformSdfProxyVertex(skinning, proxySkin,
            jointMatrices, i1, proxy.positions[i1]);
        const Eigen::Vector3f c = TransformSdfProxyVertex(skinning, proxySkin,
            jointMatrices, i2, proxy.positions[i2]);
        if (!FiniteVector(a) || !FiniteVector(b) || !FiniteVector(c))
            throw std::invalid_argument("SDF grid splat produced a non-finite vertex");

        const Eigen::Vector3f boundsMin = a.cwiseMin(b).cwiseMin(c) - expansion;
        const Eigen::Vector3f boundsMax = a.cwiseMax(b).cwiseMax(c) + expansion;
        const Eigen::Vector3f gridMin = (boundsMin - layout.origin) / layout.cellSize;
        const Eigen::Vector3f gridMax = (boundsMax - layout.origin) / layout.cellSize;
        if (gridMax.x() < 0.0f || gridMax.y() < 0.0f || gridMax.z() < 0.0f ||
            gridMin.x() > static_cast<float>(dimsX - 1) ||
            gridMin.y() > static_cast<float>(dimsY - 1) ||
            gridMin.z() > static_cast<float>(dimsZ - 1))
            continue;
        const std::uint32_t xBegin = CellFromGridCoordinate(gridMin.x(), dimsX);
        const std::uint32_t yBegin = CellFromGridCoordinate(gridMin.y(), dimsY);
        const std::uint32_t zBegin = CellFromGridCoordinate(gridMin.z(), dimsZ);
        const std::uint32_t xEnd = CellFromGridCoordinate(gridMax.x(), dimsX);
        const std::uint32_t yEnd = CellFromGridCoordinate(gridMax.y(), dimsY);
        const std::uint32_t zEnd = CellFromGridCoordinate(gridMax.z(), dimsZ);

        for (std::uint32_t z = zBegin; z <= zEnd; ++z)
        {
            for (std::uint32_t y = yBegin; y <= yEnd; ++y)
            {
                std::size_t index = static_cast<std::size_t>(xBegin) +
                    static_cast<std::size_t>(dimsX) *
                        (static_cast<std::size_t>(y) + static_cast<std::size_t>(dimsY) * z);
                for (std::uint32_t x = xBegin; x <= xEnd; ++x, ++index)
                {
                    const Eigen::Vector3f center = layout.CellCenter(x, y, z);
                    const float distance = SignedDistancePointToTriangle(center, a, b, c);
                    // 只存 ±truncation 带内的值：AABB 的角可能让一个真实距离
                    // 超过 band 的三角形覆盖到该格；若把它的另一侧符号写进来，
                    // 会压过带内最近的表面（球面/凹面尤其明显）。TressFX 的
                    // 固定格偏移不会遇到这么大的角，这里按 band 显式过滤。
                    if (!(std::abs(distance) <= truncation))
                        continue;
                    const std::uint32_t bits = TressfxSdfFloatFlip(distance);
                    // FloatFlip 后的位模式在带内按 |d| 排序；比位模式与 shader
                    // 的 atomicMin 同义。
                    if (bits < cells[index])
                        cells[index] = bits;
                }
            }
        }
    }
}

std::vector<std::uint32_t> BuildTressfxSdfGrid(const TressfxSdfLayout& layout,
    const SdfTriangleMesh& proxy, const Eigen::Matrix4f& skinning,
    const SdfProxySkinTransfer* proxySkin,
    const std::vector<Eigen::Matrix4f>* jointMatrices)
{
    std::vector<std::uint32_t> cells;
    ClearTressfxSdfGrid(layout, cells);
    SplatTressfxSdfGrid(layout, proxy, skinning, cells, proxySkin, jointMatrices);
    return cells;
}

float SampleTressfxSdfValue(const TressfxSdfLayout& layout,
    const std::vector<std::uint32_t>& cells, const Eigen::Vector3f& position)
{
    if (cells.size() != layout.CellCount() || layout.cellSize <= 0.0f)
        return TressfxSdfOutOfRangeDistance;

    const Eigen::Vector3f grid = (position - layout.origin) / layout.cellSize;
    for (int axis = 0; axis < 3; ++axis)
    {
        if (grid[axis] < 0.0f || grid[axis] > static_cast<float>(layout.dims[axis]))
            return TressfxSdfOutOfRangeDistance;
    }

    // 与 sdfBuilder::SampleSdf 相同的“相邻格子中心”锚点：坐标先减半格，
    // base 落在 [0, dims-1]，fraction 落在 [0,1]，插值永远是凸组合，不会
    // 外插越过 truncation 带。
    std::uint32_t base[3] = {0, 0, 0};
    float fraction[3] = {0.0f, 0.0f, 0.0f};
    for (int axis = 0; axis < 3; ++axis)
    {
        const float coordinate = grid[axis] - 0.5f;
        const float last = static_cast<float>(layout.dims[axis] - 1);
        if (coordinate <= 0.0f)
        {
            base[axis] = 0;
            fraction[axis] = 0.0f;
        }
        else if (coordinate >= last)
        {
            base[axis] = layout.dims[axis] - 1;
            fraction[axis] = 0.0f;
        }
        else
        {
            base[axis] = static_cast<std::uint32_t>(coordinate);
            fraction[axis] = coordinate - static_cast<float>(base[axis]);
        }
    }
    const std::uint32_t high[3] = {
        std::min(base[0] + 1, layout.dims[0] - 1),
        std::min(base[1] + 1, layout.dims[1] - 1),
        std::min(base[2] + 1, layout.dims[2] - 1)};
    float values[2][2][2];
    for (int oz = 0; oz < 2; ++oz)
    {
        for (int oy = 0; oy < 2; ++oy)
        {
            for (int ox = 0; ox < 2; ++ox)
            {
                const std::uint32_t x = ox ? high[0] : base[0];
                const std::uint32_t y = oy ? high[1] : base[1];
                const std::uint32_t z = oz ? high[2] : base[2];
                const std::size_t index = static_cast<std::size_t>(x) +
                    static_cast<std::size_t>(layout.dims[0]) *
                        (static_cast<std::size_t>(y) + static_cast<std::size_t>(layout.dims[1]) * z);
                const std::uint32_t bits = cells[index];
                // TressFX 的 8 角点哨兵检查：任一角点未初始化就不跨该区域插值。
                if (bits == TressfxSdfInitialBits)
                    return TressfxSdfOutOfRangeDistance;
                values[oz][oy][ox] = TressfxSdfIFloatFlip(bits);
            }
        }
    }

    const float fx = fraction[0];
    const float fy = fraction[1];
    const float fz = fraction[2];
    const float lower00 = values[0][0][0] * (1.0f - fx) + values[0][0][1] * fx;
    const float lower10 = values[0][1][0] * (1.0f - fx) + values[0][1][1] * fx;
    const float upper00 = values[1][0][0] * (1.0f - fx) + values[1][0][1] * fx;
    const float upper10 = values[1][1][0] * (1.0f - fx) + values[1][1][1] * fx;
    const float lower = lower00 * (1.0f - fy) + lower10 * fy;
    const float upper = upper00 * (1.0f - fy) + upper10 * fy;
    return lower * (1.0f - fz) + upper * fz;
}

Eigen::Vector3f TressfxSdfGradient(const TressfxSdfLayout& layout,
    const std::vector<std::uint32_t>& cells, const Eigen::Vector3f& position)
{
    const float halfStep = 0.5f * layout.cellSize;
    const Eigen::Vector3f dx(halfStep, 0.0f, 0.0f);
    const Eigen::Vector3f dy(0.0f, halfStep, 0.0f);
    const Eigen::Vector3f dz(0.0f, 0.0f, halfStep);
    const Eigen::Vector3f gradient(
        SampleTressfxSdfValue(layout, cells, position + dx) -
            SampleTressfxSdfValue(layout, cells, position - dx),
        SampleTressfxSdfValue(layout, cells, position + dy) -
            SampleTressfxSdfValue(layout, cells, position - dy),
        SampleTressfxSdfValue(layout, cells, position + dz) -
            SampleTressfxSdfValue(layout, cells, position - dz));
    return gradient / layout.cellSize;
}

bool ResolveTressfxSdfCollision(const TressfxSdfLayout& layout,
    const std::vector<std::uint32_t>& cells, Eigen::Vector3f& position)
{
    const float distance = SampleTressfxSdfValue(layout, cells, position);
    // TressFX 的 early-out：Distance > margin 直接返回。有符号距离下内部点
    // 是负值，因此总会被处理（这是与无符号版本的关键行为差异）。
    if (distance > layout.margin)
        return false;
    const Eigen::Vector3f gradient = TressfxSdfGradient(layout, cells, position);
    if (!(gradient.squaredNorm() > TressfxSdfGradientEpsilonSquared))
        return false;
    position += gradient.normalized() * (layout.margin - distance);
    return true;
}
}
