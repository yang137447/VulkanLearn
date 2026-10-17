#pragma once

#include <memory>
#include "mesh/meshAssetTypes.h"

namespace VL
{
// 正式 CPU 导入已经验证索引和三角拓扑；这里仅冻结局部位置，不重新导入或保留材质/GPU引用。
inline std::shared_ptr<const std::vector<Eigen::Vector3f>> FreezeMeshCollisionTriangles(const MeshSection& section)
{
    auto triangles = std::make_shared<std::vector<Eigen::Vector3f>>();
    triangles->reserve(section.indices.size());
    for (const auto index : section.indices) triangles->push_back(section.vertices[index].position);
    return triangles;
}
} // namespace VL
