#pragma once

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

#include <Eigen/Dense>

#include "vertexDataStruct.h"

namespace VL
{

// VEG_*.json 的最小静态实例描述。位置/缩放是 cm、对象局部空间；
// rotation 是归一化四元数，分量顺序 xyzw（与场景 JSON 的欧拉角不同）。
struct VegetationInstance
{
    Eigen::Vector3f position = Eigen::Vector3f::Zero();
    Eigen::Quaternionf rotation = Eigen::Quaternionf::Identity();
    Eigen::Vector3f scale = Eigen::Vector3f::Ones();
    float windPhase = 0.0f;
    float lodBias = 0.0f;
    uint32_t variant = 0;
    uint32_t seed = 0;
};

struct VegetationAsset
{
    std::string meshPath;
    std::vector<VegetationInstance> instances;
};

// 读取并校验 VEG v1；失败返回 false 并写入诊断。路径必须是资源根相对或
// 已解析的绝对路径，调用方负责把它放到资源根下。
bool LoadVegetationAsset(
    const std::filesystem::path& path,
    VegetationAsset& asset,
    std::string& error);

std::vector<InstanceVertex> BuildVegetationInstanceVertices(
    const VegetationAsset& asset);

// 将每个实例矩阵作用到 mesh section 的局部 AABB，得到批对象局部包围盒。
void AccumulateVegetationBounds(
    const VegetationAsset& asset,
    const Eigen::Vector3f& sectionBoundsMin,
    const Eigen::Vector3f& sectionBoundsMax,
    Eigen::Vector3f& boundsMin,
    Eigen::Vector3f& boundsMax);

} // namespace VL
