#pragma once

#include <cstdint>
#include <vector>

#include <Eigen/Core>

#include "physics/fracture/fragmentMesh.h"

namespace VL::Physics::Fracture
{

// 运行时球壳破碎参数。球面 Voronoi 只根据半径、厚度、冲击方向和 seed 生成断裂拓扑，
// 不读取预制碎片资产；椭球可在调用前通过非均匀缩放把球面方向映射到目标形状。
struct SphericalFractureSettings
{
    uint64_t seed = 0;
    // 球面 Voronoi 种子数；至少 4，上限 128。
    int siteCount = 32;
    // 冲击方向，内部归一化；零向量会被拒绝。
    Eigen::Vector3d impactDirection = Eigen::Vector3d(0.0, 0.0, 1.0);
    // 0 = 全部 Fibonacci 均匀分布；1 = 除一个种子外全部围绕冲击方向成环。
    double impactBias = 0.55;
    // 球面角抖动（弧度）。过大时种子会重叠并在去重后减少。
    double angularJitter = 0.08;
    // 球壳外半径和壁厚，单位与项目一致（厘米）。
    double radius = 20.0;
    double thickness = 0.2;
};

// 一个球面 Voronoi 碎片。boundary 是单位球面上的 CCW 边界方向；
// siteDirection 是生成该 cell 的种子方向，位于 cell 内部。
struct SphericalShard
{
    std::vector<Eigen::Vector3d> boundary;
    Eigen::Vector3d siteDirection = Eigen::Vector3d::UnitZ();
    double area = 0.0;
    uint32_t patternIndex = 0;
};

struct SphericalPattern
{
    std::vector<SphericalShard> shards;
    double coveredArea = 0.0;
};

// 生成运行时球面 Voronoi 破碎拓扑。返回的 cell 覆盖整个球面；
// 调用方负责把方向映射到实际球/椭球、创建刚体或软体，并限制碎片预算。
SphericalPattern GenerateSphericalBurstPattern(
    const SphericalFractureSettings& settings);

// 把球面碎片挤出成有厚度的薄壳网格。外表面沿径向朝外，内表面朝内；
// convexHullPoints 是相对 localCenter 的近似凸包点，供 Jolt 刚体近似使用。
FragmentMesh ExtrudeSphericalShard(
    const SphericalShard& shard,
    double radius,
    double thickness);

} // namespace VL::Physics::Fracture
