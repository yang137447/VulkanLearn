#include "render/environment/environmentRuntime.h"

#include <stdexcept>

#include "profiler.h"
#include "render/environment/environmentIblBaker.h"
#include "render/environment/proceduralSkyCubeGenerator.h"
#include "render/frontend/renderScene.h"
#include "render/resource/rendererResourceCache.h"

namespace VL
{

void EnvironmentRuntime::Initialize(RendererBackendVulkan& rendererBackend)
{
    environmentGpuTimer.Initialize(rendererBackend);
}

void EnvironmentRuntime::Shutdown(RendererBackendVulkan& rendererBackend)
{
    environmentGpuTimer.Shutdown(rendererBackend);
}

void EnvironmentRuntime::ResetPendingWork()
{
    environmentUpdateScheduler.Reset();
    environmentUpdateSourceCube.reset();
}

void EnvironmentRuntime::Reset()
{
    environmentUpdateState.Reset();
    environmentUpdateScheduler.Reset();
    environmentUpdateSourceCube.reset();
}

EnvironmentUpdateDiagnostics EnvironmentRuntime::GetDiagnostics() const
{
    // 这里用了raii锁，确保在访问环境诊断信息时不会发生数据竞争。
    std::lock_guard<std::mutex> lock(environmentDiagnosticsMutex);
    return environmentDiagnostics;
}

void EnvironmentRuntime::RefreshDiagnostics()
{
    VL::EnvironmentUpdateDiagnostics snapshot;
    snapshot.progress = environmentUpdateScheduler.GetProgress();
    snapshot.gpuTiming = environmentGpuTimer.GetSnapshot();

    std::lock_guard<std::mutex> lock(environmentDiagnosticsMutex);
    environmentDiagnostics = std::move(snapshot);
}

std::shared_ptr<Texture> EnvironmentRuntime::GetEnvironmentCube(
    const RenderScene& renderScene,
    const RendererResourceCache& resourceCache,
    const ProceduralSkyCubeGenerator& proceduralSkyCubeGenerator) const
{
    switch(renderScene.environment.type)
    {
        case VL::EnvironmentType::None:
            // 只为保持实时 descriptor 合同而绑定已有 cubemap；强度为零，
            // 不进行 IBL 更新且跳过 sky，不能将其视为环境补光。
            return proceduralSkyCubeGenerator.GetActiveEnvironmentCube();
        case VL::EnvironmentType::ProceduralSky:
            return proceduralSkyCubeGenerator.GetActiveEnvironmentCube();
        case VL::EnvironmentType::Hdri:
            const std::shared_ptr<Texture> environmentCube =
                resourceCache.GetWorldTexture(
                    "environmentCube");
            if(environmentCube == nullptr)
            {
                throw std::runtime_error("Environment cube not found");
            }
            return environmentCube;
    }
    throw std::runtime_error("Unknown environment type");
}

void EnvironmentRuntime::PrepareResources(
    const RenderScene& renderScene,
    RendererResourceCache& resourceCache,
    const ProceduralSkyCubeGenerator& proceduralSkyCubeGenerator,
    const EnvironmentIblBaker& environmentIblBaker) const
{
    std::shared_ptr<Texture> environmentCube =
        GetEnvironmentCube(
            renderScene,
            resourceCache,
            proceduralSkyCubeGenerator);
    resourceCache.BindWorldTexture(
        "environmentCube",
        environmentCube);

    std::shared_ptr<Texture> prefilteredEnvironmentCube =
        environmentIblBaker
            .GetPrefilteredEnvironmentCube();
    if (prefilteredEnvironmentCube == nullptr)
    {
        throw std::runtime_error("Prefiltered environment cube is not initialized");
    }
    resourceCache.BindWorldTexture(
        "prefilteredEnvironmentCube",
        prefilteredEnvironmentCube);
}

void EnvironmentRuntime::RecordIbl(
    vk::CommandBuffer commandBuffer,
    uint32_t swapchainImageIndex,
    const RenderScene& renderScene,
    const RendererResourceCache& resourceCache,
    ProceduralSkyCubeGenerator& proceduralSkyCubeGenerator,
    EnvironmentIblBaker& environmentIblBaker)
{
    if (renderScene.environment.type == VL::EnvironmentType::None) return;
    PROFILE_SCOPE("EnvironmentIbl");
    environmentGpuTimer.BeginFrame(commandBuffer, swapchainImageIndex);

    // Sky Pass 每帧直接读取最新参数；cubemap、SH 和 prefilter 只在源数据 dirty 时
    // 启动新代际，避免相机移动或单纯强度变化触发昂贵重建。
    environmentUpdateState.Observe(renderScene.environment);
    if (environmentUpdateState.IsDirty())
    {
        const uint64_t requestedGeneration = environmentUpdateState.BeginUpdate();
        const bool startedNewGeneration = environmentUpdateScheduler.RequestUpdate(
            requestedGeneration,
            renderScene.environment,
            environmentIblBaker.GetPrefilterMipCount());
        if (startedNewGeneration)
        {
            // 程序化天空先写 pending cubemap；HDRI 本身已经完整，可以直接作为冻结输入。
            environmentUpdateSourceCube =
                renderScene.environment.type == VL::EnvironmentType::ProceduralSky
                ? proceduralSkyCubeGenerator.GetPendingEnvironmentCube()
                : GetEnvironmentCube(
                    renderScene,
                    resourceCache,
                    proceduralSkyCubeGenerator);
        }
    }

    VL::EnvironmentUpdateFramePlan framePlan = environmentUpdateScheduler.BuildFramePlan();
    if (!framePlan.HasWork())
    {
        RefreshDiagnostics();
        return;
    }
    if (!environmentUpdateSourceCube)
    {
        throw std::runtime_error("Environment update source cube is missing.");
    }

    const VL::EnvironmentSnapshot& pendingSnapshot =
        environmentUpdateScheduler.GetPendingSnapshot();
    if (framePlan.cubemapFaceCount > 0)
    {
        // 这里的 for 只消费 Scheduler 为“当前渲染帧”分配的 face 批次，
        // 并不是在一帧内固定遍历 cubemap 的 6 个面。默认 cubemapFacesPerFrame == 1：
        //
        // 渲染帧        cubemapFaceCount        本帧录制
        // Frame 1              1                 Face 0
        // Frame 2              1                 Face 1
        // Frame 3              1                 Face 2
        // Frame 4              1                 Face 3
        // Frame 5              1                 Face 4
        // Frame 6              1                 Face 5
        //
        // 如果预算改为 2，这个 for 才会每帧循环两次，并用 3 帧完成全部 6 个面。
        environmentGpuTimer.BeginProduct(
            commandBuffer,
            swapchainImageIndex,
            VL::EnvironmentGpuProduct::Cubemap);
        for (uint32_t faceOffset = 0; faceOffset < framePlan.cubemapFaceCount; ++faceOffset)
        {
            PROFILE_SCOPE("EnvironmentIbl::CubemapFace");
            proceduralSkyCubeGenerator.RecordFace(
                commandBuffer,
                swapchainImageIndex,
                pendingSnapshot.skyParameters,
                framePlan.cubemapFaces[faceOffset]);
        }
        environmentGpuTimer.EndProduct(
            commandBuffer,
            swapchainImageIndex,
            VL::EnvironmentGpuProduct::Cubemap);
    }

    if (framePlan.projectSphericalHarmonics)
    {
        PROFILE_SCOPE("EnvironmentIbl::SH");
        environmentGpuTimer.BeginProduct(
            commandBuffer,
            swapchainImageIndex,
            VL::EnvironmentGpuProduct::SphericalHarmonics);
        environmentIblBaker.RecordSphericalHarmonics(
            commandBuffer,
            environmentUpdateSourceCube,
            swapchainImageIndex);
        environmentGpuTimer.EndProduct(
            commandBuffer,
            swapchainImageIndex,
            VL::EnvironmentGpuProduct::SphericalHarmonics);
    }

    if (!framePlan.prefilterMips.empty())
    {
        environmentGpuTimer.BeginProduct(
            commandBuffer,
            swapchainImageIndex,
            VL::EnvironmentGpuProduct::Prefilter);
        for (uint32_t mipLevel : framePlan.prefilterMips)
        {
            PROFILE_SCOPE("EnvironmentIbl::PrefilterMip");
            environmentIblBaker.RecordPrefilterMip(
                commandBuffer,
                environmentUpdateSourceCube,
                swapchainImageIndex,
                mipLevel);
        }
        environmentGpuTimer.EndProduct(
            commandBuffer,
            swapchainImageIndex,
            VL::EnvironmentGpuProduct::Prefilter);
    }

    if (framePlan.commit)
    {
        PROFILE_SCOPE("EnvironmentIbl::Commit");
        environmentGpuTimer.BeginProduct(
            commandBuffer,
            swapchainImageIndex,
            VL::EnvironmentGpuProduct::Commit);
        if (pendingSnapshot.type == VL::EnvironmentType::ProceduralSky)
        {
            proceduralSkyCubeGenerator.RecordCommit(commandBuffer);
        }
        environmentIblBaker.RecordCommit(commandBuffer);
        environmentGpuTimer.EndProduct(
            commandBuffer,
            swapchainImageIndex,
            VL::EnvironmentGpuProduct::Commit);
        environmentUpdateScheduler.CompleteCommit(framePlan.generation);
        environmentUpdateState.CompleteUpdate(framePlan.generation);
        environmentUpdateSourceCube.reset();
    }

    RefreshDiagnostics();
}

} // namespace VL
