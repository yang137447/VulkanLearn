#pragma once

#include <algorithm>
#include <cstdint>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

#include <Eigen/Dense>

#include "hair/tressfxCollision.h"

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
    // 与 config.capsules 同下标的关节下标；-1 表示该胶囊没有 bone、保持静态。
    // 与 jointIndex 共用同一个源对象与骨架，运行时矩阵也来自同一份 SkinPose。
    std::vector<int32_t> capsuleJointIndices;
};

// 按名字查找目标骨架关节。名字大小写敏感，与 Animation::SkeletonAsset::JointIndex
// 和 FBX 导入器一致。jointNames 为空表示源对象没有 SK：这是硬错误，不是回退。
// usage 描述调用方（如 "simulation.skinning root bone"、"collision.capsules[0].bone"），
// 让错误消息同时带角色、骨名、对象名与发丝 SM 资产路径。未知骨同样是硬错误。
inline uint32_t ResolveTressfxJointByName(
    const std::vector<std::string>& jointNames,
    std::string_view sourceSceneObject,
    std::string_view boneName,
    std::string_view usage,
    std::string_view hairAssetPath)
{
    if (jointNames.empty())
        throw std::runtime_error(
            "TressFX simulation.skinning source object \"" +
            std::string(sourceSceneObject) +
            "\" has no skeleton (SK): " + std::string(hairAssetPath));
    for (size_t index = 0; index < jointNames.size(); ++index)
    {
        if (jointNames[index] == boneName)
            return static_cast<uint32_t>(index);
    }
    throw std::runtime_error(
        "TressFX " + std::string(usage) + " \"" + std::string(boneName) +
        "\" not found in source object \"" + std::string(sourceSceneObject) +
        "\" skeleton: " + std::string(hairAssetPath));
}

// 根骨入口：错误消息保持既有措辞，供加载期与模块测试共用。
inline uint32_t ResolveTressfxRootJoint(
    const std::vector<std::string>& jointNames,
    std::string_view sourceSceneObject,
    std::string_view rootBone,
    std::string_view hairAssetPath)
{
    return ResolveTressfxJointByName(jointNames, sourceSceneObject, rootBone,
        "simulation.skinning root bone", hairAssetPath);
}

// 每个胶囊按自己的关节矩阵变换端点；没有关节（-1）、关节下标越出当前 pose
// 或胶囊数量不匹配时保留作者绑定空间的值。GPU pass 与测试共用这一份语义，
// 避免 Host 与 shader 对“未绑定胶囊”产生两种理解。
inline std::vector<TressfxCollider> TransformTressfxCapsulesByBones(
    const std::vector<TressfxCollider>& capsules,
    const std::vector<int32_t>& capsuleJointIndices,
    const std::vector<Eigen::Matrix4f>& jointMatrices)
{
    std::vector<TressfxCollider> transformed = capsules;
    const size_t boundCount =
        std::min(capsules.size(), capsuleJointIndices.size());
    for (size_t index = 0; index < boundCount; ++index)
    {
        const int32_t joint = capsuleJointIndices[index];
        if (joint < 0 || static_cast<size_t>(joint) >= jointMatrices.size())
            continue;
        transformed[index] = TransformTressfxCapsuleByBone(
            capsules[index], jointMatrices[static_cast<size_t>(joint)]);
    }
    return transformed;
}

// CPU 侧对 TressFXSimulationIntegration InitialPos 的镜像，只用于纯数学测试。
// TressFX 对每个顶点先算 InitialPos = ApplyVertexBoneSkinning(RestPos)，不可移动点
// （根前缀，或 VL 扩展的逆质量 <= 0）整步都写回 InitialPos。VL 只有单个根骨矩阵，
// 所以这里就是 M * rest；可移动点不会调用这个函数。GPU pass 必须保持同一规则。
inline Eigen::Vector3f TressfxInitialPosition(
    const Eigen::Matrix4f& skinningMatrix,
    const Eigen::Vector3f& restPosition)
{
    return (skinningMatrix * restPosition.homogeneous()).head<3>();
}

} // namespace VL::Hair
