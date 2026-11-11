#include "renderSystem.h"

#include <iostream>
#include <stdexcept>
#include <vector>

#include "profiler.h"
#include "render/shadow/localShadowMath.h"

using namespace VL;


void RenderSystem::PrepareLocalShadowLights()
{
    activeLocalShadowLights.clear();
    localShadowDrawnCasters.fill(0);
    localShadowTotalCasters.fill(0);
    // CSM 各级 caster 统计也按帧清零。该函数每帧在 shadow/localShadow
    // pass 录制前调用一次，是统一的重置点，不能放在逐级 UBO 更新里。
    shadowCascadeDrawnCasters.fill(0);
    shadowCascadeTotalCasters.fill(0);
    for (VL::LightSnapshot& light : currentRenderScene.lights)
    {
        light.shadowSlice = VL::kInvalidLocalShadowSlice;
    }
    particleRenderer.ClearParticleLightShadowBindings();

    std::vector<VL::LightSnapshot> candidates =
        currentRenderScene.lights;
    std::unordered_map<uint64_t, size_t> sceneIndexByLightId;
    for (size_t index = 0; index < currentRenderScene.lights.size(); ++index)
    {
        sceneIndexByLightId.emplace(
            currentRenderScene.lights[index].lightId, index);
    }
    std::unordered_map<uint64_t, std::string> particleKeyByLightId;
    for (const VL::ParticleLightShadowCandidate& particle :
         particleRenderer.GetParticleLightShadowCandidates())
    {
        uint64_t hash = 14695981039346656037ull;
        for (unsigned char character : particle.emitterKey)
        {
            hash ^= character;
            hash *= 1099511628211ull;
        }
        VL::LightSnapshot light;
        light.lightId = hash;
        light.type = VL::LightSnapshotType::Point;
        light.color = Eigen::Vector3f::Ones();
        light.intensity = 1.0f;
        light.position = particle.position;
        light.radius = particle.radius;
        light.castShadows = particle.castShadows;
        particleKeyByLightId.emplace(hash, particle.emitterKey);
        candidates.push_back(light);
    }

    const std::vector<size_t> ranked =
        VL::RankLocalShadowLights(
            candidates,
            currentRenderScene.camera.position,
            VL::kMaxLocalShadowLights);
    activeLocalShadowLights.reserve(ranked.size());
    for (size_t slice = 0; slice < ranked.size(); ++slice)
    {
        VL::LightSnapshot& light =
            candidates[ranked[slice]];
        light.shadowSlice = static_cast<uint32_t>(slice);
        light.shadowFar =
            VL::ComputeLocalShadowFar(light.radius);
        light.shadowBias =
            std::max(0.0f, light.shadowBias);
        const auto particle = particleKeyByLightId.find(light.lightId);
        if (particle != particleKeyByLightId.end())
        {
            particleRenderer.SetParticleLightShadowBinding(
                particle->second,
                light.shadowSlice,
                light.shadowFar,
                light.shadowBias,
                true);
        }
        else
        {
            const auto scene = sceneIndexByLightId.find(light.lightId);
            if (scene != sceneIndexByLightId.end())
            {
                VL::LightSnapshot& original =
                    currentRenderScene.lights[scene->second];
                original.shadowSlice = light.shadowSlice;
                original.shadowFar = light.shadowFar;
                original.shadowBias = light.shadowBias;
            }
        }
        activeLocalShadowLights.push_back(light);
    }
}

void RenderSystem::LogLocalShadowCasterStatistics()
{
    // 低频输出，避免每帧刷控制台；120 帧约 2 秒（60 FPS）。
    constexpr uint32_t kLogIntervalFrames = 120;
    if (currentFrame % kLogIntervalFrames != 0)
    {
        return;
    }

    // CSM 级联剔除统计：每级 casters=drawn/total，用于确认视锥剔除生效
    // 且没有因为级联矩阵异常把整级清空。
    if (IsCsmEnabled())
    {
        for (uint32_t cascadeIndex = 0;
             cascadeIndex < csmRuntime.GetFrameSettings().cascadeCount &&
             cascadeIndex < shadowCascadeDrawnCasters.size();
             ++cascadeIndex)
        {
            // 0/0 表示该级没有可用视锥（例如场景没有方向光）或没有 caster，
            // 不输出容易误解的空统计。
            if (shadowCascadeTotalCasters[cascadeIndex] == 0)
            {
                continue;
            }
            std::cout << "[CsmShadow] cascade=" << cascadeIndex
                      << " casters="
                      << shadowCascadeDrawnCasters[cascadeIndex]
                      << "/" << shadowCascadeTotalCasters[cascadeIndex]
                      << std::endl;
        }
    }

    for (size_t index = 0;
         index < activeLocalShadowLights.size();
         ++index)
    {
        const VL::LightSnapshot& light =
            activeLocalShadowLights[index];
        std::cout << "[LocalShadow] slice=" << index
                  << " type="
                  << (light.type == VL::LightSnapshotType::Point
                          ? "point"
                          : "spot")
                  << " casters="
                  << localShadowDrawnCasters[index]
                  << "/" << localShadowTotalCasters[index]
                  << std::endl;
    }
}

bool RenderSystem::IsLocalShadowActive(
    uint32_t localShadowIndex) const
{
    return localShadowIndex < activeLocalShadowLights.size() &&
        activeLocalShadowLights[localShadowIndex].castShadows;
}

uint32_t RenderSystem::GetLocalShadowMode(
    uint32_t localShadowIndex) const
{
    if (localShadowIndex >= activeLocalShadowLights.size())
    {
        return 0;
    }
    return activeLocalShadowLights[localShadowIndex].type ==
        VL::LightSnapshotType::Point ? 1u : 0u;
}

