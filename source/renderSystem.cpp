#include "renderSystem.h"
#include <limits>
#include <algorithm>
#include <array>
#include <stdexcept>
#include <string>
#include "core/runtimeResult.h"
#include "sceneNode.h"
#include "materialInstance.h"
#include "material.h"
#include "pipeline/pipelineBase.h"
#include "commonFunction.h"
#include "renderGraph.h"
#include "render/backend/rendererObjectResourceRegistry.h"
#include "render/resource/rendererResourceCache.h"
#include "render/backend/rendererBackendVulkan.h"
#include "shaderReflect.h"
#include "profiler.h"
#include "vulkanDebug.h"
#include "world/world.h"

namespace
{

std::shared_ptr<MaterialInstance> GetPassMaterialInstance(const char* passName)
{
    const auto& renderpasses = RenderGraph::GetInstance().GetRenderpasses();
    auto passIt = renderpasses.find(passName);
    if (passIt == renderpasses.end())
    {
        return nullptr;
    }

    return passIt->second.materialInstance.lock();
}

bool SetPassVectorComponent(
    const char* passName,
    const char* parameterName,
    int componentIndex,
    float value,
    const char* missingPassMessage,
    std::string& outMessage)
{
    std::shared_ptr<MaterialInstance> materialInstance = GetPassMaterialInstance(passName);
    if (!materialInstance)
    {
        outMessage = missingPassMessage;
        return false;
    }

    if (!materialInstance->HasParameter(parameterName))
    {
        outMessage = std::string("Parameter '") + parameterName + "' not found in pass material.";
        return false;
    }

    Eigen::Vector4f params = materialInstance->GetParameter<Eigen::Vector4f>(parameterName);
    params[componentIndex] = value;
    materialInstance->SetParameter(parameterName, params);
    return true;
}

} // namespace

RenderSystem::~RenderSystem()
{
    ShutdownRenderObject();
}

void RenderSystem::SetCsmSettings(const VL::CsmSettings& settings)
{
    csmSettings = settings;
    shadowCascadeFrameData.valid = false;
}

void RenderSystem::SetActiveWorld(std::shared_ptr<const VL::World> world)
{
    activeWorld = std::move(world);
    worldSnapshotQueue.Clear();
    worldSnapshotBuilder.Reset();
    currentRenderScene = VL::RenderScene();
    currentResolvedRenderScene = VL::ResolvedRenderScene();
    hasRenderScene = false;
    initializedRenderWorldGeneration = 0;
    nextSnapshotFrameIndex = 0;
    shadowCascadeFrameData.valid = false;
}

bool RenderSystem::SetToneMappingMode(int mode, std::string& outMessage)
{
    if (!SetPassVectorComponent(
            "toneMapping",
            "u_toneMappingParams",
            3,
            static_cast<float>(mode),
            "Tone mapping pass material instance is expired.",
            outMessage))
    {
        return false;
    }

    outMessage = "Tone mapping mode set to " + std::to_string(mode);
    return true;
}

bool RenderSystem::SetBloomStrength(float value, std::string& outMessage)
{
    if (!SetPassVectorComponent(
            "toneMapping",
            "u_toneMappingParams",
            1,
            value,
            "Tone mapping pass material instance is expired.",
            outMessage))
    {
        return false;
    }

    outMessage = "Bloom strength set to " + std::to_string(value);
    return true;
}

bool RenderSystem::SetBloomThreshold(float value, std::string& outMessage)
{
    if (!SetPassVectorComponent(
            "bloomPrefilter",
            "u_bloomPrefilterParams",
            0,
            value,
            "Bloom prefilter material instance is expired.",
            outMessage))
    {
        return false;
    }

    outMessage = "Bloom threshold set to " + std::to_string(value);
    return true;
}

bool RenderSystem::SetBloomKnee(float value, std::string& outMessage)
{
    if (!SetPassVectorComponent(
            "bloomPrefilter",
            "u_bloomPrefilterParams",
            1,
            value,
            "Bloom prefilter material instance is expired.",
            outMessage))
    {
        return false;
    }

    outMessage = "Bloom knee set to " + std::to_string(value);
    return true;
}

