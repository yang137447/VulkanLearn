#pragma once

#include <cstdint>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

#include <Eigen/Dense>

namespace VL::Hair
{
// 解析完成后的发丝根点绑定。jointIndex 是源角色“目标骨架”的关节下标，
// 与 Animation::SkinPose::matrices 的下标空间一致；sourceSceneObject 是场景对象
// 身份（不是单 section 的 debugName），运行时据此在冻结快照里找 draw packet。
struct TressfxSkinningBinding
{
    bool enabled = false;
    std::string sourceSceneObject;
    std::string rootBone;
    uint32_t jointIndex = 0;
};

// 按名字查找目标骨架关节。名字大小写敏感，与 Animation::SkeletonAsset::JointIndex
// 和 FBX 导入器一致。jointNames 为空表示源对象没有 SK：这是硬错误，不是回退。
// 错误消息同时带对象名、关节名与发丝 SM 资产路径，便于从场景直接定位。
inline uint32_t ResolveTressfxRootJoint(
    const std::vector<std::string>& jointNames,
    std::string_view sourceSceneObject,
    std::string_view rootBone,
    std::string_view hairAssetPath)
{
    if (jointNames.empty())
        throw std::runtime_error(
            "TressFX simulation.skinning source object \"" +
            std::string(sourceSceneObject) +
            "\" has no skeleton (SK): " + std::string(hairAssetPath));
    for (size_t index = 0; index < jointNames.size(); ++index)
    {
        if (jointNames[index] == rootBone)
            return static_cast<uint32_t>(index);
    }
    throw std::runtime_error(
        "TressFX simulation.skinning root bone \"" + std::string(rootBone) +
        "\" not found in source object \"" + std::string(sourceSceneObject) +
        "\" skeleton: " + std::string(hairAssetPath));
}

// CPU 侧对 kernel 固定点分支的逐点镜像，只用于纯数学测试。
// localPoint < rootFixedPoints 的根前缀由骨骼矩阵 M 变换：p = M * rest；
// 其余点（包括逆质量 <= 0 的固定点）仍使用 rest。GPU kernel 必须保持同一规则。
inline Eigen::Vector3f TressfxFixedPointPosition(
    const Eigen::Matrix4f& skinningMatrix,
    const Eigen::Vector3f& restPosition,
    uint32_t localPoint,
    uint32_t rootFixedPoints)
{
    if (localPoint >= rootFixedPoints)
        return restPosition;
    return (skinningMatrix * restPosition.homogeneous()).head<3>();
}

} // namespace VL::Hair
