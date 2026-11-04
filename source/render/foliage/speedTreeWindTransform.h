#pragma once

#include <Eigen/Dense>

namespace VL
{

inline Eigen::Matrix3f BuildSpeedTreeWorldToLocalDirectionMatrix(
    const Eigen::Matrix4f& model)
{
    return model.block<3, 3>(0, 0).inverse();
}

inline Eigen::Vector3f TransformSpeedTreeWindDirectionToLocal(
    const Eigen::Matrix3f& worldToLocalDirectionMatrix,
    const Eigen::Vector3f& worldWindDirection)
{
    // 场景强度为 0 时世界风向为零向量。这里显式返回零，而不是依赖 normalized()
    // 对零向量的行为；着色器据此零向量早退，植被保持静止且不会出现 NaN。
    if (worldWindDirection.squaredNorm() == 0.0f)
    {
        return Eigen::Vector3f::Zero();
    }
    return (worldToLocalDirectionMatrix * worldWindDirection).normalized();
}

} // namespace VL
