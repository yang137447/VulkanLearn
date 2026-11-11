#include "renderSystem.h"

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <iostream>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <vector>

#include "commonFunction.h"
#include "material.h"
#include "materialInstance.h"
#include "particles/particleDump.h"
#include "particles/particleEvents.h"
#include "profiler.h"
#include "render/particles/particleGpuTimer.h"
#include "render/backend/rendererBackendVulkan.h"
#include "render/resource/rendererResourceCache.h"
#include "render/volumetricFog/volumetricFogRenderer.h"
#include "renderGraph.h"
#include "texture.h"

using namespace VL;


void RenderSystem::ConfigureParticleSceneLighting()
{
    // 粒子图形 pass 的场景灯光/IBL descriptor：场景灯与 GPU 粒子灯按
    // swapchain image 绑定，IBL 取活动 World 的预过滤 cubemap 与全局
    // BRDF LUT。资源缺失时传空 descriptor，由 ParticleRenderer 内部的
    // 黑色兜底资源接管，保证 shader 永远读到合法描述符。
    const VL::RendererResourceCache& lightingCache =
        VL::RendererResourceCache::GetInstance();
    vk::DescriptorImageInfo environmentCubeDescriptor{};
    const std::shared_ptr<Texture> prefilteredEnvironment =
        lightingCache.GetGlobalTexture("prefilteredEnvironmentCube");
    if (prefilteredEnvironment != nullptr)
    {
        environmentCubeDescriptor =
            prefilteredEnvironment->GetDescriptorInfo();
    }
    vk::DescriptorImageInfo brdfLutDescriptor{};
    const std::shared_ptr<Texture> brdfLut =
        lightingCache.GetGlobalTexture("brdfLut");
    if (brdfLut != nullptr)
    {
        brdfLutDescriptor = brdfLut->GetDescriptorInfo();
    }
    particleRenderer.SetSceneLightingResources(
        GetLightBufferInfo(),
        frameResources.GetParticleLightBufferInfos(),
        environmentCubeDescriptor,
        brdfLutDescriptor);
    particleRenderer.SetLightCullingResources(
        frameResources.GetLightGridBufferInfos(),
        frameResources.GetLightIndexBufferInfos(),
        frameResources.GetLightGridParamBufferInfos());
}

void RenderSystem::PrepareParticleShadowCasters(
    vk::CommandBuffer& commandBuffer,
    const Renderpass& renderPass,
    uint32_t swapChainImageIndex,
    uint32_t octant)
{
    if (renderPass.type == VL::RenderGraphPassType::Shadow)
    {
        const uint32_t cascadeIndex =
            renderPass.shadowCascadeIndex;
        if (!csmRuntime.GetFrameData().valid ||
            cascadeIndex >= csmRuntime.GetFrameData().lightViewProj.size())
        {
            return;
        }
        // CSM 不需要光位置/远平面；w=bias 仅保存，接收面 bias 仍在
        // 采样端应用，caster 不再重复偏移深度。
        particleRenderer.PrepareShadow(
            commandBuffer,
            swapChainImageIndex,
            csmRuntime.GetFrameData().lightViewProj[cascadeIndex],
            0,
            0,
            Eigen::Vector3f::Zero(),
            0.0f,
            0.0f);
        return;
    }
    if (renderPass.type != VL::RenderGraphPassType::LocalShadow)
    {
        return;
    }

    const uint32_t lightIndex = renderPass.localShadowIndex;
    if (lightIndex >= activeLocalShadowLights.size())
    {
        return;
    }
    const VL::LightSnapshot& light =
        activeLocalShadowLights[lightIndex];
    const float farCm = VL::ComputeLocalShadowFar(light.radius);
    const uint32_t mode =
        light.type == VL::LightSnapshotType::Point ? 1u : 0u;
    Eigen::Matrix4f viewProjection;
    if (mode == 1)
    {
        // 与 shadowLocal / localShadowSampling 共用同一八面体编码；
        // view 是单位阵，投影矩阵直接把世界方向折到 [-1,1]^2。
        viewProjection =
            VL::BuildOctahedralProjectionMatrix(
                light.position,
                octant);
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
        viewProjection = matrices.projection * matrices.view;
    }
    particleRenderer.PrepareShadow(
        commandBuffer,
        swapChainImageIndex,
        viewProjection,
        static_cast<int>(mode),
        static_cast<int>(octant),
        light.position,
        farCm,
        std::max(0.0f, light.shadowBias));
}

void RenderSystem::RecordParticleShadowCasters(
    vk::CommandBuffer& commandBuffer,
    const Renderpass& renderPass,
    uint32_t swapChainImageIndex)
{
    particleRenderer.RecordShadow(
        commandBuffer,
        swapChainImageIndex,
        renderPass);
}

