#include "terrain/terrainCollisionPatch.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <stdexcept>

namespace VL
{
namespace
{

float SmoothStep(float edge0, float edge1, float value)
{
    if (edge1 <= edge0)
        return 1.0f;
    const float t = std::clamp((value - edge0) / (edge1 - edge0), 0.0f, 1.0f);
    return t * t * (3.0f - 2.0f * t);
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

} // namespace

std::shared_ptr<const TerrainCollisionPatch> BuildTerrainCollisionPatch(
    const TerrainCollisionProxy& macroProxy,
    const Eigen::Vector2f& centerLocalCm,
    const TerrainCollisionPatchBuildSettings& settings)
{
    if (!macroProxy.IsValid())
        throw std::invalid_argument(
            "Terrain collision patch requires a valid macro proxy");
    if (!std::isfinite(settings.patchSizeCm) ||
        !std::isfinite(settings.spacingCm) ||
        !std::isfinite(settings.edgeBlendCm) ||
        settings.patchSizeCm <= 0.0f || settings.spacingCm <= 0.0f ||
        settings.edgeBlendCm < 0.0f)
        throw std::invalid_argument(
            "Terrain collision patch requires positive size/spacing and "
            "non-negative edge blend");
    if (!centerLocalCm.allFinite())
        throw std::invalid_argument(
            "Terrain collision patch center is not finite");

    const float half = settings.patchSizeCm * 0.5f;
    if (!macroProxy.ContainsLocalXz(
            centerLocalCm.x() - half,
            centerLocalCm.y() - half) ||
        !macroProxy.ContainsLocalXz(
            centerLocalCm.x() + half,
            centerLocalCm.y() + half))
    {
        // 边缘 patch 会伸到地形外；该段回退到宏 body，避免伪造额外地面。
        return nullptr;
    }

    uint32_t sampleCount =
        static_cast<uint32_t>(
            std::ceil(settings.patchSizeCm / settings.spacingCm)) + 1;
    if (sampleCount % TerrainCollisionHeightFieldBlockSize != 0)
        ++sampleCount;
    if (sampleCount < TerrainCollisionMinSampleCount ||
        sampleCount > TerrainCollisionMaxSampleCount)
        throw std::invalid_argument(
            "Terrain collision patch sample count is outside [4, 4096]");
    const float actualSpacing =
        settings.patchSizeCm / static_cast<float>(sampleCount - 1);

    auto patch = std::make_shared<TerrainCollisionPatch>();
    patch->centerLocalCm = centerLocalCm;
    patch->sourcePatchSizeCm = settings.patchSizeCm;
    TerrainCollisionProxy& proxy = patch->proxy;
    proxy.placementId = macroProxy.placementId;
    proxy.placement = macroProxy.placement;
    const Eigen::Matrix3f rotation =
        PlacementRotationMatrix(macroProxy.placement.rotation);
    const Eigen::Vector3f centerOffset = rotation * Eigen::Vector3f(
        centerLocalCm.x(), 0.0f, centerLocalCm.y());
    proxy.placement.position.x() += centerOffset.x();
    proxy.placement.position.z() += centerOffset.z();
    proxy.placement.scale = Eigen::Vector3f::Ones();
    proxy.sampleCount = sampleCount;
    proxy.sizeXCm = settings.patchSizeCm;
    proxy.sizeZCm = settings.patchSizeCm;
    proxy.heightCm.resize(
        static_cast<size_t>(sampleCount) * sampleCount);

    float minimumHeightCm = std::numeric_limits<float>::max();
    float maximumHeightCm = -std::numeric_limits<float>::max();
    const float startX = centerLocalCm.x() - half;
    const float startZ = centerLocalCm.y() - half;
    for (uint32_t z = 0; z < sampleCount; ++z)
    {
        const float localZ = startZ + static_cast<float>(z) * actualSpacing;
        for (uint32_t x = 0; x < sampleCount; ++x)
        {
            const float localX =
                startX + static_cast<float>(x) * actualSpacing;
            const float macroHeight =
                macroProxy.SampleHeightCmLocal(localX, localZ);
            const float edgeDistance = std::min(
                std::min(localX - (centerLocalCm.x() - half),
                    (centerLocalCm.x() + half) - localX),
                std::min(localZ - (centerLocalCm.y() - half),
                    (centerLocalCm.y() + half) - localZ));
            const float edgeFactor = SmoothStep(
                0.0f, settings.edgeBlendCm, edgeDistance);
            // A 口径：唯一高度源是宏代理，blend 当前是恒等；保留公式给后续
            // 更高分辨率的碰撞源。
            const float sourceHeight = macroHeight;
            const float heightCm =
                macroHeight + (sourceHeight - macroHeight) * edgeFactor;
            if (!std::isfinite(heightCm))
                throw std::invalid_argument(
                    "Terrain collision patch contains non-finite samples");
            proxy.heightCm[static_cast<size_t>(z) * sampleCount + x] =
                heightCm;
            minimumHeightCm = std::min(minimumHeightCm, heightCm);
            maximumHeightCm = std::max(maximumHeightCm, heightCm);
        }
    }

    // 保守 AABB：patch 局部 X/Z 半宽 + 高度极值，经 placement 旋转/平移。
    Eigen::Vector3f boundsMin(
        std::numeric_limits<float>::max(),
        std::numeric_limits<float>::max(),
        std::numeric_limits<float>::max());
    Eigen::Vector3f boundsMax(
        -std::numeric_limits<float>::max(),
        -std::numeric_limits<float>::max(),
        -std::numeric_limits<float>::max());
    for (const float localX : {-half, half})
    {
        for (const float localZ : {-half, half})
        {
            for (const float localY : {minimumHeightCm, maximumHeightCm})
            {
                const Eigen::Vector3f world =
                    proxy.placement.position +
                    rotation * Eigen::Vector3f(localX, localY, localZ);
                boundsMin = boundsMin.cwiseMin(world);
                boundsMax = boundsMax.cwiseMax(world);
            }
        }
    }
    proxy.boundsMinCm = boundsMin;
    proxy.boundsMaxCm = boundsMax;
    if (!patch->IsValid())
        throw std::invalid_argument(
            "Terrain collision patch failed validation");
    return patch;
}

} // namespace VL
