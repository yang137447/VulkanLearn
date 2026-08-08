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
    return (worldToLocalDirectionMatrix * worldWindDirection).normalized();
}

} // namespace VL