void RenderSystem::RecordParticlePass(
    vk::CommandBuffer& commandBuffer,
    const Renderpass& renderPass,
    uint32_t swapChainImageIndex)
{
    // 粒子管线与描述符完全独立于 Material/RenderGraph pass material；
    // 该调用发生在 particle pass 已绑定的 sceneColor 窗口内，sceneDepth
    // 由 renderPass 的 set 3 输入描述符集提供。
    // timestamp 只包住图形绘制，不含 RecordSimulation 的 compute 与排序 dispatch。
    particleGpuTimer.BeginPass(commandBuffer, swapChainImageIndex);
    particleRenderer.RecordGraphics(
        commandBuffer,
        swapChainImageIndex,
        renderPass);
    particleGpuTimer.EndPass(commandBuffer, swapChainImageIndex);
}

void RenderSystem::RecordDistortionPass(
    vk::CommandBuffer& commandBuffer,
    const Renderpass& renderPass,
    uint32_t swapChainImageIndex)
{
    // 扰动发射器复用粒子池与 set 0；这里只切换 pass 的颜色目标，
    // 由 ParticleRenderer 决定哪些 emitter 进入 distortion 管线。
    particleRenderer.RecordDistortion(
        commandBuffer,
        swapChainImageIndex,
        renderPass);
}

void RenderSystem::RecordFogCompositePass(
    vk::CommandBuffer& commandBuffer,
    const Renderpass& renderPass,
    uint32_t swapChainImageIndex)
{
    // fogComposite 的 pipeline/descriptor 由 RenderGraph 拥有的
    // VolumetricFogRenderer 提供；没有雾节点的图不会走到这里。
    const auto& fog = RenderGraph::GetInstance().GetVolumetricFog();
    if (fog)
    {
        fog->RecordComposite(
            commandBuffer, swapChainImageIndex, renderPass);
    }
}

void RenderSystem::RecordDecalStencilPass(
    vk::CommandBuffer& commandBuffer,
    const Renderpass& renderPass,
    uint32_t swapChainImageIndex,
    const VL::RenderScene& renderScene)
{
    // mask pass 只写 decalStencil，不触碰 DBuffer；DecalRenderer 在这一步
    // 用与 decals pass 相同的实例顺序写 bit0。
    decalRenderer.RecordStencil(
        commandBuffer,
        swapChainImageIndex,
        renderPass,
        renderScene);
}

void RenderSystem::RecordDecalPass(
    vk::CommandBuffer& commandBuffer,
    const Renderpass& renderPass,
    uint32_t swapChainImageIndex,
    const VL::RenderScene& renderScene)
{
    decalRenderer.RecordGraphics(
        commandBuffer,
        swapChainImageIndex,
        renderPass,
        renderScene);
}

bool RenderSystem::DumpParticles(
    const std::string& fileName,
    uint32_t maxPerEmitter,
    std::string& outMessage)
{
    if (rendererBackend == nullptr)
    {
        outMessage =
            "Particle dump failed: renderer backend is unavailable.";
        return false;
    }
    if (resourceRoot.empty())
    {
        outMessage =
            "Particle dump failed: resource root is unavailable.";
        return false;
    }

    // GT 与 RT 分线程时，dump 必须等上一帧完全归还所有权；没有渲染线程时
    // 回调为空，下面的 device wait 仍然覆盖同步模式下已提交的命令。
    if (renderThreadIdleCallback)
    {
        renderThreadIdleCallback();
    }
    try
    {
        rendererBackend->WaitIdle();
    }
    catch (const std::exception& exception)
    {
        outMessage =
            "Particle dump failed while waiting for the device: " +
            std::string(exception.what());
        return false;
    }

    std::filesystem::path outputPath;
    std::string pathError;
    if (!VL::ResolveParticleDumpPath(
            resourceRoot,
            fileName,
            outputPath,
            pathError))
    {
        outMessage = "Particle dump failed: " + pathError;
        return false;
    }

    // dump 根部的 GPU 字段与 particlestats 同源，避免两套口径。
    const VL::ParticleFrameStats frameStats = GetParticleFrameStats();
    std::string dumpError;
    if (!particleRenderer.DumpParticles(
            *rendererBackend,
            outputPath,
            std::min(maxPerEmitter, VL::kParticleDumpMaxDetails),
            frameStats,
            dumpError))
    {
        outMessage =
            "Particle dump failed: " +
            (dumpError.empty() ? "unknown error" : dumpError);
        return false;
    }

    outMessage =
        "Particle dump written: " + outputPath.string();
    return true;
}

VL::ParticleFrameStats RenderSystem::GetParticleFrameStats() const
{
    // 分线程渲染时只做线程让渡，避免 GT 读到 RT 正在写的统计；
    // 这里不 WaitIdle、不碰 Vulkan 对象；GPU 字段只是上一次 BeginFrame
    // 已回收完成的样本，不是即时查询。
    if (renderThreadIdleCallback)
    {
        renderThreadIdleCallback();
    }
    VL::ParticleFrameStats stats = particleRenderer.GetFrameStats();
    const VL::ParticleGpuTimingSnapshot gpu =
        particleGpuTimer.GetSnapshot();
    stats.gpuTimingSupported = gpu.supported;
    stats.gpuMilliseconds = gpu.lastMilliseconds;
    stats.gpuAverageMilliseconds = gpu.averageMilliseconds;
    stats.gpuMaxMilliseconds = gpu.maxMilliseconds;
    stats.gpuSampleCount = gpu.sampleCount;
    return stats;
}

