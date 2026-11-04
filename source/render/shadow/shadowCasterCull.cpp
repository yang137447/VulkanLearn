#include "render/shadow/shadowCasterCull.h"

#include <cmath>
#include <cstddef>

namespace VL
{
namespace
{

constexpr float kPlaneNormalEpsilon = 1e-6f;

} // namespace

bool ShadowCasterCullFrustum::Build(
    const Eigen::Matrix4f& lightViewProjection)
{
    valid = false;
    if (!lightViewProjection.allFinite())
    {
        return false;
    }

    // Gribb/Hartmann 平面提取。矩阵按列向量约定使用：clip = M * [x,y,z,1]。
    // Vulkan 的近平面是 z = 0（不是 OpenGL 的 z = -w），所以 near 取 row2，
    // 其余五个面仍是 row3 ± rowN。
    const Eigen::Vector4f row0 = lightViewProjection.row(0);
    const Eigen::Vector4f row1 = lightViewProjection.row(1);
    const Eigen::Vector4f row2 = lightViewProjection.row(2);
    const Eigen::Vector4f row3 = lightViewProjection.row(3);

    const std::array<Eigen::Vector4f, 6> coefficients = {
        row3 + row0, // left:   x + w >= 0
        row3 - row0, // right:  w - x >= 0
        row3 + row1, // bottom: y + w >= 0
        row3 - row1, // top:    w - y >= 0
        row2,        // near:   z >= 0
        row3 - row2  // far:    w - z >= 0
    };

    for (size_t index = 0; index < coefficients.size(); ++index)
    {
        const Eigen::Vector4f& coefficient = coefficients[index];
        Eigen::Vector3f normal = coefficient.head<3>();
        const float length = normal.norm();
        if (!std::isfinite(length) || length <= kPlaneNormalEpsilon)
        {
            // 退化级联（例如零尺寸投影）不产生可信平面，整级联回退到“全部保留”。
            return false;
        }

        Plane& plane = planes[index];
        plane.normal = normal / length;
        plane.offset = coefficient.w() / length;
        if (!plane.normal.allFinite() || !std::isfinite(plane.offset))
        {
            return false;
        }
    }

    valid = true;
    return true;
}

bool ShadowCasterCullFrustum::IntersectsWorldBounds(
    const Eigen::Vector3f& boundsMin,
    const Eigen::Vector3f& boundsMax,
    float padding) const
{
    // 任何输入不确定的情况都保留 draw；这是本模块唯一的失效方向。
    if (!valid)
    {
        return true;
    }
    if (!boundsMin.allFinite() || !boundsMax.allFinite() ||
        !std::isfinite(padding))
    {
        return true;
    }
    if ((boundsMin.array() > boundsMax.array()).any())
    {
        // 退化/空包围盒没有可信空间范围，不能据此剔除。
        return true;
    }

    const float safePadding = padding > 0.0f ? padding : 0.0f;
    const Eigen::Vector3f center = (boundsMin + boundsMax) * 0.5f;
    Eigen::Vector3f extent = (boundsMax - boundsMin) * 0.5f;
    extent.array() += safePadding;

    for (const Plane& plane : planes)
    {
        const float projection = plane.normal.dot(center) + plane.offset;
        const float radius = plane.normal.cwiseAbs().dot(extent);
        const float distanceToPlane = projection + radius;
        if (!std::isfinite(distanceToPlane))
        {
            return true;
        }

        // 只有整个 AABB 严格越过平面才可剔除；相切（== 0）也保留，
        // 让浮点误差不可能变成可见的阴影缺口。
        if (distanceToPlane < 0.0f)
        {
            return false;
        }
    }

    return true;
}

bool ShadowCasterBoundsTouchReceiverLightSpaceXy(
    const Eigen::Vector3f& boundsMin,
    const Eigen::Vector3f& boundsMax,
    const ShadowCasterReceiverBounds& receiver,
    float padding)
{
    if (!boundsMin.allFinite() || !boundsMax.allFinite() ||
        !receiver.valid ||
        !receiver.worldToLight.allFinite() ||
        !receiver.lightSpaceXyMinMax.allFinite() ||
        !std::isfinite(padding))
    {
        return true;
    }
    if ((boundsMin.array() > boundsMax.array()).any())
    {
        return true;
    }

    const float safePadding = padding > 0.0f ? padding : 0.0f;
    const Eigen::Vector3f center = (boundsMin + boundsMax) * 0.5f;
    Eigen::Vector3f extent = (boundsMax - boundsMin) * 0.5f;
    extent.array() += safePadding;

    // 线性变换后 AABB 的中心与半轴：c' = M c，e' = |M| e。
    const Eigen::Vector3f lightCenter = receiver.worldToLight * center;
    const Eigen::Vector3f lightExtent =
        receiver.worldToLight.cwiseAbs() * extent;
    if (!lightCenter.allFinite() || !lightExtent.allFinite())
    {
        return true;
    }

    const float receiverMinX = receiver.lightSpaceXyMinMax.x();
    const float receiverMinY = receiver.lightSpaceXyMinMax.y();
    const float receiverMaxX = receiver.lightSpaceXyMinMax.z();
    const float receiverMaxY = receiver.lightSpaceXyMinMax.w();
    if (!(receiverMinX <= receiverMaxX) || !(receiverMinY <= receiverMaxY))
    {
        return true;
    }

    // 严格分离才剔除；相切仍保留，和平面测试采用同一保守规则。
    if (lightCenter.x() + lightExtent.x() < receiverMinX ||
        lightCenter.x() - lightExtent.x() > receiverMaxX ||
        lightCenter.y() + lightExtent.y() < receiverMinY ||
        lightCenter.y() - lightExtent.y() > receiverMaxY)
    {
        return false;
    }
    return true;
}

} // namespace VL