bool RenderSystem::SetBloomClamp(float value, std::string& outMessage)
{
    if (!SetPassVectorComponent(
            "bloomPrefilter",
            "u_bloomPrefilterParams",
            2,
            value,
            "Bloom prefilter material instance is expired.",
            outMessage))
    {
        return false;
    }

    outMessage = "Bloom clamp set to " + std::to_string(value);
    return true;
}

void RenderSystem::InitRenderObject()
{
    PROFILE_FUNCTION();
    auto& renderGraph = RenderGraph::GetInstance();
    
    RefreshRenderSceneFromActiveWorld();
    BuildResolvedRenderScene();

    //初始化渲染需要的资源
    this->RenderInitialize();
    VL::RendererDescriptorContext descriptorContext = BuildRendererDescriptorContext();
    renderGraph.RenderInitialize(*rendererBackend, descriptorContext);
    InitializeCurrentRenderSceneResources();
}

void RenderSystem::Render()
{
    PROFILE_SCOPE("RenderSystem::Render");
    // Single-thread compatibility path: GT publishes the same immutable
    // snapshot that the optional RT consumes, then renders it immediately.
    PublishSnapshotFromActiveWorld();
    RenderLatestSnapshotOrLastGood();
}

void RenderSystem::ReleaseSwapchainDependentResources()
{
    VL::RendererResourceCache::GetInstance().ShutdownSwapchainDependentWorldResources();
    ShutdownFrameResources();
    initializedRenderWorldGeneration = 0;
}

void RenderSystem::RebuildSwapchainDependentResources()
{
    RenderGraph& renderGraph = RenderGraph::GetInstance();

    RefreshRenderSceneFromActiveWorld();
    BuildResolvedRenderScene();
    RenderInitialize();

    VL::RendererDescriptorContext descriptorContext = BuildRendererDescriptorContext();
    renderGraph.RenderInitialize(*rendererBackend, descriptorContext);
    InitializeCurrentRenderSceneResources();
}

void RenderSystem::RebuildRenderGraphDependentResources()
{
    if (rendererBackend == nullptr)
    {
        throw std::runtime_error("RenderSystem renderer backend is not set");
    }

    RenderGraph& renderGraph = RenderGraph::GetInstance();

    RefreshRenderSceneFromActiveWorld();
    BuildResolvedRenderScene();
    RenderInitialize();

    VL::RendererDescriptorContext descriptorContext = BuildRendererDescriptorContext();
    renderGraph.RenderInitialize(*rendererBackend, descriptorContext);
    InitializeCurrentRenderSceneResources();
}

void RenderSystem::UpdateGlobalUBOForPass(vk::CommandBuffer& commandBuffer)
{
    UpdateUBOGlobal(commandBuffer);
}

void RenderSystem::UpdateShadowGlobalUBOForPass(
    vk::CommandBuffer& commandBuffer,
    uint32_t passWidth,
    uint32_t passHeight,
    uint32_t cascadeIndex)
{
    UpdateUBOGlobalForShadow(commandBuffer, passWidth, passHeight, cascadeIndex);
}

void RenderSystem::UpdateMaterialInstanceUBOForPass(
    const std::shared_ptr<MaterialInstance>& materialInstance)
{
    frameResources.UpdateMaterialInstanceUniformBuffer(swapChainImageIndex, *materialInstance);
}

void RenderSystem::UpdateObjectUBOForPass(
    VL::RendererObjectGpuResources& objectResources,
    const VL::RenderDrawPacket& drawPacket)
{
    // Object transforms come from the frozen RenderDrawPacket. The upload path
    // addresses backend-owned object GPU resources through the resolved draw
    // packet.
    frameResources.UpdateObjectUniformBuffer(swapChainImageIndex, objectResources, drawPacket);
}

void RenderSystem::UploadLightsForPass(
    uint32_t swapChainImageIndex,
    const std::vector<VL::LightSnapshot>& lights)
{
    frameResources.UpdateLightBuffer(swapChainImageIndex, lights);
}