const VL::LightSnapshot* RenderSystem::GetLocalShadowLight(
    uint32_t localShadowIndex) const
{
    if (localShadowIndex >= activeLocalShadowLights.size())
    {
        return nullptr;
    }
    return &activeLocalShadowLights[localShadowIndex];
}

void RenderSystem::ReportLocalShadowCasterStats(
    uint32_t localShadowIndex,
    uint32_t drawnCasters,
    uint32_t totalCasters)
{
    if (localShadowIndex >= VL::kMaxLocalShadowLights)
    {
        return;
    }
    localShadowDrawnCasters[localShadowIndex] = drawnCasters;
    localShadowTotalCasters[localShadowIndex] = totalCasters;
}

bool RenderSystem::GetShadowCascadeFrustumPlanes(
    uint32_t cascadeIndex,
    VL::ShadowFrustumPlanes& outPlanes) const
{
    if (!IsCsmEnabled() ||
        !IsShadowCascadeActive(cascadeIndex) ||
        !csmRuntime.GetFrameData().valid ||
        cascadeIndex >= csmRuntime.GetFrameData().lightViewProj.size())
    {
        return false;
    }

    const Eigen::Matrix4f& lightViewProjection =
        csmRuntime.GetFrameData().lightViewProj[cascadeIndex];
    if (!lightViewProjection.allFinite())
    {
        // 矩阵异常时保留全量绘制，宁可多画也不能漏影。
        return false;
    }
    // 场景没有方向光时 CalculateShadowMatrixForCameraRange 会回退单位阵；
    // 此时 CSM 实际没有有效视锥，不能按单位立方体剔除 caster。
    if (lightViewProjection.isApprox(
            Eigen::Matrix4f::Identity(),
            1.0e-6f))
    {
        return false;
    }
    outPlanes = VL::ExtractShadowFrustumPlanes(lightViewProjection);
    return true;
}

void RenderSystem::ReportShadowCascadeCasterStats(
    uint32_t cascadeIndex,
    uint32_t drawnCasters,
    uint32_t totalCasters)
{
    if (cascadeIndex >= shadowCascadeDrawnCasters.size())
    {
        return;
    }
    shadowCascadeDrawnCasters[cascadeIndex] = drawnCasters;
    shadowCascadeTotalCasters[cascadeIndex] = totalCasters;
}

void RenderSystem::UpdateLocalShadowUBOForPass(
    vk::CommandBuffer& commandBuffer,
    uint32_t passWidth,
    uint32_t passHeight,
    uint32_t lightIndex,
    uint32_t octant,
    uint32_t mode)
{
    PROFILE_FUNCTION();
    (void)passWidth;
    (void)passHeight;
    if (lightIndex >= activeLocalShadowLights.size())
    {
        throw std::runtime_error(
            "LocalShadow pass requested an inactive light index");
    }

    const VL::LightSnapshot& light =
        activeLocalShadowLights[lightIndex];
    const float farCm =
        VL::ComputeLocalShadowFar(light.radius);

    UBOGlobal ubo;
    if (mode == 1)
    {
        // 八面体象限：view 固定为单位阵，投影矩阵直接输出世界点相对灯位置
        // 的折叠坐标；depth 由片元 shader 的方向长度写入，因此逆矩阵不参与
        // 任何计算，显式置单位阵避免奇异投影产生 NaN。
        ubo.view = Eigen::Matrix4f::Identity();
        ubo.projection =
            VL::BuildOctahedralProjectionMatrix(
                light.position,
                octant);
        ubo.invView = Eigen::Matrix4f::Identity();
        ubo.invProjection = Eigen::Matrix4f::Identity();
        ubo.invViewProjection = Eigen::Matrix4f::Identity();
        ubo.cameraPosition = light.position;
    }
    else
    {
        const VL::LocalShadowSpotMatrices matrices =
            VL::BuildSpotShadowMatrices(
                light.position,
                light.direction,
                light.coneAngleOuter,
                1.0f,
                farCm);
        ubo.view = matrices.view;
        ubo.projection = matrices.projection;
        ubo.invView = ubo.view.inverse();
        ubo.invProjection = ubo.projection.inverse();
        ubo.viewProjection = ubo.projection * ubo.view;
        ubo.invViewProjection = ubo.viewProjection.inverse();
        Eigen::Matrix3f rotT = ubo.view.block<3, 3>(0, 0);
        Eigen::Vector3f trans = ubo.view.block<3, 1>(0, 3);
        ubo.cameraPosition = -(rotT.transpose() * trans);
    }

    if (mode == 1)
    {
        ubo.viewProjection = ubo.projection * ubo.view;
        ubo.invViewProjection = Eigen::Matrix4f::Identity();
    }
    ubo.previousViewProjection = ubo.viewProjection;
    ubo.debugViewMode = currentRenderScene.debugViewMode;
    ubo.environmentIntensity = currentRenderScene.environment.intensity;
    ubo.environmentType =
        currentRenderScene.environment.type ==
            VL::EnvironmentType::ProceduralSky ? 1 : 0;
    ubo.skyParameters = currentRenderScene.environment.skyParameters;
    // xyz = 灯位置，w = far；shader 用 length(worldPos - xyz) / w。
    ubo.localShadowLightPos = Eigen::Vector4f(
        light.position.x(),
        light.position.y(),
        light.position.z(),
        farCm);
    // x = 0 透视 / 1 八面体，y = 象限，z = 接收面 bias cm。
    ubo.localShadowParams = Eigen::Vector4f(
        static_cast<float>(mode),
        static_cast<float>(octant),
        std::max(0.0f, light.shadowBias),
        0.0f);
    frameResources.UpdateGlobalUniformBuffer(
        commandBuffer,
        swapChainImageIndex,
        ubo);
}
