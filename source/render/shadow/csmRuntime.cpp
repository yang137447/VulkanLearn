#include "render/shadow/csmRuntime.h"

#include <algorithm>
#include <cmath>

#include "commonFunction.h"
#include "profiler.h"
#include "render/frontend/renderScene.h"
#include "sceneNode.h"

namespace VL
{

void CsmRuntime::SetSettings(const VL::CsmSettings& settings)
{
    std::lock_guard<std::mutex> lock(csmSettingsMutex);
    csmSettings = settings;
    csmSettingsRevision.fetch_add(1, std::memory_order_release);
}

VL::CsmSettings CsmRuntime::GetSettings() const
{
    std::lock_guard<std::mutex> lock(csmSettingsMutex);
    return csmSettings;
}

void CsmRuntime::SnapshotFrameSettings()
{
    VL::CsmSettings snapshot;
    uint64_t revision = 0;
    {
        std::lock_guard<std::mutex> lock(csmSettingsMutex);
        snapshot = csmSettings;
        revision = csmSettingsRevision.load(std::memory_order_acquire);
    }
    if (frameCsmSettingsInitialized && revision == frameCsmSettingsRevision)
    {
        return;
    }
    frameCsmSettings = snapshot;
    frameCsmSettingsRevision = revision;
    frameCsmSettingsInitialized = true;
    // RT-owned cascade cache: RT is the only writer.
    shadowCascadeFrameData.valid = false;
}

bool CsmRuntime::SetCastShadows(bool enabled, std::string& outMessage)
{
    {
        std::lock_guard<std::mutex> lock(csmSettingsMutex);
        csmSettings.castShadows = enabled;
        csmSettingsRevision.fetch_add(1, std::memory_order_release);
    }
    outMessage = std::string("CSM ") + (enabled ? "enabled." : "disabled.");
    return true;
}

bool CsmRuntime::SetDynamicShadowDistance(
    float distance,
    std::string& outMessage)
{
    if (distance <= 0.0f)
    {
        outMessage = "CSM shadow distance must be positive.";
        return false;
    }

    {
        std::lock_guard<std::mutex> lock(csmSettingsMutex);
        csmSettings.dynamicShadowDistance = distance;
        csmSettingsRevision.fetch_add(1, std::memory_order_release);
    }
    outMessage = "CSM shadow distance set to " + std::to_string(distance);
    return true;
}

bool CsmRuntime::SetCascadeCount(
    uint32_t cascadeCount,
    std::string& outMessage)
{
    if (cascadeCount < 1 ||
        cascadeCount > VL::CsmSettings::MaxCascadeCount)
    {
        outMessage = "CSM cascade count must be in the range [1, 4].";
        return false;
    }

    {
        std::lock_guard<std::mutex> lock(csmSettingsMutex);
        csmSettings.cascadeCount = cascadeCount;
        csmSettingsRevision.fetch_add(1, std::memory_order_release);
    }
    outMessage =
        "CSM cascade count set to " +
        std::to_string(cascadeCount);
    return true;
}

bool CsmRuntime::SetCascadeDistributionExponent(
    float exponent,
    std::string& outMessage)
{
    if (exponent < 1.0f || exponent > 10.0f)
    {
        outMessage =
            "CSM cascade distribution exponent must be in the range [1, 10].";
        return false;
    }

    {
        std::lock_guard<std::mutex> lock(csmSettingsMutex);
        csmSettings.cascadeDistributionExponent = exponent;
        csmSettingsRevision.fetch_add(1, std::memory_order_release);
    }
    outMessage =
        "CSM cascade distribution exponent set to " +
        std::to_string(exponent);
    return true;
}

bool CsmRuntime::SetCascadeTransitionFraction(
    float fraction,
    std::string& outMessage)
{
    if (fraction < 0.0f || fraction > 1.0f)
    {
        outMessage =
            "CSM cascade transition fraction must be in the range [0, 1].";
        return false;
    }

    {
        std::lock_guard<std::mutex> lock(csmSettingsMutex);
        csmSettings.cascadeTransitionFraction = fraction;
        csmSettingsRevision.fetch_add(1, std::memory_order_release);
    }
    outMessage =
        "CSM cascade transition fraction set to " +
        std::to_string(fraction);
    return true;
}

bool CsmRuntime::SetShadowDistanceFadeoutFraction(
    float fraction,
    std::string& outMessage)
{
    if (fraction < 0.0f || fraction > 1.0f)
    {
        outMessage =
            "CSM shadow distance fadeout fraction must be in the range [0, 1].";
        return false;
    }

    {
        std::lock_guard<std::mutex> lock(csmSettingsMutex);
        csmSettings.shadowDistanceFadeoutFraction = fraction;
        csmSettingsRevision.fetch_add(1, std::memory_order_release);
    }
    outMessage =
        "CSM shadow distance fadeout fraction set to " +
        std::to_string(fraction);
    return true;
}

bool CsmRuntime::SetShadowBias(
    float value,
    std::string& outMessage)
{
    if (value < 0.0f || value > 1.0f)
    {
        outMessage = "CSM shadow bias must be in the range [0, 1].";
        return false;
    }

    {
        std::lock_guard<std::mutex> lock(csmSettingsMutex);
        csmSettings.shadowBias = value;
        csmSettingsRevision.fetch_add(1, std::memory_order_release);
    }
    outMessage = "CSM shadow bias set to " + std::to_string(value);
    return true;
}

bool CsmRuntime::SetShadowSlopeBias(
    float value,
    std::string& outMessage)
{
    if (value < 0.0f || value > 1.0f)
    {
        outMessage = "CSM shadow slope bias must be in the range [0, 1].";
        return false;
    }

    {
        std::lock_guard<std::mutex> lock(csmSettingsMutex);
        csmSettings.shadowSlopeBias = value;
        csmSettingsRevision.fetch_add(1, std::memory_order_release);
    }
    outMessage =
        "CSM shadow slope bias set to " +
        std::to_string(value);
    return true;
}

bool CsmRuntime::SetShadowCascadeBiasDistribution(
    float value,
    std::string& outMessage)
{
    if (value < 0.0f || value > 1.0f)
    {
        outMessage =
            "CSM shadow cascade bias distribution must be in the range [0, 1].";
        return false;
    }

    {
        std::lock_guard<std::mutex> lock(csmSettingsMutex);
        csmSettings.shadowCascadeBiasDistribution = value;
        csmSettingsRevision.fetch_add(1, std::memory_order_release);
    }
    outMessage =
        "CSM shadow cascade bias distribution set to " +
        std::to_string(value);
    return true;
}

bool CsmRuntime::IsEnabled() const noexcept
{
    return frameCsmSettings.castShadows;
}

bool CsmRuntime::IsCascadeActive(
    uint32_t cascadeIndex) const noexcept
{
    return frameCsmSettings.castShadows &&
        cascadeIndex < frameCsmSettings.cascadeCount;
}

bool CsmRuntime::GetCascadeLightViewProjection(
    uint32_t cascadeIndex,
    Eigen::Matrix4f& outLightViewProjection) const
{
    // shadowCascadeFrameData 在 UpdateShadowGlobalUBOForPass 里按本帧、本 pass
    // 分辨率构建；这里返回的矩阵与刚写入 shadow UBO 的级联完全同源。
    if (!shadowCascadeFrameData.valid ||
        cascadeIndex >= frameCsmSettings.cascadeCount ||
        cascadeIndex >= VL::CsmSettings::MaxCascadeCount)
    {
        return false;
    }

    const Eigen::Matrix4f& lightViewProjection =
        shadowCascadeFrameData.lightViewProj[cascadeIndex];
    if (!lightViewProjection.allFinite())
    {
        return false;
    }

    outLightViewProjection = lightViewProjection;
    return true;
}

bool CsmRuntime::GetCascadeCasterReceiverBounds(
    uint32_t cascadeIndex,
    VL::ShadowCasterReceiverBounds& outBounds) const
{
    if (!shadowCascadeFrameData.valid ||
        cascadeIndex >= frameCsmSettings.cascadeCount ||
        cascadeIndex >= VL::CsmSettings::MaxCascadeCount)
    {
        return false;
    }

    outBounds = shadowCascadeFrameData.casterReceiverBounds[cascadeIndex];
    return outBounds.valid &&
        outBounds.worldToLight.allFinite() &&
        outBounds.lightSpaceXyMinMax.allFinite();
}

void CsmRuntime::BuildFrameData(
    const RenderScene& renderScene,
    uint64_t frameIndex,
    uint32_t passSizeWidth,
    uint32_t passSizeHeight)
{
    if (!frameCsmSettingsInitialized)
    {
        SnapshotFrameSettings();
    }
    // 如果本帧已经计算过相同场景、相同分辨率的 cascade 数据，直接复用缓存
    if (shadowCascadeFrameData.valid &&
        shadowCascadeFrameData.frameIndex == frameIndex &&
        shadowCascadeFrameData.worldGeneration == renderScene.worldGeneration &&
        shadowCascadeFrameData.width == passSizeWidth &&
        shadowCascadeFrameData.height == passSizeHeight)
    {
        return;
    }

    const VL::CameraSnapshot& camera = renderScene.camera;
    const float cascadeNear = camera.clipNear;
    const float cascadeFar =
        std::max(
            cascadeNear + 0.1f,
            frameCsmSettings.dynamicShadowDistance);

    shadowCascadeFrameData.view.fill(Eigen::Matrix4f::Identity());
    shadowCascadeFrameData.projection.fill(Eigen::Matrix4f::Identity());
    shadowCascadeFrameData.lightViewProj.fill(Eigen::Matrix4f::Identity());
    shadowCascadeFrameData.cascadeSplits = Eigen::Vector4f::Zero();
    shadowCascadeFrameData.bias.fill(Eigen::Vector4f::Zero());
    // 本帧 caster 的光源空间包围盒要重新投影：角色/头发/风每帧都可能动。
    casterLightSpaceBoundsValid = false;
    shadowCascadeFrameData.csmParameters = Eigen::Vector4f(
        frameCsmSettings.cascadeTransitionFraction,
        frameCsmSettings.shadowDistanceFadeoutFraction,
        static_cast<float>(frameCsmSettings.cascadeCount),
        frameCsmSettings.dynamicShadowDistance);

    float previousSplit = cascadeNear;
    for (uint32_t cascadeIndex = 0;
         cascadeIndex < frameCsmSettings.cascadeCount;
         ++cascadeIndex)
    {
        const float p = static_cast<float>(cascadeIndex + 1) /
            static_cast<float>(frameCsmSettings.cascadeCount);
        // UE 风格 Distribution Exponent：指数越大，越多分辨率集中到相机附近。
        // 最后一级始终精确落在 Dynamic Shadow Distance。
        const float splitFraction =
            std::pow(
                p,
                frameCsmSettings.cascadeDistributionExponent);
        const float splitFar =
            cascadeNear +
            (cascadeFar - cascadeNear) * splitFraction;

        ShadowProjectionParams params = CalculateShadowMatrixForCameraRange(
            renderScene,
            previousSplit,
            splitFar,
            passSizeWidth,
            passSizeHeight);
        shadowCascadeFrameData.view[cascadeIndex] = params.viewMatrix;
        shadowCascadeFrameData.projection[cascadeIndex] = params.projectionMatrix;
        shadowCascadeFrameData.lightViewProj[cascadeIndex] = params.projectionMatrix * params.viewMatrix;
        shadowCascadeFrameData.casterReceiverBounds[cascadeIndex] =
            params.casterReceiverBounds;
        shadowCascadeFrameData.cascadeSplits[cascadeIndex] = splitFar;

        // 用户只编辑 UE 风格的全局 Bias。每级实际接收面 Bias 在这里派生，
        // Distribution 为 0 时各级相同，为 1 时按级联序号逐级增大。
        const float cascadeBiasScale =
            1.0f +
            frameCsmSettings.shadowCascadeBiasDistribution *
                static_cast<float>(cascadeIndex);
        shadowCascadeFrameData.bias[cascadeIndex] =
            Eigen::Vector4f(
                frameCsmSettings.shadowBias *
                    VL::CsmSettings::MaxReceiverDepthBias *
                    cascadeBiasScale,
                frameCsmSettings.shadowSlopeBias *
                    VL::CsmSettings::MaxSlopeBiasMultiplier,
                0.0f,
                0.0f);
        previousSplit = splitFar;
    }

    for (uint32_t cascadeIndex = frameCsmSettings.cascadeCount;
         cascadeIndex < VL::CsmSettings::MaxCascadeCount;
         ++cascadeIndex)
    {
        shadowCascadeFrameData.cascadeSplits[cascadeIndex] =
            cascadeFar;
    }

    shadowCascadeFrameData.frameIndex = frameIndex;
    shadowCascadeFrameData.worldGeneration = renderScene.worldGeneration;
    shadowCascadeFrameData.width = passSizeWidth;
    shadowCascadeFrameData.height = passSizeHeight;
    shadowCascadeFrameData.valid = true;
}

ShadowProjectionParams CsmRuntime::CalculateShadowMatrixForCameraRange(
    const RenderScene& renderScene,
    float splitNear,
    float splitFar,
    uint32_t passSizeWidth,
    uint32_t passSizeHeight)
{
    PROFILE_FUNCTION();

    // 根据相机级联范围 (splitNear, splitFar) 计算当前级联的阴影投影矩阵。
    //
    // 流程：
    // 1. 从 camera 构建视锥体 8 个角点（世界空间）
    // 2. 将角点变换到光源空间 (worldToShadowMatrix)
    // 3. 计算光源空间 XY 范围 + Z 深度范围
    // 4. 可选：遍历场景 drawPacket 收紧 Z 范围 (ComputeCascadeLightSpaceZBounds)
    // 5. 根据当前 ShadowStrategy 委托具体算法：
    //    - DynamicTightBox  : 凸包 + 旋转边最小包围矩形
    //    - StableBoundingSphere : 视锥体外接球 + texel snapping
    //    - StableRectangular    : 光源空间 AABB + texel snapping
    const VL::CameraSnapshot& camera = renderScene.camera;
    Eigen::Vector3f cameraPosition = camera.position;
    Eigen::Vector3f cameraDirection = camera.forward;
        // near, far
    float cameraNear = splitNear;
    float cameraFar = splitFar;

    Eigen::Vector3f cameraRight = camera.right;
    Eigen::Vector3f cameraUp = camera.up;
    float cameraHFov = camera.horizontalFovDegrees;
    // 阴影视锥必须与冻结相机投影一致；停靠视口不再采用整窗口宽高比。
    float aspect = std::abs(camera.projection(1, 1) / camera.projection(0, 0));
    float cameraHFovRad = cameraHFov * static_cast<float>(M_PI) / 180.0f;

    float tanHalfFov = std::tan(cameraHFovRad * 0.5f);
    float nearHalfWidth = tanHalfFov * cameraNear;
    float nearHalfHeight = nearHalfWidth / aspect;
    float farHalfWidth = tanHalfFov * cameraFar;
    float farHalfHeight = farHalfWidth / aspect;

    Eigen::Vector3f nearCenter = cameraPosition + cameraDirection * cameraNear;
    Eigen::Vector3f farCenter = cameraPosition + cameraDirection * cameraFar;

    std::vector<Eigen::Vector3f> frustumPoints;
    frustumPoints.reserve(8);
    frustumPoints.emplace_back(nearCenter + cameraUp * nearHalfHeight - cameraRight * nearHalfWidth);
    frustumPoints.emplace_back(nearCenter + cameraUp * nearHalfHeight + cameraRight * nearHalfWidth);
    frustumPoints.emplace_back(nearCenter - cameraUp * nearHalfHeight - cameraRight * nearHalfWidth);
    frustumPoints.emplace_back(nearCenter - cameraUp * nearHalfHeight + cameraRight * nearHalfWidth);
    frustumPoints.emplace_back(farCenter + cameraUp * farHalfHeight - cameraRight * farHalfWidth);
    frustumPoints.emplace_back(farCenter + cameraUp * farHalfHeight + cameraRight * farHalfWidth);
    frustumPoints.emplace_back(farCenter - cameraUp * farHalfHeight - cameraRight * farHalfWidth);
    frustumPoints.emplace_back(farCenter - cameraUp * farHalfHeight + cameraRight * farHalfWidth);
        
    // 2. 计算阴影映射相机的位置
        // 2.1以世界中心为原点，使用光源的旋转矩阵 构建shadowCoordinateSystem
    const VL::LightSnapshot* directionalLight = nullptr;
    for (const VL::LightSnapshot& light : renderScene.lights)
    {
        if (light.type == VL::LightSnapshotType::Directional)
        {
            directionalLight = &light;
            break;
        }
    }
    if (!directionalLight)
    {
        ShadowProjectionParams params;
        params.viewMatrix = Eigen::Matrix4f::Identity();
        params.projectionMatrix = Eigen::Matrix4f::Identity();
        return params;
    }
        // 2.1构建worldCoordinateSystem -> shadowCoordinateSystem
    Eigen::Matrix3f worldToShadowMatrix = directionalLight->worldToLight;

    // 2.3将点转换到shadowCoordinateSystem
    std::vector<Eigen::Vector3f> pointsInShadowSys;
    pointsInShadowSys.reserve(frustumPoints.size());
    for (const auto& point : frustumPoints)
    {
        pointsInShadowSys.emplace_back(worldToShadowMatrix * point);
    }
    
    // ====================================================================================================
    // STABILIZATION LOGIC START
    // ====================================================================================================
    ShadowStrategy strategy = ShadowStrategy::StableBoundingSphere; // Default strategy: StableBoundingSphere (Safest)
    // Note: StableRectangular requires handling non-square shadow maps or viewport resizing to maintain isotropic texel density.
    // Since our physical shadow map texture is likely square, mapping a rectangular projection to it causes stretching/anisotropy.
    
    ShadowProjectionParams params;

    // Calculate Z bounds first as they are needed for all strategies
    float minX = std::numeric_limits<float>::max();
    float maxX = std::numeric_limits<float>::lowest();
    float minY = std::numeric_limits<float>::max();
    float maxY = std::numeric_limits<float>::lowest();
    float defaultMinZ = std::numeric_limits<float>::max();
    float defaultMaxZ = std::numeric_limits<float>::lowest();
    for(const auto& point : pointsInShadowSys)
    {
        maxX = std::max(maxX, point.x());
        minX = std::min(minX, point.x());
        maxY = std::max(maxY, point.y());
        minY = std::min(minY, point.y());
        defaultMaxZ = std::max(defaultMaxZ, point.z());
        defaultMinZ = std::min(defaultMinZ, point.z());
    }

    float minZ = defaultMinZ;
    float maxZ = defaultMaxZ;
    if (frameCsmSettings.lightSpaceCasterBounds)
    {
        // Performance note: this scans all draw packets once per cascade. Current scenes are small
        // enough for this straightforward path; if profiling shows CPU pressure here, precompute
        // light-space draw bounds once per shadow frame and reuse them across cascades.
        const auto cascadeLightSpaceZBounds = ComputeCascadeLightSpaceZBounds(
            renderScene,
            worldToShadowMatrix,
            minX,
            maxX,
            minY,
            maxY);
        if (cascadeLightSpaceZBounds.has_value())
        {
            minZ = cascadeLightSpaceZBounds->first;
            maxZ = cascadeLightSpaceZBounds->second;
        }
    }

    float zNear = 0.0f;
    float zFar = maxZ - minZ;
    if (zFar < 0.1f) zFar = 1.0f;

    switch (strategy)
    {
    case ShadowStrategy::DynamicTightBox:
        params = CalculateShadowMatrix_DynamicTight(pointsInShadowSys, worldToShadowMatrix.block<3, 3>(0, 0), maxZ, zFar);
        break;
    case ShadowStrategy::StableBoundingSphere:
        params = CalculateShadowMatrix_StableSphere(pointsInShadowSys, worldToShadowMatrix.block<3, 3>(0, 0), static_cast<float>(passSizeWidth), maxZ, zFar);
        break;
    case ShadowStrategy::StableRectangular:
        params = CalculateShadowMatrix_StableRectangular(pointsInShadowSys, worldToShadowMatrix.block<3, 3>(0, 0), static_cast<float>(passSizeWidth), maxZ, zFar);
        break;
    }

    // 接收面范围取自相机 split 的 8 个角点，和阴影拟合使用同一份
    // light-space 坐标；caster 预筛不依赖后续按 caster 扩张的 Z 范围。
    params.casterReceiverBounds.worldToLight = worldToShadowMatrix;
    params.casterReceiverBounds.lightSpaceXyMinMax =
        Eigen::Vector4f(minX, minY, maxX, maxY);
    params.casterReceiverBounds.valid = true;

    return params;
}

// ====================================================================================================
// Shadow Strategy Implementations
// ====================================================================================================

// DynamicTightBox：凸包 + 最小包围矩形。对光源空间视锥体的 XY 投影点计算凸包，
// 遍历每条凸包边作为候选方向，在该方向上求 AABB，选取边长最小的方向，得到
// 最小面积包围矩形。然后构建正交投影矩阵。
//
// 算法步骤：
//   ->shadowCoordinateSystem
//       获取凸包点序 (ConvexHull)
//       对每条边建立 EdgeCoordinateSystem (2D 坐标系)
//       ->EdgeCoordinateSystem
//           将凸包点转换到 EdgeCoordinateSystem
//           计算 AABB，获取 maxEdgeLength
//           在循环中找到最小的 maxEdgeLength → 记录 CenterInEdge
//       <-EdgeCoordinateSystem
//       将 CenterInEdge 转回 shadowCoordinateSystem
//       计算 ShadowCameraPosition、ZNear、ZFar
//   <-shadowCoordinateSystem
//   转回世界空间，构建 view/projection 矩阵
ShadowProjectionParams CsmRuntime::CalculateShadowMatrix_DynamicTight(
    const std::vector<Eigen::Vector3f>& pointsInShadowSys,
    const Eigen::Matrix3f& worldToShadowRotation,
    float sceneMaxZ, 
    float sceneZRange)
{
    PROFILE_FUNCTION();
    // 1. 进行凸包点查找，获取index
    std::vector<uint32_t> cullPointIndex;
    cullPointIndex = Algorithm::ConvexHull(pointsInShadowSys);
    
    // 2. 根据这些凸包点来计算面积最小的方形区域（等价于边长最小的方形区域）
    float minLength = std::numeric_limits<float>::max();
    Eigen::Vector2f centerInEdgeCoord;
    Eigen::Matrix2f shadowToEdgeCoordMatrix = Eigen::Matrix2f::Identity();
    Eigen::Vector2f EdgeCoordOriginInShadowSys;
    
    // 遍历所有边，将平面坐标系旋转到该边的方向，计算方形的aabb, 求得面积
    // 这样就能找到最小的方形区域
    for (size_t i = 0; i < cullPointIndex.size(); i++)
    {
        // 2.4.1 以p1为中心，p2-p1为方向，构建平面坐标系
            uint32_t p1Idx = cullPointIndex[i];
            uint32_t p2Idx = cullPointIndex[(i + 1) % cullPointIndex.size()];
            Eigen::Vector3f p1 = pointsInShadowSys[p1Idx];
            Eigen::Vector3f p2 = pointsInShadowSys[p2Idx];

            Eigen::Vector2f edge(p2.x() - p1.x(), p2.y() - p1.y());
            if (edge.norm() < 1e-6) continue;
            edge.normalize();

            // R = [ c  -s ]  这里是左乘
            //     [ s c ]
            float c = edge.x();
            float s = edge.y();
            Eigen::Matrix2f _shadowToEdgeCoordMatrix;
            _shadowToEdgeCoordMatrix << c, -s,
                        s, c;
            // 2.4.2 求取该平面变换后的aabb
            float minX = std::numeric_limits<float>::max();
            float maxX = std::numeric_limits<float>::lowest();
            float minY = std::numeric_limits<float>::max();
            float maxY = std::numeric_limits<float>::lowest();

            for (auto idx : cullPointIndex)
            {
                Eigen::Vector2f rotatedP = _shadowToEdgeCoordMatrix * (Eigen::Vector2f(pointsInShadowSys[idx].x(), pointsInShadowSys[idx].y()) - Eigen::Vector2f(p1.x(), p1.y()));
                minX = std::min(minX, rotatedP.x());
                maxX = std::max(maxX, rotatedP.x());
                minY = std::min(minY, rotatedP.y());
                maxY = std::max(maxY, rotatedP.y());
            }
            float width = std::abs(maxX - minX);
            float height = std::abs(maxY - minY);
            float length = std::max(width, height);
            // 2.4.3 有最小的length, 则找到了最小的方形区域
            if(length < minLength)
            {
                minLength = length;
                centerInEdgeCoord = Eigen::Vector2f((minX + maxX) / 2.0f, (minY + maxY) / 2.0f);
                shadowToEdgeCoordMatrix = _shadowToEdgeCoordMatrix;
                EdgeCoordOriginInShadowSys = Eigen::Vector2f(p1.x(), p1.y());
            }
    }
    
    // 3. 计算中心点的位置
    // x, y 在shadowCoordinateSystem中的值
    Eigen::Vector2f center2DInShadowSys = shadowToEdgeCoordMatrix.transpose() * centerInEdgeCoord + EdgeCoordOriginInShadowSys;

    Eigen::Vector3f ShadowCameraPositionInShadowSys = Eigen::Vector3f(center2DInShadowSys.x(), center2DInShadowSys.y(), sceneMaxZ);
    
    // 4. 计算shadowCamera的位置 (World Space)。pointsInShadowSys 已经在 light-aligned shadow space，
    // worldToShadowRotation 用于把拟合出的 shadow-space camera 位置和方向转回世界空间。
    Eigen::Vector3f shadowCameraPosition = worldToShadowRotation.transpose() * ShadowCameraPositionInShadowSys;

    // 更新Camera
    Camera shadowCamera;
    shadowCamera.SetCamera(shadowCameraPosition, CommonFunction::QuatToRotation(Eigen::Quaternionf(worldToShadowRotation.transpose())));

    // 计算Near/Far
    float nearPlane = 0.0f;
    float farPlane = sceneZRange;

    shadowCamera.SetOrthographic(
        minLength, 
        1.0f, // Aspect ratio
        nearPlane, 
        farPlane);

    ShadowProjectionParams params;
    
    // Apply the extra rotation (rollMatrix) to the View Matrix
    Eigen::Matrix4f rollMatrix = Eigen::Matrix4f::Identity();
    rollMatrix(0, 0) = shadowToEdgeCoordMatrix(0, 0);
    rollMatrix(0, 1) = shadowToEdgeCoordMatrix(0, 1);
    rollMatrix(1, 0) = shadowToEdgeCoordMatrix(1, 0);
    rollMatrix(1, 1) = shadowToEdgeCoordMatrix(1, 1);
    
    params.viewMatrix = rollMatrix * shadowCamera.GetViewMatrix();
    params.projectionMatrix = shadowCamera.GetProjectionMatrix();
    return params;
}

ShadowProjectionParams CsmRuntime::CalculateShadowMatrix_StableSphere(
    const std::vector<Eigen::Vector3f>& pointsInShadowSys,
    const Eigen::Matrix3f& worldToShadowRotation, 
    float shadowMapResolution, 
    float sceneMaxZ, 
    float sceneZRange)
{
    PROFILE_FUNCTION();
    // 1. Calculate Frustum Center
    Eigen::Vector3f frustumCenter = Eigen::Vector3f::Zero();
    for(const auto& p : pointsInShadowSys) {
        frustumCenter += p;
    }
    frustumCenter /= static_cast<float>(pointsInShadowSys.size());

    // 2. Calculate Radius
    float radius = 0.0f;
    for(const auto& p : pointsInShadowSys) {
        radius = std::max(radius, (p - frustumCenter).norm());
    }
    radius = std::ceil(radius * 16.0f) / 16.0f;

    // 3. Snapping
    float worldUnitsPerTexel = (2.0f * radius) / shadowMapResolution;
    frustumCenter.x() = std::floor(frustumCenter.x() / worldUnitsPerTexel) * worldUnitsPerTexel;
    frustumCenter.y() = std::floor(frustumCenter.y() / worldUnitsPerTexel) * worldUnitsPerTexel;

    // 4. Construct Camera
    // Camera placed at the center of the sphere in X/Y, but at sceneMaxZ in Z
    Eigen::Vector3f shadowCameraPosition = frustumCenter;
    shadowCameraPosition.z() = sceneMaxZ;
    
    // Transform back to World Space for Camera.SetCamera
    Eigen::Vector3f shadowCameraPositionWorld = worldToShadowRotation.transpose() * shadowCameraPosition;
    
    Camera shadowCamera;
    shadowCamera.SetCamera(shadowCameraPositionWorld, CommonFunction::QuatToRotation(Eigen::Quaternionf(worldToShadowRotation.transpose()))); // Use Light Rotation

    // 5. Construct Projection
    shadowCamera.SetOrthographic(
        radius * 2.0f, 
        1.0f, // Aspect ratio is 1.0
        0.0f, // Near plane relative to camera
        sceneZRange // Far plane
    );

    ShadowProjectionParams params;
    params.viewMatrix = shadowCamera.GetViewMatrix();
    params.projectionMatrix = shadowCamera.GetProjectionMatrix();
    return params;
}

ShadowProjectionParams CsmRuntime::CalculateShadowMatrix_StableRectangular(
    const std::vector<Eigen::Vector3f>& pointsInShadowSys,
    const Eigen::Matrix3f& worldToShadowRotation, 
    float shadowMapResolution, 
    float sceneMaxZ, 
    float sceneZRange)
{
    PROFILE_FUNCTION();
    // 1. Calculate AABB of Frustum Slice in Shadow Space
    float minX = std::numeric_limits<float>::max();
    float maxX = std::numeric_limits<float>::lowest();
    float minY = std::numeric_limits<float>::max();
    float maxY = std::numeric_limits<float>::lowest();

    for(const auto& p : pointsInShadowSys) {
        minX = std::min(minX, p.x());
        maxX = std::max(maxX, p.x());
        minY = std::min(minY, p.y());
        maxY = std::max(maxY, p.y());
    }

    // StableRectangular keeps a tight light-space AABB and snaps its edges to
    // texel units. It is less rotation-stable than the sphere path, but uses
    // more of the shadow map for the current frustum.
    Eigen::Vector3f diagonal = pointsInShadowSys[6] - pointsInShadowSys[0];
    float diagonalLength = diagonal.norm();
    float worldUnitsPerTexel = diagonalLength / shadowMapResolution;
    
    minX = std::floor(minX / worldUnitsPerTexel) * worldUnitsPerTexel;
    maxX = std::floor(maxX / worldUnitsPerTexel) * worldUnitsPerTexel;
    minY = std::floor(minY / worldUnitsPerTexel) * worldUnitsPerTexel;
    maxY = std::floor(maxY / worldUnitsPerTexel) * worldUnitsPerTexel;
    
    float width = maxX - minX;
    float height = maxY - minY;
    float centerX = (minX + maxX) * 0.5f;
    float centerY = (minY + maxY) * 0.5f;

    // 3. Construct Camera
    Eigen::Vector3f shadowCameraPosition = Eigen::Vector3f(centerX, centerY, sceneMaxZ);
    Eigen::Vector3f shadowCameraPositionWorld = worldToShadowRotation.transpose() * shadowCameraPosition;

    Camera shadowCamera;
    shadowCamera.SetCamera(shadowCameraPositionWorld, CommonFunction::QuatToRotation(Eigen::Quaternionf(worldToShadowRotation.transpose())));

    float aspect = width / height;

    // Camera::SetOrthographic uses horizontal coverage when paired with
    // aspect = width / height, matching the fitted light-space AABB.
    shadowCamera.SetOrthographic(
        width,
        aspect,
        0.0f,
        sceneZRange
    );

    ShadowProjectionParams params;
    params.viewMatrix = shadowCamera.GetViewMatrix();
    params.projectionMatrix = shadowCamera.GetProjectionMatrix();
    return params;
}

std::pair<float, float> ComputeMinMaxAlongAxis(
    const Eigen::Vector3f& aabbMin,
    const Eigen::Vector3f& aabbMax,
    const Eigen::Vector3f& axis)
{
    Eigen::Vector3f dir = axis;
    float len = dir.norm();
    if (len > 0.0f)
    {
        dir /= len;
    }
    std::array<Eigen::Vector3f, 8> corners = {
        Eigen::Vector3f(aabbMin.x(), aabbMin.y(), aabbMin.z()),
        Eigen::Vector3f(aabbMax.x(), aabbMin.y(), aabbMin.z()),
        Eigen::Vector3f(aabbMin.x(), aabbMax.y(), aabbMin.z()),
        Eigen::Vector3f(aabbMax.x(), aabbMax.y(), aabbMin.z()),
        Eigen::Vector3f(aabbMin.x(), aabbMin.y(), aabbMax.z()),
        Eigen::Vector3f(aabbMax.x(), aabbMin.y(), aabbMax.z()),
        Eigen::Vector3f(aabbMin.x(), aabbMax.y(), aabbMax.z()),
        Eigen::Vector3f(aabbMax.x(), aabbMax.y(), aabbMax.z())
    };
    float minProj = dir.dot(corners[0]);
    float maxProj = minProj;
    for (size_t i = 1; i < corners.size(); ++i)
    {
        float proj = dir.dot(corners[i]);
        minProj = std::min(minProj, proj);
        maxProj = std::max(maxProj, proj);
    }
    return { minProj, maxProj };
}

std::optional<std::pair<float, float>> CsmRuntime::ComputeCascadeLightSpaceZBounds(
    const RenderScene& renderScene,
    const Eigen::Matrix3f& worldToShadowMatrix,
    float minX,
    float maxX,
    float minY,
    float maxY)
{
    if (renderScene.drawPackets.empty())
    {
        return std::nullopt;
    }

    // 投影只取决于光源朝向：四个级联共用同一份 worldToShadowMatrix，因此每帧
    // 只投影一次并缓存；逐级联只按相机 slice 的 XY 范围过滤，再合并 Z。
    // 之前每个级联都重扫全部 draw packet，等于 4 倍的投影计算。
    if (!casterLightSpaceBoundsValid)
    {
        const Eigen::Vector3f lightSpaceXAxis = worldToShadowMatrix.row(0).transpose();
        const Eigen::Vector3f lightSpaceYAxis = worldToShadowMatrix.row(1).transpose();
        const Eigen::Vector3f lightSpaceZAxis = worldToShadowMatrix.row(2).transpose();
        const Eigen::Vector3f absLightSpaceXAxis = lightSpaceXAxis.cwiseAbs();
        const Eigen::Vector3f absLightSpaceYAxis = lightSpaceYAxis.cwiseAbs();
        const Eigen::Vector3f absLightSpaceZAxis = lightSpaceZAxis.cwiseAbs();

        casterLightSpaceBounds.clear();
        casterLightSpaceBounds.reserve(renderScene.drawPackets.size());
        for (const VL::RenderDrawPacket& drawPacket : renderScene.drawPackets)
        {
            // Current limitation: SpeedTree world bounds describe the undeformed
            // mesh and exclude vertex-shader wind displacement. The foliage bounds
            // contract must eventually provide expanded caster bounds so strong
            // wind cannot clip branch shadows from the CSM light-space Z range.
            const Eigen::Vector3f center =
                (drawPacket.worldBoundsMin + drawPacket.worldBoundsMax) * 0.5f;
            const Eigen::Vector3f extent =
                (drawPacket.worldBoundsMax - drawPacket.worldBoundsMin) * 0.5f;

            ShadowCasterLightSpaceBounds bounds;
            const float projectedCenterX = lightSpaceXAxis.dot(center);
            const float projectedRadiusX = absLightSpaceXAxis.dot(extent);
            bounds.minX = projectedCenterX - projectedRadiusX;
            bounds.maxX = projectedCenterX + projectedRadiusX;
            const float projectedCenterY = lightSpaceYAxis.dot(center);
            const float projectedRadiusY = absLightSpaceYAxis.dot(extent);
            bounds.minY = projectedCenterY - projectedRadiusY;
            bounds.maxY = projectedCenterY + projectedRadiusY;
            const float projectedCenterZ = lightSpaceZAxis.dot(center);
            const float projectedRadiusZ = absLightSpaceZAxis.dot(extent);
            bounds.minZ = projectedCenterZ - projectedRadiusZ;
            bounds.maxZ = projectedCenterZ + projectedRadiusZ;
            casterLightSpaceBounds.push_back(bounds);
        }
        casterLightSpaceBoundsValid = true;
    }

    float minZ = std::numeric_limits<float>::max();
    float maxZ = std::numeric_limits<float>::lowest();
    bool hasCaster = false;
    for (const ShadowCasterLightSpaceBounds& bounds : casterLightSpaceBounds)
    {
        if (bounds.maxX < minX || bounds.minX > maxX)
        {
            continue;
        }
        if (bounds.maxY < minY || bounds.minY > maxY)
        {
            continue;
        }
        minZ = std::min(minZ, bounds.minZ);
        maxZ = std::max(maxZ, bounds.maxZ);
        hasCaster = true;
    }

    if (!hasCaster)
    {
        return std::nullopt;
    }

    return std::make_pair(minZ, maxZ);
}

std::array<Eigen::Vector3f, 8> BuildWorldCorners(
    const Eigen::Vector3f& localMin,
    const Eigen::Vector3f& localMax,
    const Eigen::Matrix4f& modelMatrix)
{
    return {
        (modelMatrix * Eigen::Vector4f(localMin.x(), localMin.y(), localMin.z(), 1.0f)).head<3>(),
        (modelMatrix * Eigen::Vector4f(localMax.x(), localMin.y(), localMin.z(), 1.0f)).head<3>(),
        (modelMatrix * Eigen::Vector4f(localMin.x(), localMax.y(), localMin.z(), 1.0f)).head<3>(),
        (modelMatrix * Eigen::Vector4f(localMax.x(), localMax.y(), localMin.z(), 1.0f)).head<3>(),
        (modelMatrix * Eigen::Vector4f(localMin.x(), localMin.y(), localMax.z(), 1.0f)).head<3>(),
        (modelMatrix * Eigen::Vector4f(localMax.x(), localMin.y(), localMax.z(), 1.0f)).head<3>(),
        (modelMatrix * Eigen::Vector4f(localMin.x(), localMax.y(), localMax.z(), 1.0f)).head<3>(),
        (modelMatrix * Eigen::Vector4f(localMax.x(), localMax.y(), localMax.z(), 1.0f)).head<3>()
    };
}

void ComputeAabbFromCorners(
    const std::array<Eigen::Vector3f, 8>& corners,
    Eigen::Vector3f& outMin,
    Eigen::Vector3f& outMax)
{
    outMin = corners[0];
    outMax = corners[0];
    for (const auto& corner : corners)
    {
        outMin = outMin.cwiseMin(corner);
        outMax = outMax.cwiseMax(corner);
    }
}

} // namespace VL