void RenderSystem::UpdateUBOGlobal(vk::CommandBuffer& commandBuffer)
{
    PROFILE_FUNCTION();
    if (!hasRenderScene)
    {
        RefreshRenderSceneFromActiveWorld();
    }

    UBOGlobal ubo;
    const VL::CameraSnapshot& camera = currentRenderScene.camera;
    ubo.view = camera.view;
    ubo.projection = camera.projection;
    ubo.invView = ubo.view.inverse();
    ubo.invProjection = ubo.projection.inverse();
    ubo.viewProjection = camera.viewProjection;
    ubo.invViewProjection = ubo.viewProjection.inverse();
    ubo.previousViewProjection = camera.previousViewProjection;
    if (!shadowCascadeFrameData.valid)
    {
        BuildShadowCascadeFrameData(1, 1);
    }
    ubo.lightViewProj = shadowCascadeFrameData.lightViewProj;
    ubo.cascadeSplits = shadowCascadeFrameData.cascadeSplits;
    ubo.shadowBias = shadowCascadeFrameData.bias;
    ubo.cameraPosition = camera.position;
    ubo.environmentSH = currentRenderScene.environment.sphericalHarmonics;
    ubo.debugViewMode = currentRenderScene.debugViewMode;
    ubo.environmentIntensity = currentRenderScene.environment.intensity;

    frameResources.UpdateGlobalUniformBuffer(commandBuffer, swapChainImageIndex, ubo);
}

void RenderSystem::UpdateUBOGlobalForShadow(
    vk::CommandBuffer& commandBuffer,
    uint32_t passSizeWidth,
    uint32_t passSizeHeight,
    uint32_t cascadeIndex)
{
    PROFILE_FUNCTION();
    if (!hasRenderScene)
    {
        RefreshRenderSceneFromActiveWorld();
    }

    BuildShadowCascadeFrameData(passSizeWidth, passSizeHeight);
    const uint32_t activeCascadeIndex =
        std::min(cascadeIndex, csmSettings.cascadeCount - 1);

    UBOGlobal ubo;
    ubo.view = shadowCascadeFrameData.view[activeCascadeIndex];
    ubo.projection = shadowCascadeFrameData.projection[activeCascadeIndex];
    ubo.invView = ubo.view.inverse();
    ubo.invProjection = ubo.projection.inverse();
    ubo.viewProjection = ubo.projection * ubo.view;
    ubo.invViewProjection = ubo.viewProjection.inverse();
    ubo.previousViewProjection = ubo.viewProjection;
    ubo.lightViewProj = shadowCascadeFrameData.lightViewProj;
    ubo.cascadeSplits = shadowCascadeFrameData.cascadeSplits;
    ubo.shadowBias = shadowCascadeFrameData.bias;
    ubo.environmentSH = currentRenderScene.environment.sphericalHarmonics;
    ubo.debugViewMode = currentRenderScene.debugViewMode;
    ubo.environmentIntensity = currentRenderScene.environment.intensity;
    {
        Eigen::Matrix3f rotT = ubo.view.block<3, 3>(0, 0);
        Eigen::Vector3f trans = ubo.view.block<3, 1>(0, 3);
        ubo.cameraPosition = -(rotT.transpose() * trans);
    }

    frameResources.UpdateGlobalUniformBuffer(commandBuffer, swapChainImageIndex, ubo);
}

