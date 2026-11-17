#include "terrain/terrainCollisionProxy.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <stdexcept>
#include <string>

#include "terrain/terrainHeightmap.h"

namespace VL
{
namespace
{

bool IsFiniteVector(const Eigen::Vector3f& value)
{
    return std::isfinite(value.x()) && std::isfinite(value.y()) &&
        std::isfinite(value.z());
}

Eigen::Matrix3f PlacementRotationMatrix(const Eigen::Vector3f& degrees)
{
    constexpr float DegreesToRadians = 3.14159265358979323846f / 180.0f;
    const Eigen::Vector3f radians = degrees * DegreesToRadians;
    const Eigen::AngleAxisf yaw(radians.y(), Eigen::Vector3f::UnitY());
    const Eigen::AngleAxisf pitch(radians.x(), Eigen::Vector3f::UnitX());
    const Eigen::AngleAxisf roll(radians.z(), Eigen::Vector3f::UnitZ());
    return (yaw * pitch * roll).toRotationMatrix();
}

void ValidateInputs(
    const TerrainBuildPlan& plan,
    const TerrainCollisionPlacement& placement)
{
    if (!plan.heightmap)
        throw std::invalid_argument(
            "Terrain collision proxy requires a decoded heightmap");
    if (plan.heightmap->width < 2 || plan.heightmap->height < 2)
        throw std::invalid_argument(
            "Terrain collision proxy requires at least a 2x2 heightmap");
    if (!std::isfinite(plan.desc.size.x()) ||
        !std::isfinite(plan.desc.size.y()) ||
        plan.desc.size.x() <= 0.0f || plan.desc.size.y() <= 0.0f)
        throw std::invalid_argument(
            "Terrain collision proxy requires positive finite size");
    if (!std::isfinite(plan.desc.heightScale) ||
        !std::isfinite(plan.desc.heightOffset))
        throw std::invalid_argument(
            "Terrain collision proxy requires finite heightScale/heightOffset");
    if (!IsFiniteVector(placement.position) ||
        !IsFiniteVector(placement.rotation))
        throw std::invalid_argument(
            "Terrain collision proxy requires finite position/rotation");
    if (!IsFiniteVector(placement.scale) || placement.scale.x() <= 0.0f ||
        placement.scale.y() <= 0.0f || placement.scale.z() <= 0.0f)
        throw std::invalid_argument(
            "Terrain collision proxy requires positive finite scale");
}

} // namespace

uint32_t ResolveTerrainCollisionSampleCount(
    uint32_t requestedSampleCount,
    uint32_t gridResolution)
{
    uint32_t requested = requestedSampleCount == 0
        ? gridResolution + 1
        : requestedSampleCount;
    if (requested < TerrainCollisionMinSampleCount)
        throw std::invalid_argument(
            "Terrain collision sampleCount must be at least 4");
    if (requested > TerrainCollisionMaxSampleCount)
        throw std::invalid_argument(
            "Terrain collision sampleCount must not exceed 4096");
    const uint32_t remainder =
        requested % TerrainCollisionHeightFieldBlockSize;
    if (remainder != 0)
    {
        const uint32_t increment =
            TerrainCollisionHeightFieldBlockSize - remainder;
        if (requested > TerrainCollisionMaxSampleCount - increment)
            throw std::invalid_argument(
                "Terrain collision sampleCount must not exceed 4096");
        requested += increment;
    }
    return requested;
}

bool TerrainCollisionProxy::IsValid() const
{
    if (sampleCount < TerrainCollisionMinSampleCount ||
        sampleCount > TerrainCollisionMaxSampleCount ||
        sampleCount % TerrainCollisionHeightFieldBlockSize != 0)
        return false;
    if (!std::isfinite(sizeXCm) || !std::isfinite(sizeZCm) ||
        sizeXCm <= 0.0f || sizeZCm <= 0.0f)
        return false;
    if (heightCm.size() !=
        static_cast<size_t>(sampleCount) * sampleCount)
        return false;
    if (!boundsMinCm.allFinite() || !boundsMaxCm.allFinite())
        return false;
    if ((boundsMinCm.array() > boundsMaxCm.array()).any())
        return false;
    for (const float height : heightCm)
    {
        if (!std::isfinite(height))
            return false;
    }
    return true;
}

std::shared_ptr<const TerrainCollisionProxy> BuildTerrainCollisionProxy(
    const TerrainBuildPlan& plan,
    const TerrainCollisionPlacement& placement,
    uint32_t requestedSampleCount)
{
    ValidateInputs(plan, placement);
    const TerrainAssetDesc& desc = plan.desc;
    const uint32_t sampleCount = ResolveTerrainCollisionSampleCount(
        requestedSampleCount, desc.gridResolution);
    const float inverseCountMinusOne =
        1.0f / static_cast<float>(sampleCount - 1);
    const TerrainHeightmap::MeshUvTransform heightUvTransform =
        TerrainHeightmap::MeshUvTransformFor(desc, *plan.heightmap);

    auto proxy = std::make_shared<TerrainCollisionProxy>();
    proxy->placementId = TerrainPlacementIdentity(plan);
    proxy->placement = placement;
    proxy->sampleCount = sampleCount;
    proxy->sizeXCm = desc.size.x() * placement.scale.x();
    proxy->sizeZCm = desc.size.y() * placement.scale.z();
    proxy->heightCm.resize(
        static_cast<size_t>(sampleCount) * sampleCount);

    float minimumHeightCm = std::numeric_limits<float>::max();
    float maximumHeightCm = -std::numeric_limits<float>::max();
    for (uint32_t y = 0; y < sampleCount; ++y)
    {
        const float v = static_cast<float>(y) * inverseCountMinusOne;
        for (uint32_t x = 0; x < sampleCount; ++x)
        {
            const float u = static_cast<float>(x) * inverseCountMinusOne;
            const Eigen::Vector2f heightUv = heightUvTransform.Apply(u, v);
            // 与渲染网格同一采样口径：u 对应 +X、v 对应 +Z；世界 Y 已乘
            // placement.scale.y，Jolt body 只做 cm->m 换算。
            const float heightCm =
                TerrainHeightmap::SampleHeightCm(
                    desc, *plan.heightmap, heightUv.x(), heightUv.y()) *
                placement.scale.y();
            if (!std::isfinite(heightCm))
                throw std::invalid_argument(
                    "Terrain collision proxy contains non-finite samples");
            proxy->heightCm[
                static_cast<size_t>(y) * sampleCount + x] = heightCm;
            minimumHeightCm = std::min(minimumHeightCm, heightCm);
            maximumHeightCm = std::max(maximumHeightCm, heightCm);
        }
    }

    // 保守 AABB：覆盖 heightfield 的四个角与高度极值。它不是紧包围盒，
    // 只用于流式/调试；Jolt body 仍会给出精确世界包围盒。
    const Eigen::Matrix3f rotation =
        PlacementRotationMatrix(placement.rotation);
    const float halfX = proxy->sizeXCm * 0.5f;
    const float halfZ = proxy->sizeZCm * 0.5f;
    Eigen::Vector3f boundsMin(
        std::numeric_limits<float>::max(),
        std::numeric_limits<float>::max(),
        std::numeric_limits<float>::max());
    Eigen::Vector3f boundsMax(
        -std::numeric_limits<float>::max(),
        -std::numeric_limits<float>::max(),
        -std::numeric_limits<float>::max());
    for (const float localX : {-halfX, halfX})
    {
        for (const float localZ : {-halfZ, halfZ})
        {
            for (const float localY : {minimumHeightCm, maximumHeightCm})
            {
                const Eigen::Vector3f world = placement.position +
                    rotation * Eigen::Vector3f(localX, localY, localZ);
                boundsMin = boundsMin.cwiseMin(world);
                boundsMax = boundsMax.cwiseMax(world);
            }
        }
    }
    proxy->boundsMinCm = boundsMin;
    proxy->boundsMaxCm = boundsMax;
    if (!proxy->IsValid())
        throw std::invalid_argument(
            "Terrain collision proxy failed validation");
    return proxy;
}

} // namespace VL
