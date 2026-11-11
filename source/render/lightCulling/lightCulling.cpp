#include "render/lightCulling/lightCulling.h"

#include <algorithm>
#include <cmath>

namespace VL
{
namespace
{

bool IsFinite(const Eigen::Vector3f& value)
{
    return std::isfinite(value.x()) &&
        std::isfinite(value.y()) &&
        std::isfinite(value.z());
}

LightScreenBounds FullScreenBounds(const LightCullingCamera& camera)
{
    LightScreenBounds bounds;
    bounds.visible = true;
    bounds.minX = 0.0f;
    bounds.minY = 0.0f;
    bounds.maxX = static_cast<float>(camera.width);
    bounds.maxY = static_cast<float>(camera.height);
    return bounds;
}

bool ShouldCullByViewDepth(
    float viewDepthCm,
    float sphereRadius,
    const LightCullingCamera& camera)
{
    // 半径为 0 代表“无包围范围”，不能据此判定整盏灯都在裁剪面外。
    if (!(sphereRadius > 0.0f))
    {
        return false;
    }
    if (std::isfinite(camera.nearCm) && camera.nearCm > 0.0f &&
        viewDepthCm + sphereRadius <= camera.nearCm)
    {
        return true;
    }
    if (std::isfinite(camera.farCm) && camera.farCm > camera.nearCm &&
        viewDepthCm - sphereRadius > camera.farCm)
    {
        return true;
    }
    return false;
}

} // namespace

void ComputeLightTileGrid(
    uint32_t renderWidth,
    uint32_t renderHeight,
    uint32_t& outTilesX,
    uint32_t& outTilesY)
{
    const uint32_t width = std::max(renderWidth, 1u);
    const uint32_t height = std::max(renderHeight, 1u);
    outTilesX =
        (width + kLightCullingTileSize - 1) / kLightCullingTileSize;
    outTilesY =
        (height + kLightCullingTileSize - 1) / kLightCullingTileSize;
}

LightScreenBounds ComputeLightScreenBounds(
    const Eigen::Vector3f& lightCenter,
    float sphereRadius,
    const LightCullingCamera& camera)
{
    LightScreenBounds bounds;
    if (!IsFinite(lightCenter) ||
        !std::isfinite(sphereRadius))
    {
        return bounds;
    }

    const Eigen::Vector4f centerView =
        camera.view *
        Eigen::Vector4f(lightCenter.x(), lightCenter.y(), lightCenter.z(), 1.0f);
    const float viewDepth = -centerView.z();
    const float radius = std::max(sphereRadius, 0.0f);
    if (ShouldCullByViewDepth(viewDepth, radius, camera))
    {
        return bounds;
    }

    // 半径 <= 0 的灯在引擎语义里是“无界”：不能只投影一个点，否则会把
    // 它错误地限制在一个 tile 附近。统一覆盖全屏，保证不漏灯。
    if (!(sphereRadius > 0.0f))
    {
        return FullScreenBounds(camera);
    }

    const Eigen::Vector4f clip =
        camera.projection * centerView;
    // clip.w <= 0 表示包围球跨过相机平面（或灯在相机后方但半径覆盖到前方）。
    // 此时透视除法没有定义，直接保守覆盖全屏。
    if (!(clip.w() > 1.0e-4f))
    {
        return FullScreenBounds(camera);
    }

    const float ndcX = clip.x() / clip.w();
    const float ndcY = clip.y() / clip.w();
    if (!std::isfinite(ndcX) || !std::isfinite(ndcY))
    {
        return FullScreenBounds(camera);
    }

    const float width = static_cast<float>(std::max(camera.width, 1u));
    const float height = static_cast<float>(std::max(camera.height, 1u));
    const float centerX = (ndcX * 0.5f + 0.5f) * width;
    const float centerY = (ndcY * 0.5f + 0.5f) * height;

    // 近距离灯球的屏幕半径会趋于无穷；用“球面最靠近 near 的深度”放大半径，
    // 保证保守。该值只影响 tile 覆盖范围，不参与光照强度。
    const float safeNear = std::max(camera.nearCm, 1.0e-3f);
    const float nearestDepth = std::max(viewDepth - radius, safeNear);
    const float scaleX = std::abs(camera.projection(0, 0));
    const float scaleY = std::abs(camera.projection(1, 1));
    float radiusPxX =
        radius * scaleX / nearestDepth * 0.5f * width;
    float radiusPxY =
        radius * scaleY / nearestDepth * 0.5f * height;
    const float clampLimit = width + height;
    radiusPxX = std::min(std::max(radiusPxX, 0.0f), clampLimit);
    radiusPxY = std::min(std::max(radiusPxY, 0.0f), clampLimit);

    bounds.visible = true;
    bounds.minX = centerX - radiusPxX;
    bounds.minY = centerY - radiusPxY;
    bounds.maxX = centerX + radiusPxX;
    bounds.maxY = centerY + radiusPxY;
    return bounds;
}

Eigen::Vector3f ComputeSpotCullingCenter(const LightSnapshot& light)
{
    const float forwardLength = light.direction.norm();
    if (forwardLength <= 1.0e-6f || !(light.radius > 0.0f))
    {
        return light.position;
    }
    return light.position +
        light.direction / forwardLength * (0.5f * light.radius);
}

float ComputeSpotCullingRadius(const LightSnapshot& light)
{
    if (!(light.radius > 0.0f))
    {
        // 0 = 作者未限制半径，按无界处理。
        return 0.0f;
    }
    const float halfHeight = 0.5f * light.radius;
    // 锥体外角接近 90° 时 tan 发散；夹到 86° 仍覆盖全部有效聚光，
    // 也避免把包围球膨胀成整屏（那会退化成不裁剪）。
    const float outer = std::clamp(light.coneAngleOuter, 0.0f, 1.5f);
    const float baseRadius = light.radius * std::tan(outer);
    return std::sqrt(
        halfHeight * halfHeight +
        baseRadius * baseRadius);
}

bool TileOverlapsLightBounds(
    const LightScreenBounds& bounds,
    uint32_t tileX,
    uint32_t tileY,
    uint32_t tileSize,
    uint32_t width,
    uint32_t height)
{
    if (!bounds.visible)
    {
        return false;
    }
    const float tileMinX =
        static_cast<float>(tileX * tileSize);
    const float tileMinY =
        static_cast<float>(tileY * tileSize);
    const float tileMaxX = std::min(
        tileMinX + static_cast<float>(tileSize),
        static_cast<float>(width));
    const float tileMaxY = std::min(
        tileMinY + static_cast<float>(tileSize),
        static_cast<float>(height));
    return
        bounds.minX < tileMaxX &&
        bounds.maxX > tileMinX &&
        bounds.minY < tileMaxY &&
        bounds.maxY > tileMinY;
}

bool LightTileAccumulator::Append(uint32_t lightIndex)
{
    if (indices.size() >= capacity)
    {
        ++overflow;
        return false;
    }
    indices.push_back(lightIndex);
    return true;
}

void BuildLightTileReference(
    const std::vector<LightSnapshot>& lights,
    const LightCullingCamera& camera,
    uint32_t tileX,
    uint32_t tileY,
    uint32_t tileSize,
    LightTileAccumulator& accumulator)
{
    // 与 RendererFrameResources::UpdateLightBuffer 的打包顺序一致：
    // 方向光 [0, dir)，点光 [dir, dir+point)，聚光 [dir+point, ...)。
    // 因此这里也必须按类型分三段遍历，不能按场景作者顺序——GPU 的
    // culling compute 读的是已经按类型打包后的 SSBO。
    uint32_t lightIndex = 0;
    for (const LightSnapshot& light : lights)
    {
        if (light.type != LightSnapshotType::Directional)
        {
            continue;
        }
        // 方向光不裁剪：所有 tile 都能看到它。
        accumulator.Append(lightIndex);
        ++lightIndex;
    }

    for (const LightSnapshot& light : lights)
    {
        if (light.type != LightSnapshotType::Point)
        {
            continue;
        }
        const LightScreenBounds bounds =
            ComputeLightScreenBounds(
                light.position,
                light.radius,
                camera);
        if (TileOverlapsLightBounds(
                bounds,
                tileX,
                tileY,
                tileSize,
                camera.width,
                camera.height))
        {
            accumulator.Append(lightIndex);
        }
        ++lightIndex;
    }

    for (const LightSnapshot& light : lights)
    {
        if (light.type != LightSnapshotType::Spot)
        {
            continue;
        }
        const LightScreenBounds bounds =
            ComputeLightScreenBounds(
                ComputeSpotCullingCenter(light),
                ComputeSpotCullingRadius(light),
                camera);
        if (TileOverlapsLightBounds(
                bounds,
                tileX,
                tileY,
                tileSize,
                camera.width,
                camera.height))
        {
            accumulator.Append(lightIndex);
        }
        ++lightIndex;
    }
}

} // namespace VL