void RenderSystem::BuildShadowCascadeFrameData(uint32_t passSizeWidth, uint32_t passSizeHeight)
{
    if (csmSettings.cascadeCount < 1)
    {
        csmSettings.cascadeCount = 1;
    }

    // 如果本帧已经计算过相同场景、相同分辨率的 cascade 数据，直接复用缓存
    if (shadowCascadeFrameData.valid &&
        shadowCascadeFrameData.frameIndex == currentFrame &&
        shadowCascadeFrameData.worldGeneration == currentRenderScene.worldGeneration &&
        shadowCascadeFrameData.width == passSizeWidth &&
        shadowCascadeFrameData.height == passSizeHeight)
    {
        return;
    }

    const VL::CameraSnapshot& camera = currentRenderScene.camera;
    const float cascadeNear = camera.clipNear;
    const float cascadeFar = std::max(cascadeNear + 0.1f, csmSettings.shadowDistance);

    shadowCascadeFrameData.view.fill(Eigen::Matrix4f::Identity());
    shadowCascadeFrameData.projection.fill(Eigen::Matrix4f::Identity());
    shadowCascadeFrameData.lightViewProj.fill(Eigen::Matrix4f::Identity());
    shadowCascadeFrameData.cascadeSplits = Eigen::Vector4f::Zero();
    shadowCascadeFrameData.bias = csmSettings.bias;

    float previousSplit = cascadeNear;
    for (uint32_t cascadeIndex = 0; cascadeIndex < csmSettings.cascadeCount; ++cascadeIndex)
    {
        const float p = static_cast<float>(cascadeIndex + 1) / static_cast<float>(csmSettings.cascadeCount);
        const float logSplit = cascadeNear * std::pow(cascadeFar / cascadeNear, p);
        const float uniformSplit = cascadeNear + (cascadeFar - cascadeNear) * p;
        // splitFar 最大不能超过 cascadeFar，按照实际的值是几十就是几十，是几百就是几百
        const float splitFar = csmSettings.splitLambda * logSplit + (1.0f - csmSettings.splitLambda) * uniformSplit;

        ShadowProjectionParams params = CalculateShadowMatrixForCameraRange(
            previousSplit,
            splitFar,
            passSizeWidth,
            passSizeHeight);
        shadowCascadeFrameData.view[cascadeIndex] = params.viewMatrix;
        shadowCascadeFrameData.projection[cascadeIndex] = params.projectionMatrix;
        shadowCascadeFrameData.lightViewProj[cascadeIndex] = params.projectionMatrix * params.viewMatrix;
        shadowCascadeFrameData.cascadeSplits[cascadeIndex] = splitFar;
        previousSplit = splitFar;
    }

    shadowCascadeFrameData.frameIndex = currentFrame;
    shadowCascadeFrameData.worldGeneration = currentRenderScene.worldGeneration;
    shadowCascadeFrameData.width = passSizeWidth;
    shadowCascadeFrameData.height = passSizeHeight;
    shadowCascadeFrameData.valid = true;
}

