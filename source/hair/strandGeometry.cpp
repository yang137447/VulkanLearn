// Tangent/rest-length/ribbon topology adapted from AMD TressFXAsset.cpp.
// Copyright (c) 2016-2025 Advanced Micro Devices, Inc. All rights reserved.
// MIT license and source revision: THIRD_PARTY_NOTICES.md.
#include "hair/strandGeometry.h"

#include <Eigen/Core>
#include <algorithm>
#include <cmath>
#include <limits>
#include <new>

namespace VL::Hair
{
namespace
{
Eigen::Vector3d Position(const TfxAsset& asset, std::size_t index)
{
    const auto& point = asset.positions[index];
    return {point[0], point[1], point[2]};
}

std::array<float, 4> Tangent(const Eigen::Vector3d& direction)
{
    return {static_cast<float>(direction.x()), static_cast<float>(direction.y()),
        static_cast<float>(direction.z()), 0};
}
}

RuntimeResult<StrandGeometry> PrepareStrandGeometry(
    const TfxAsset& asset, const std::string& sourcePath)
{
    try
    {
        StrandGeometry geometry;
        const auto pointCount = asset.positions.size();
        geometry.tangents.resize(pointCount);
        geometry.restLengths.resize(pointCount, 0);
        const auto points = asset.pointsPerStrand;
        geometry.ribbonIndices.reserve(static_cast<std::size_t>(asset.guideCount) * (points - 1u) * 6u);
        for (std::size_t strand = 0; strand < asset.guideCount; ++strand)
        {
            const auto root = strand * points;
            Eigen::Vector3d previousDirection = Eigen::Vector3d::Zero();
            for (std::size_t segment = 0; segment + 1 < points; ++segment)
            {
                const auto index = root + segment;
                // w 是逆质量，不能进入方向和长度运算。用 double 计算差值，避免有限 float
                // 坐标相减/平方溢出；导入期拒绝不可表示的长度，渲染期无需逐帧兜底。
                const Eigen::Vector3d delta = Position(asset, index + 1) - Position(asset, index);
                const double length = delta.norm();
                if (!(length >= std::numeric_limits<float>::denorm_min())
                    || length > std::numeric_limits<float>::max())
                    return RuntimeResult<StrandGeometry>::Failure(MakeRuntimeError(
                        "Hair.StrandSegment", "Segment length must be positive and representable as float",
                        sourcePath, "positions[" + std::to_string(index) + "]"));
                const Eigen::Vector3d direction = delta / length;
                geometry.restLengths[index] = static_cast<float>(length);
                geometry.maxRestLength = std::max(geometry.maxRestLength, geometry.restLengths[index]);
                if (segment == 0)
                    geometry.tangents[index] = Tangent(direction);
                else
                {
                    // TressFX 对相邻单位方向取角平分线，保留其非等距采样语义。
                    const Eigen::Vector3d sum = previousDirection + direction;
                    const double sumLength = sum.norm();
                    if (sumLength == 0)
                        return RuntimeResult<StrandGeometry>::Failure(MakeRuntimeError(
                            "Hair.StrandTangent", "An exact strand reversal has no ribbon tangent",
                            sourcePath, "positions[" + std::to_string(index) + "]"));
                    geometry.tangents[index] = Tangent(sum / sumLength);
                }
                previousDirection = direction;
                const auto left = static_cast<std::uint32_t>(2u * index);
                // 左右顶点由后续 shader 按相机展开；每段两三角形，不跨 guide 连线。
                geometry.ribbonIndices.insert(geometry.ribbonIndices.end(),
                    {left, left + 1u, left + 2u, left + 2u, left + 1u, left + 3u});
            }
            geometry.tangents[root + points - 1u] = Tangent(previousDirection);
        }
        return RuntimeResult<StrandGeometry>::Success(std::move(geometry));
    }
    catch (const std::bad_alloc&)
    {
        return RuntimeResult<StrandGeometry>::Failure(MakeRuntimeError(
            "Hair.StrandBudget", "Cannot allocate strand geometry", sourcePath));
    }
}
}