void RenderSystem::RestartParticleSystems(
    const std::string& sceneObjectName)
{
    // emitter 的 elapsed/prewarm/needsClear 由 RecordSimulation 在渲染线程
    // 上读取并推进；分线程时先停住 RT，避免在帧中途改这些字段。
    if (renderThreadIdleCallback)
    {
        renderThreadIdleCallback();
    }
    particleRenderer.RestartParticleSystems(sceneObjectName);
}

bool RenderSystem::KillParticle(
    const std::string& sceneObjectName,
    const std::string& emitterName,
    uint32_t particleIndex,
    std::string& outMessage)
{
    if (renderThreadIdleCallback)
    {
        renderThreadIdleCallback();
    }
    std::string error;
    if (!particleRenderer.RequestParticleKill(
            sceneObjectName, emitterName, particleIndex, error))
    {
        outMessage = "Particle kill rejected: " + error;
        return false;
    }
    outMessage =
        "Particle kill queued: " + sceneObjectName + "." +
        emitterName + " slot=" + std::to_string(particleIndex);
    return true;
}

bool RenderSystem::SpawnParticleAt(
    const std::string& sceneObjectName,
    const std::string& emitterName,
    const Eigen::Vector3f& worldPosition,
    float lifetimeSeconds,
    std::string& outMessage)
{
    if (renderThreadIdleCallback)
    {
        renderThreadIdleCallback();
    }
    std::string error;
    if (!particleRenderer.RequestParticleSpawnAt(
            sceneObjectName,
            emitterName,
            worldPosition,
            lifetimeSeconds,
            error))
    {
        outMessage = "Particle spawn rejected: " + error;
        return false;
    }
    outMessage =
        "Particle spawn queued: " + sceneObjectName + "." +
        emitterName + " at (" +
        std::to_string(worldPosition.x()) + ", " +
        std::to_string(worldPosition.y()) + ", " +
        std::to_string(worldPosition.z()) + ")";
    return true;
}

bool RenderSystem::DrainParticleEvents(
    uint32_t maxPerEmitter,
    std::vector<VL::ParticleEventBatch>& outBatches,
    std::string& outError)
{
    if (rendererBackend == nullptr)
    {
        outError = "Particle event drain failed: backend is unavailable.";
        return false;
    }
    if (renderThreadIdleCallback)
    {
        renderThreadIdleCallback();
    }
    try
    {
        rendererBackend->WaitIdle();
    }
    catch (const std::exception& exception)
    {
        outError = std::string(
            "Particle event drain failed while waiting for the device: ") +
            exception.what();
        return false;
    }
    return particleRenderer.DrainParticleEvents(
        *rendererBackend, maxPerEmitter, outBatches, outError);
}

void RenderSystem::RenderParticleSimulation(
    vk::CommandBuffer& commandBuffer,
    uint32_t frameIndex)
{
    particleGpuTimer.BeginFrame(commandBuffer, swapChainImageIndex);

    const VL::ShadowCascadeFrameData& cascadeData =
        csmRuntime.GetFrameData();
    VL::ParticleShadowCascadeData particleShadowData;
    particleShadowData.valid = cascadeData.valid;
    particleShadowData.lightViewProj = cascadeData.lightViewProj;
    particleShadowData.bias = cascadeData.bias;
    particleShadowData.cascadeSplits = cascadeData.cascadeSplits;
    particleShadowData.csmParameters = cascadeData.csmParameters;
    particleRenderer.SetShadowCascadeData(particleShadowData);

    // 粒子池是跨帧连续状态，不能像 per-image UBO 那样复制；有 emitter
    // 时等待上一提交完成，避免两帧同时改写同一段粒子/排序/事件缓冲。
    if (particleRenderer.HasEmitters())
    {
        rendererBackend->WaitForPreviousFrameFence(frameIndex);
    }
    // GPU 粒子灯输出每帧从零开始；compute 只写本帧存活槽位，光照
    // shader 用 high-water count + 零强度空槽读取。
    frameResources.ClearParticleLightBuffer(
        commandBuffer,
        swapChainImageIndex);
    particleRenderer.RecordSimulation(
        commandBuffer,
        swapChainImageIndex,
        currentRenderScene,
        GetSceneRenderExtent());
}

void RenderSystem::RecordParticleSoftDepthPass(
    vk::CommandBuffer& commandBuffer,
    const Renderpass& renderPass)
{
    particleRenderer.RecordSoftDepth(
        commandBuffer,
        swapChainImageIndex,
        renderPass,
        RenderGraph::GetInstance(),
        currentRenderScene);
}

void RenderSystem::SetPendingSimulationDelta(float seconds)
{
    // NaN/负值按 0 处理；上限与 EngineLoop 的 MaxSimulationDelta 一致。
    pendingSimulationDeltaSeconds = std::isfinite(seconds)
        ? std::clamp(seconds, 0.0f, 0.1f)
        : 0.0f;
}