RenderSystem::ShadowProjectionParams RenderSystem::CalculateShadowMatrixForCameraRange(
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
    const VL::CameraSnapshot& camera = currentRenderScene.camera;
    Eigen::Vector3f cameraPosition = camera.position;
    Eigen::Vector3f cameraDirection = camera.forward;
        // near, far
    float cameraNear = splitNear;
    float cameraFar = splitFar;

    Eigen::Vector3f cameraRight = camera.right;
    Eigen::Vector3f cameraUp = camera.up;
    float cameraHFov = camera.horizontalFovDegrees;
    float aspect = static_cast<float>(CommonFunction::GetWindowSize().x()) / static_cast<float>(CommonFunction::GetWindowSize().y());
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
    for (const VL::LightSnapshot& light : currentRenderScene.lights)
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
    if (csmSettings.lightSpaceCasterBounds)
    {
        // Performance note: this scans all draw packets once per cascade. Current scenes are small
        // enough for this straightforward path; if profiling shows CPU pressure here, precompute
        // light-space draw bounds once per shadow frame and reuse them across cascades.
        const auto cascadeLightSpaceZBounds = ComputeCascadeLightSpaceZBounds(
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
RenderSystem::ShadowProjectionParams RenderSystem::CalculateShadowMatrix_DynamicTight(
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

    RenderSystem::ShadowProjectionParams params;
    
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

RenderSystem::ShadowProjectionParams RenderSystem::CalculateShadowMatrix_StableSphere(
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

    RenderSystem::ShadowProjectionParams params;
    params.viewMatrix = shadowCamera.GetViewMatrix();
    params.projectionMatrix = shadowCamera.GetProjectionMatrix();
    return params;
}

RenderSystem::ShadowProjectionParams RenderSystem::CalculateShadowMatrix_StableRectangular(
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

    RenderSystem::ShadowProjectionParams params;
    params.viewMatrix = shadowCamera.GetViewMatrix();
    params.projectionMatrix = shadowCamera.GetProjectionMatrix();
    return params;
}

void RenderSystem::RefreshRenderSceneFromActiveWorld()
{
    PublishSnapshotFromActiveWorld();
    if (!ConsumeLatestSnapshotIntoRenderScene())
    {
        throw std::runtime_error("WorldSnapshotQueue did not return the snapshot just published");
    }
}

void RenderSystem::PublishSnapshotFromActiveWorld()
{
    if (!activeWorld)
    {
        throw std::runtime_error("RenderSystem active World is not set");
    }

    VL::WorldSnapshotBuildDesc buildDesc;
    buildDesc.worldGeneration = activeWorld->GetGeneration();
    buildDesc.frameIndex = nextSnapshotFrameIndex++;
    buildDesc.debugViewMode = debugViewMode;
    buildDesc.environmentIntensity = environmentIntensity;

    auto snapshotResult = worldSnapshotBuilder.Build(*activeWorld, buildDesc);
    if (snapshotResult.IsFailure())
    {
        throw std::runtime_error(VL::FormatRuntimeError(snapshotResult.Error()));
    }

    worldSnapshotQueue.Publish(std::move(snapshotResult.Value()));
}

bool RenderSystem::ConsumeLatestSnapshotIntoRenderScene()
{
    auto snapshot = worldSnapshotQueue.ConsumeLatest();
    if (!snapshot.has_value())
    {
        return false;
    }

    auto renderSceneResult = rendererFrontend.BuildRenderScene(**snapshot);
    if (renderSceneResult.IsFailure())
    {
        throw std::runtime_error(VL::FormatRuntimeError(renderSceneResult.Error()));
    }

    currentRenderScene = std::move(renderSceneResult.Value());
    hasRenderScene = true;
    return true;
}

void RenderSystem::RenderLatestSnapshotOrLastGood()
{
    PROFILE_SCOPE("RenderSystem::RenderLatestSnapshotOrLastGood");
    if (rendererBackend == nullptr)
    {
        throw std::runtime_error("RenderSystem renderer backend is not set");
    }

    const bool consumedSnapshot = ConsumeLatestSnapshotIntoRenderScene();
    if (consumedSnapshot)
    {
        BuildResolvedRenderScene();
        if (currentRenderScene.worldGeneration != initializedRenderWorldGeneration)
        {
            InitializeCurrentRenderSceneResources();
        }
    }
    else if (!hasRenderScene)
    {
        throw std::runtime_error("RenderSystem has no RenderScene to render");
    }

    RecordAndSubmitCurrentRenderScene();
}

void RenderSystem::BuildResolvedRenderScene()
{
    const auto& resourceCache = VL::RendererResourceCache::GetInstance();
    auto resolvedSceneResult = resolvedRenderSceneBuilder.Build(currentRenderScene, resourceCache);
    if (resolvedSceneResult.IsFailure())
    {
        throw std::runtime_error(VL::FormatRuntimeError(resolvedSceneResult.Error()));
    }

    currentResolvedRenderScene = std::move(resolvedSceneResult.Value());
}

void RenderSystem::InitializeCurrentRenderSceneResources()
{
    frameResources.EnsureLightCapacity(currentRenderScene.lights.size(), *rendererBackend);

    VL::RendererDescriptorContext descriptorContext = BuildRendererDescriptorContext();
    VL::RendererResourceCache& resourceCache = VL::RendererResourceCache::GetInstance();
    RenderGraph::GetInstance().RefreshRuntimeDescriptors(*rendererBackend, descriptorContext);

    VL::RendererObjectResourceRegistry objectResourceRegistry;
    objectResourceRegistry.InitializeResolvedSceneResources(
        *rendererBackend,
        descriptorContext,
        currentRenderScene,
        currentResolvedRenderScene,
        resourceCache);

    initializedRenderWorldGeneration = currentRenderScene.worldGeneration;
}

void RenderSystem::RecordAndSubmitCurrentRenderScene()
{
    const RenderGraph& renderGraph = RenderGraph::GetInstance();

    // Backend owns swapchain frame synchronization. RenderSystem only records
    // pass work into the command buffer it receives for this image.
    VL::RendererFrameContext frameContext = rendererBackend->BeginFrame(currentFrame);
    uint32_t frameIndex = frameContext.frameIndex;
    swapChainImageIndex = frameContext.swapchainImageIndex;
    vk::CommandBuffer commandBuffer = frameContext.commandBuffer;

    {
        VulkanDebug::ScopedRegion frameRegion(
            commandBuffer,
            "Frame:" + std::to_string(frameIndex) + " Image:" + std::to_string(swapChainImageIndex),
            VulkanDebug::DebugCategory::eDefault);

        const auto& renderPassOrdered = renderGraph.GetRenderpassesOrdered();
        for (size_t passIndex = 0; passIndex < renderPassOrdered.size(); ++passIndex)
        {
            const auto& renderPassName = renderPassOrdered[passIndex];
            std::string renderPassScopeName = "RenderPass:" + renderPassName;
            PROFILE_SCOPE(renderPassScopeName.c_str());
            const auto& renderPass = renderGraph.GetRenderpasses().at(renderPassName);

            VulkanDebug::ScopedRegion passRegion(
                commandBuffer,
                renderPassName,
                VulkanDebug::DebugCategory::ePass);

            VL::PassRuntimeContext passContext{
                commandBuffer,
                renderPass,
                renderGraph,
                passIndex,
                swapChainImageIndex,
                currentRenderScene,
                currentResolvedRenderScene,
                *this
            };
            passRuntime.RecordPass(renderPassName, passContext);
        }
    }

    // RenderSystem only records pass commands. Queue, semaphore, fence,
    // swapchain, present, and retire epoch details stay backend-owned.
    rendererBackend->SubmitFrame(frameContext, currentFrame);

    currentFrame = currentFrame + 1;
}

void RenderSystem::RenderInitialize()
{
    InitializeFrameResources();
    ValidateFrameResourceDescriptors();
}

VL::RendererDescriptorContext RenderSystem::BuildRendererDescriptorContext() const
{
    VL::RendererDescriptorContext descriptorContext;
    descriptorContext.globalUniformBufferInfos = &GetUBOGlobalBufferInfo();
    descriptorContext.lightBufferInfos = &GetLightBufferInfo();
    descriptorContext.resourceCache = &VL::RendererResourceCache::GetInstance();
    return descriptorContext;
}

void RenderSystem::ShutdownRenderObject()
{
    ShutdownFrameResources();
}

void RenderSystem::InitializeFrameResources()
{
    if (rendererBackend == nullptr)
    {
        throw std::runtime_error("RenderSystem cannot initialize frame resources without a renderer backend");
    }

    frameResources.Initialize(*rendererBackend);
    frameResources.EnsureLightCapacity(currentRenderScene.lights.size(), *rendererBackend);
}

void RenderSystem::ShutdownFrameResources()
{
    if (rendererBackend == nullptr)
    {
        return;
    }

    frameResources.Shutdown(*rendererBackend);
}

void RenderSystem::ValidateFrameResourceDescriptors()
{
    if (!frameResources.IsInitialized())
    {
        throw std::runtime_error("RenderSystem cannot expose frame resource descriptors before frame resources are initialized");
    }
}

std::pair<float, float> RenderSystem::ComputeMinMaxAlongAxis(const Eigen::Vector3f& aabbMin, const Eigen::Vector3f& aabbMax, const Eigen::Vector3f& axis) const
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

std::optional<std::pair<float, float>> RenderSystem::ComputeCascadeLightSpaceZBounds(
    const Eigen::Matrix3f& worldToShadowMatrix,
    float minX,
    float maxX,
    float minY,
    float maxY) const
{
    if (currentRenderScene.drawPackets.empty())
    {
        return std::nullopt;
    }

    float minZ = std::numeric_limits<float>::max();
    float maxZ = std::numeric_limits<float>::lowest();
    bool hasCaster = false;
    const Eigen::Vector3f lightSpaceXAxis = worldToShadowMatrix.row(0).transpose();
    const Eigen::Vector3f lightSpaceYAxis = worldToShadowMatrix.row(1).transpose();
    const Eigen::Vector3f lightSpaceZAxis = worldToShadowMatrix.row(2).transpose();
    const Eigen::Vector3f absLightSpaceXAxis = lightSpaceXAxis.cwiseAbs();
    const Eigen::Vector3f absLightSpaceYAxis = lightSpaceYAxis.cwiseAbs();
    const Eigen::Vector3f absLightSpaceZAxis = lightSpaceZAxis.cwiseAbs();

    for (const VL::RenderDrawPacket& drawPacket : currentRenderScene.drawPackets)
    {
        const Eigen::Vector3f center = (drawPacket.worldBoundsMin + drawPacket.worldBoundsMax) * 0.5f;
        const Eigen::Vector3f extent = (drawPacket.worldBoundsMax - drawPacket.worldBoundsMin) * 0.5f;
        const float projectedCenterX = lightSpaceXAxis.dot(center);
        const float projectedRadiusX = absLightSpaceXAxis.dot(extent);
        const float objectMinX = projectedCenterX - projectedRadiusX;
        const float objectMaxX = projectedCenterX + projectedRadiusX;
        if (objectMaxX < minX || objectMinX > maxX)
        {
            continue;
        }

        const float projectedCenterY = lightSpaceYAxis.dot(center);
        const float projectedRadiusY = absLightSpaceYAxis.dot(extent);
        const float objectMinY = projectedCenterY - projectedRadiusY;
        const float objectMaxY = projectedCenterY + projectedRadiusY;
        if (objectMaxY < minY || objectMinY > maxY)
        {
            continue;
        }

        const float projectedCenterZ = lightSpaceZAxis.dot(center);
        const float projectedRadiusZ = absLightSpaceZAxis.dot(extent);
        minZ = std::min(minZ, projectedCenterZ - projectedRadiusZ);
        maxZ = std::max(maxZ, projectedCenterZ + projectedRadiusZ);
        hasCaster = true;
    }

    if (!hasCaster)
    {
        return std::nullopt;
    }

    return std::make_pair(minZ, maxZ);
}

std::array<Eigen::Vector3f, 8> RenderSystem::BuildWorldCorners(const Eigen::Vector3f& localMin, const Eigen::Vector3f& localMax, const Eigen::Matrix4f& modelMatrix) const
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

void RenderSystem::ComputeAabbFromCorners(const std::array<Eigen::Vector3f, 8>& corners, Eigen::Vector3f& outMin, Eigen::Vector3f& outMax)
{
    outMin = corners[0];
    outMax = corners[0];
    for (const auto& corner : corners)
    {
        outMin = outMin.cwiseMin(corner);
        outMax = outMax.cwiseMax(corner);
    }
}

void RenderSystem::ComputeViewAabbFromWorldCorners(const Eigen::Matrix4f& viewMatrix, const std::array<Eigen::Vector3f, 8>& worldCorners, Eigen::Vector3f& outMin, Eigen::Vector3f& outMax)
{
    outMin = (viewMatrix * Eigen::Vector4f(worldCorners[0].x(), worldCorners[0].y(), worldCorners[0].z(), 1.0f)).head<3>();
    outMax = outMin;
    for (const auto& corner : worldCorners)
    {
        Eigen::Vector3f viewCorner = (viewMatrix * Eigen::Vector4f(corner.x(), corner.y(), corner.z(), 1.0f)).head<3>();
        outMin = outMin.cwiseMin(viewCorner);
        outMax = outMax.cwiseMax(viewCorner);
    }
}

bool RenderSystem::IntersectsSplitFrustumFast(const Eigen::Vector3f& viewMin, const Eigen::Vector3f& viewMax, float splitNear, float splitFar, float fovRad, float aspect, float padding)
{
    float minZView = viewMin.z();
    float maxZView = viewMax.z();
    float splitNearP = std::max(0.0f, splitNear - padding);
    float splitFarP = splitFar + padding;
    if (maxZView < -splitFarP || minZView > -splitNearP)
    {
        return false;
    }
    float zForXY = std::min(minZView, -splitNearP);
    float distance = std::max(0.0f, -zForXY);
    float halfWidth = std::tan(fovRad * 0.5f) * distance;
    float halfHeight = halfWidth / aspect;
    if (viewMax.x() < -halfWidth || viewMin.x() > halfWidth || viewMax.y() < -halfHeight || viewMin.y() > halfHeight)
    {
        return false;
    }
    return true;
}
