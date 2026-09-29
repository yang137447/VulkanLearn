#pragma once

#include <memory>

#include <Eigen/Dense>

#include "core/renderer.h"
#include "mesh/meshAssetTypes.h"

namespace VL
{

// CPU 导入结果与 core 的唯一网格转换边界，不重新解析 OBJ/FBX 或读取 GPU buffer。
std::shared_ptr<const Offline::Mesh> BuildOfflineMesh(const MeshSection& section,
    const std::atomic<std::uint64_t>* latestGeneration = nullptr,
    std::uint64_t expectedGeneration = 0);
Offline::MeshInstance BuildOfflineInstance(
    const Offline::Mesh& mesh,
    const Eigen::Matrix4f& model,
    Offline::SurfaceModel materialModel);

} // namespace VL
