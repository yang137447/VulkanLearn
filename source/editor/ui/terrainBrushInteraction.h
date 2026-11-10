#pragma once

#include <algorithm>
#include <cmath>

#include <Eigen/Dense>

#include "editor/ui/blueprintViewportInteraction.h"
#include "terrain/terrainAssetTypes.h"
#include "terrain/terrainHeightmap.h"

namespace VL::EditorUi
{

// 地形 brush 命中结果。u/v 是高度图归一化坐标，worldPosition 只用于画环，
// 不进入 Scene 文档；调用方不得把它当成可持久化身份。
struct TerrainBrushSurfaceHit
{
    bool valid = false;
    Eigen::Vector2f uv = Eigen::Vector2f::Zero();
    Eigen::Vector2f pixel = Eigen::Vector2f::Zero();
    Eigen::Vector3f worldPosition = Eigen::Vector3f::Zero();
};

inline Eigen::Matrix4f TerrainBrushWorldFromLocal(const Eigen::Vector3f& position,
    const Eigen::Vector3f& rotationDegrees, const Eigen::Vector3f& scale)
{
    const Eigen::Vector3f radians = rotationDegrees * 0.017453292519943295f;
    const Eigen::Quaternionf yaw(Eigen::AngleAxisf(radians.y(), Eigen::Vector3f::UnitY()));
    const Eigen::Quaternionf pitch(Eigen::AngleAxisf(radians.x(), Eigen::Vector3f::UnitX()));
    const Eigen::Quaternionf roll(Eigen::AngleAxisf(radians.z(), Eigen::Vector3f::UnitZ()));
    Eigen::Matrix4f matrix = Eigen::Matrix4f::Identity();
    matrix.block<3, 3>(0, 0) = (yaw * pitch * roll).toRotationMatrix() *
        scale.asDiagonal();
    matrix.block<3, 1>(0, 3) = position;
    return matrix;
}

inline float TerrainBrushHeightDelta(const TerrainBuildPlan& plan,
    const Eigen::Vector3f& localOrigin, const Eigen::Vector3f& localDirection,
    float distance, Eigen::Vector2f& uv)
{
    const Eigen::Vector3f local = localOrigin + localDirection * distance;
    uv = Eigen::Vector2f(
        std::clamp(local.x() / plan.desc.size.x() + 0.5f, 0.0f, 1.0f),
        std::clamp(local.z() / plan.desc.size.y() + 0.5f, 0.0f, 1.0f));
    const float height = TerrainHeightmap::SampleHeightCm(
        plan.desc, *plan.heightmap, uv.x(), uv.y());
    return local.y() - height;
}

// 射线先逆变换到地形局部空间，再在 [-size/2,size/2] 的 X/Z 包围盒内做
// heightfield 步进 + 二分细化。这样 WORLD 的旋转/缩放/平移不会把 UV 或
// 世界高度算错；地形高度只从共享只读 plan 中取。
inline TerrainBrushSurfaceHit PickTerrainBrushSurface(
    const EditorSceneCameraActor& camera, float width, float height,
    const Eigen::Vector2f& pixel, const TerrainBuildPlan& plan,
    const Eigen::Vector3f& position, const Eigen::Vector3f& rotationDegrees,
    const Eigen::Vector3f& scale)
{
    TerrainBrushSurfaceHit result;
    if (!BlueprintViewportDetail::ValidCamera(camera, width, height) ||
        !pixel.allFinite() || plan.heightmap == nullptr ||
        plan.desc.size.x() <= 0.0f || plan.desc.size.y() <= 0.0f ||
        !position.allFinite() || !rotationDegrees.allFinite() || !scale.allFinite() ||
        (scale.array().abs() < 1.0e-6f).any())
        return result;

    const Eigen::Matrix4f worldFromLocal = TerrainBrushWorldFromLocal(
        position, rotationDegrees, scale);
    const Eigen::FullPivLU<Eigen::Matrix4f> decomposition(worldFromLocal);
    if (!decomposition.isInvertible()) return result;
    const Eigen::Matrix4f localFromWorld = decomposition.inverse();
    const Eigen::Vector3f ray = BlueprintViewportDetail::RayDirection(
        camera, width, height, pixel);
    const Eigen::Vector3f localOrigin = (localFromWorld *
        camera.position.homogeneous()).head<3>();
    const Eigen::Vector3f localDirection =
        localFromWorld.block<3, 3>(0, 0) * ray;
    if (!localOrigin.allFinite() || !localDirection.allFinite() ||
        localDirection.squaredNorm() < 1.0e-12f)
        return result;

    const float halfX = plan.desc.size.x() * 0.5f;
    const float halfZ = plan.desc.size.y() * 0.5f;
    // 高度图允许 HDR/EXR 的 0..1 之外的样本，不能用 heightOffset/heightScale
    // 猜一个窄 Y 包围盒；X/Z 限定地形 footprint，Y 交给实际高度采样裁剪。
    const Eigen::Vector3f boundsMin(-halfX, -1.0e9f, -halfZ);
    const Eigen::Vector3f boundsMax(halfX, 1.0e9f, halfZ);
    float enter = std::max(0.0f, camera.clipNear);
    float exit = camera.clipFar;
    for (int axis = 0; axis < 3; ++axis)
    {
        if (std::abs(localDirection[axis]) < 1.0e-9f)
        {
            if (localOrigin[axis] < boundsMin[axis] ||
                localOrigin[axis] > boundsMax[axis])
                return result;
            continue;
        }
        const float first = (boundsMin[axis] - localOrigin[axis]) /
            localDirection[axis];
        const float last = (boundsMax[axis] - localOrigin[axis]) /
            localDirection[axis];
        enter = std::max(enter, std::min(first, last));
        exit = std::min(exit, std::max(first, last));
        if (enter > exit) return result;
    }

    constexpr int Steps = 512;
    float previousT = enter;
    Eigen::Vector2f previousUv;
    float previousDelta = TerrainBrushHeightDelta(
        plan, localOrigin, localDirection, enter, previousUv);
    if (std::abs(previousDelta) < 0.05f)
    {
        result.valid = true;
        result.uv = previousUv;
        result.pixel = pixel;
        result.worldPosition = (worldFromLocal *
            (localOrigin + localDirection * enter).homogeneous()).head<3>();
        return result;
    }
    for (int step = 1; step <= Steps; ++step)
    {
        const float currentT = enter + (exit - enter) *
            static_cast<float>(step) / static_cast<float>(Steps);
        Eigen::Vector2f currentUv;
        const float currentDelta = TerrainBrushHeightDelta(
            plan, localOrigin, localDirection, currentT, currentUv);
        if (previousDelta > 0.0f && currentDelta <= 0.0f ||
            previousDelta < 0.0f && currentDelta >= 0.0f)
        {
            float low = previousT;
            float high = currentT;
            Eigen::Vector2f lowUv = previousUv;
            for (int iteration = 0; iteration < 20; ++iteration)
            {
                const float middle = (low + high) * 0.5f;
                Eigen::Vector2f middleUv;
                const float middleDelta = TerrainBrushHeightDelta(
                    plan, localOrigin, localDirection, middle, middleUv);
                if (std::abs(middleDelta) < 0.01f)
                {
                    low = high = middle;
                    lowUv = middleUv;
                    break;
                }
                if ((previousDelta > 0.0f && middleDelta > 0.0f) ||
                    (previousDelta < 0.0f && middleDelta < 0.0f))
                {
                    low = middle;
                    lowUv = middleUv;
                }
                else
                {
                    high = middle;
                }
            }
            result.valid = true;
            result.uv = lowUv;
            result.pixel = pixel;
            result.worldPosition = (worldFromLocal *
                (localOrigin + localDirection * low).homogeneous()).head<3>();
            return result;
        }
        previousT = currentT;
        previousUv = currentUv;
        previousDelta = currentDelta;
    }
    return result;
}

// 圆环的屏幕半径按相机投影换算；世界半径取半径乘地形最大水平缩放，
// 只服务编辑器视觉反馈，不参与 brush 的 UV 半径计算。
inline float TerrainBrushRadiusPixels(const EditorSceneCameraActor& camera,
    float width, float height, const Eigen::Vector3f& centerWorld,
    float radiusCm, const Eigen::Vector3f& scale)
{
    if (!BlueprintViewportDetail::ValidCamera(camera, width, height) ||
        !centerWorld.allFinite() || !scale.allFinite() ||
        !std::isfinite(radiusCm) || radiusCm <= 0.0f)
        return 0.0f;
    const float worldRadius = radiusCm *
        std::max(std::abs(scale.x()), std::abs(scale.z()));
    Eigen::Vector2f centerPixel;
    Eigen::Vector2f edgePixel;
    if (!ProjectBlueprintViewportPoint(camera, width, height, centerWorld, centerPixel) ||
        !ProjectBlueprintViewportPoint(camera, width, height,
            centerWorld + camera.right * worldRadius, edgePixel))
        return 0.0f;
    const float radius = (edgePixel - centerPixel).norm();
    return std::isfinite(radius) ? radius : 0.0f;
}

} // namespace VL::EditorUi
