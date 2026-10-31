#pragma once

#include <vector>

#include <Eigen/Core>

#include "physics/fracture/planarFracture.h"

namespace VL::Physics::Fracture
{

// 平面碎片挤出后的 CPU 网格。positions/normals 按面复制顶点以便硬边着色；
// convexHullPoints 只保留给 Jolt 凸包，不参与渲染索引。
// 所有坐标都相对于 localCenter，渲染和刚体都把它当局部原点。
struct FragmentMesh
{
    std::vector<Eigen::Vector3f> positions;
    std::vector<Eigen::Vector3f> normals;
    std::vector<uint32_t> indices;
    std::vector<Eigen::Vector3f> convexHullPoints;
    Eigen::Vector3f localCenter = Eigen::Vector3f::Zero();
    double volume = 0.0;
};

struct ExtrusionSettings
{
    // 世界单位与项目一致：厘米。厚度必须为正。
    double thickness = 0.6;
    // 挤出中心在碎片局部 Z 轴上的位置。
    double centerZ = 0.0;
};

// 将 CCW 平面碎片沿 Z 轴挤出为封闭凸棱柱。用于玻璃碎片的渲染和 Jolt ConvexHullShape。
FragmentMesh ExtrudePlanarShard(
    const PlanarShard& shard,
    const ExtrusionSettings& settings);

// 三角网格有向体积；外法线朝外时为正。用于验证挤出结果的绕序。
double SignedMeshVolume(
    const std::vector<Eigen::Vector3f>& positions,
    const std::vector<uint32_t>& indices);

} // namespace VL::Physics::Fracture
