#include "renderSystem.h"
#include "shader/reload/shaderReloadCoordinator.h"
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
#include "pipeline/pipelineFactory.h"
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

void RenderSystem::SetUiOverlayShaderPaths(std::string vertexPath, std::string fragmentPath)
{
    uiVertexShaderPath = std::move(vertexPath);
    uiFragmentShaderPath = std::move(fragmentPath);
}

void RenderSystem::SetCsmSettings(const VL::CsmSettings& settings)
{
    csmSettings = settings;
    shadowCascadeFrameData.valid = false;
}

bool RenderSystem::SetCsmCastShadows(bool enabled, std::string& outMessage)
{
    csmSettings.castShadows = enabled;
    shadowCascadeFrameData.valid = false;
    outMessage = std::string("CSM ") + (enabled ? "enabled." : "disabled.");
    return true;
}

bool RenderSystem::SetCsmDynamicShadowDistance(
    float distance,
    std::string& outMessage)
{
    if (distance <= 0.0f)
    {
        outMessage = "CSM shadow distance must be positive.";
        return false;
    }

    csmSettings.dynamicShadowDistance = distance;
    shadowCascadeFrameData.valid = false;
    outMessage = "CSM shadow distance set to " + std::to_string(distance);
    return true;
}

bool RenderSystem::SetCsmCascadeCount(
    uint32_t cascadeCount,
    std::string& outMessage)
{
    if (cascadeCount < 1 ||
        cascadeCount > VL::CsmSettings::MaxCascadeCount)
    {
        outMessage = "CSM cascade count must be in the range [1, 4].";
        return false;
    }

    csmSettings.cascadeCount = cascadeCount;
    shadowCascadeFrameData.valid = false;
    outMessage =
        "CSM cascade count set to " +
        std::to_string(cascadeCount);
    return true;
}

bool RenderSystem::SetCsmCascadeDistributionExponent(
    float exponent,
    std::string& outMessage)
{
    if (exponent < 1.0f || exponent > 10.0f)
    {
        outMessage =
            "CSM cascade distribution exponent must be in the range [1, 10].";
        return false;
    }

    csmSettings.cascadeDistributionExponent = exponent;
    shadowCascadeFrameData.valid = false;
    outMessage =
        "CSM cascade distribution exponent set to " +
        std::to_string(exponent);
    return true;
}

bool RenderSystem::SetCsmCascadeTransitionFraction(
    float fraction,
    std::string& outMessage)
{
    if (fraction < 0.0f || fraction > 1.0f)
    {
        outMessage =
            "CSM cascade transition fraction must be in the range [0, 1].";
        return false;
    }

    csmSettings.cascadeTransitionFraction = fraction;
    shadowCascadeFrameData.valid = false;
    outMessage =
        "CSM cascade transition fraction set to " +
        std::to_string(fraction);
    return true;
}

bool RenderSystem::SetCsmShadowDistanceFadeoutFraction(
    float fraction,
    std::string& outMessage)
{
    if (fraction < 0.0f || fraction > 1.0f)
    {
        outMessage =
            "CSM shadow distance fadeout fraction must be in the range [0, 1].";
        return false;
    }

    csmSettings.shadowDistanceFadeoutFraction = fraction;
    shadowCascadeFrameData.valid = false;
    outMessage =
        "CSM shadow distance fadeout fraction set to " +
        std::to_string(fraction);
    return true;
}

bool RenderSystem::SetCsmShadowBias(
    float value,
    std::string& outMessage)
{
    if (value < 0.0f || value > 1.0f)
    {
        outMessage = "CSM shadow bias must be in the range [0, 1].";
        return false;
    }

    csmSettings.shadowBias = value;
    shadowCascadeFrameData.valid = false;
    outMessage = "CSM shadow bias set to " + std::to_string(value);
    return true;
}

bool RenderSystem::SetCsmShadowSlopeBias(
    float value,
    std::string& outMessage)
{
    if (value < 0.0f || value > 1.0f)
    {
        outMessage = "CSM shadow slope bias must be in the range [0, 1].";
        return false;
    }

    csmSettings.shadowSlopeBias = value;
    shadowCascadeFrameData.valid = false;
    outMessage =
        "CSM shadow slope bias set to " +
        std::to_string(value);
    return true;
}

bool RenderSystem::SetCsmShadowCascadeBiasDistribution(
    float value,
    std::string& outMessage)
{
    if (value < 0.0f || value > 1.0f)
    {
        outMessage =
            "CSM shadow cascade bias distribution must be in the range [0, 1].";
        return false;
    }

    csmSettings.shadowCascadeBiasDistribution = value;
    shadowCascadeFrameData.valid = false;
    outMessage =
        "CSM shadow cascade bias distribution set to " +
        std::to_string(value);
    return true;
}

VL::EnvironmentUpdateDiagnostics RenderSystem::GetEnvironmentUpdateDiagnostics() const
{
    // 这里用了raii锁，确保在访问环境诊断信息时不会发生数据竞争。
    std::lock_guard<std::mutex> lock(environmentDiagnosticsMutex);
    return environmentDiagnostics;
}

ComputeShaderArtifact RenderSystem::GetActiveComputeShaderArtifact(
    const std::string& shaderName) const
{
    if (shaderName == "generator/skyToCubemap")
    {
        return proceduralSkyCubeGenerator.GetActiveArtifact();
    }
    if (shaderName == "generator/skySHGenerate")
    {
        return environmentIblBaker
            .GetActiveSkySHGenerateArtifact();
    }
    if (shaderName == "generator/prefilterEnvMap")
    {
        return environmentIblBaker
            .GetActivePrefilterEnvMapArtifact();
    }
    throw std::runtime_error(
        "No live compute reload participant for shader: " +
        shaderName);
}

void RenderSystem::SetActiveWorld(std::shared_ptr<const VL::World> world)
{
    activeWorld = std::move(world);
    worldSnapshotQueue.Clear();
    worldSnapshotBuilder.Reset();
    currentRenderScene = VL::RenderScene();
    currentResolvedRenderScene = VL::ResolvedRenderScene();
    hasRenderScene = false;
    windClockInitialized = false;
    currentWindTimeSeconds = 0.0;
    initializedRenderWorldGeneration = 0;
    nextSnapshotFrameIndex = 0;
    shadowCascadeFrameData.valid = false;
    if (activeWorld)
    {
        speedTreeWindProfiles.Configure(activeWorld->GetSpeedTreeWindProfiles());
        (void)speedTreeWindProfiles.SetStrength(speedTreeStrength);
        (void)speedTreeWindProfiles.SetGustingEnabled(speedTreeGustingEnabled);
    }
    else
    {
        speedTreeWindProfiles.Reset();
    }
}

bool RenderSystem::ForceSpeedTreeGust()
{
    return speedTreeWindProfiles.ForceGust();
}

bool RenderSystem::SetSpeedTreeStrength(float strength)
{
    if (!speedTreeWindProfiles.SetStrength(strength))
    {
        return false;
    }
    speedTreeStrength = strength;
    return true;
}

bool RenderSystem::SetSpeedTreeGustingEnabled(bool enabled)
{
    if (!speedTreeWindProfiles.SetGustingEnabled(enabled))
    {
        return false;
    }
    speedTreeGustingEnabled = enabled;
    return true;
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
    toneMappingMode = mode;
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
    bloomStrength = value;
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
    bloomThreshold = value;
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
    bloomKnee = value;
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
    bloomClamp = value;
    return true;
}

void RenderSystem::InitializeWorldTransactionResources()
{
    PROFILE_FUNCTION();
    RenderInitialize();
}

void RenderSystem::FinalizeInitialRenderObjectInitialization()
{
    PROFILE_FUNCTION();
    if (uiRenderSnapshotQueue != nullptr && !uiVertexShaderPath.empty() && !uiFragmentShaderPath.empty())
    {
        uiOverlayRenderer.Initialize(
            *rendererBackend,
            uiVertexShaderPath,
            uiFragmentShaderPath);
        ShaderVariantKey uiVariant;
        uiVariant.shaderName = "uiOverlay";
        uiOverlayRenderer.SetActiveShaderArtifact(
            pipelineFactory->PrepareGraphicsShaderVariant(
                uiVariant));
        if (shaderReloadCoordinator != nullptr)
        {
            shaderReloadCoordinator->SetUiOverlayParticipant(
                &uiOverlayRenderer);
        }
    }
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
    uiOverlayRenderer.ReleaseSwapchainDependentResources();
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
    if (uiOverlayRenderer.IsInitialized())
    {
        uiOverlayRenderer.RebuildSwapchainDependentResources();
    }
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

void RenderSystem::RefreshResolvedSceneAfterShaderReload()
{
    if (hasRenderScene)
    {
        BuildResolvedRenderScene();
    }
}

VL::PreparedRuntimeBinding RenderSystem::PrepareRuntimeBinding(
    std::shared_ptr<const VL::World> world,
    VL::RendererResourceCache& candidateCache,
    RenderGraph& candidateGraph)
{
    if (!world)
    {
        throw std::runtime_error(
            "Cannot prepare runtime binding without a candidate World");
    }
    if (rendererBackend == nullptr)
    {
        throw std::runtime_error(
            "Cannot prepare runtime binding without a renderer backend");
    }

    VL::PreparedRuntimeBinding prepared;
    prepared.world = std::move(world);
    prepared.csmSettings =
        prepared.world->GetCsmSettings();

    VL::WorldSnapshotBuildDesc snapshotDesc;
    snapshotDesc.worldGeneration =
        prepared.world->GetGeneration();
    snapshotDesc.frameIndex = nextSnapshotFrameIndex;
    snapshotDesc.debugViewMode = debugViewMode;
    snapshotDesc.environmentIntensity =
        environmentIntensity;
    auto snapshotResult =
        prepared.snapshotBuilder.Build(
            *prepared.world,
            snapshotDesc);
    if (snapshotResult.IsFailure())
    {
        throw std::runtime_error(
            VL::FormatRuntimeError(snapshotResult.Error()));
    }

    auto renderSceneResult =
        rendererFrontend.BuildRenderScene(
            snapshotResult.Value());
    if (renderSceneResult.IsFailure())
    {
        throw std::runtime_error(
            VL::FormatRuntimeError(
                renderSceneResult.Error()));
    }
    prepared.renderScene =
        std::move(renderSceneResult.Value());

    auto resolvedSceneResult =
        resolvedRenderSceneBuilder.Build(
            prepared.renderScene,
            candidateCache);
    if (resolvedSceneResult.IsFailure())
    {
        throw std::runtime_error(
            VL::FormatRuntimeError(
                resolvedSceneResult.Error()));
    }
    prepared.resolvedRenderScene =
        std::move(resolvedSceneResult.Value());

    prepared.lightCapacity =
        frameResources.PrepareLightCapacity(
            prepared.renderScene.lights.size(),
            *rendererBackend);
    PrepareEnvironmentResources(
        prepared.renderScene,
        candidateCache);

    const std::vector<vk::DescriptorBufferInfo>&
        candidateLightBufferInfos =
            prepared.lightCapacity.GetBufferInfos(
                frameResources);
    const VL::RendererDescriptorContext
        descriptorContext =
            BuildRendererDescriptorContext(
                candidateCache,
                candidateGraph,
                candidateLightBufferInfos);
    candidateGraph.RenderInitialize(
        *rendererBackend,
        descriptorContext);

    VL::RendererObjectResourceRegistry
        objectResourceRegistry;
    objectResourceRegistry.InitializeResolvedSceneResources(
        *rendererBackend,
        descriptorContext,
        prepared.renderScene,
        prepared.resolvedRenderScene,
        candidateCache);

    prepared.speedTreeWindProfiles.Configure(
        prepared.world->GetSpeedTreeWindProfiles());
    (void)prepared.speedTreeWindProfiles.SetStrength(
        speedTreeStrength);
    (void)prepared.speedTreeWindProfiles
        .SetGustingEnabled(speedTreeGustingEnabled);
    prepared.nextSnapshotFrameIndex =
        nextSnapshotFrameIndex + 1;
    return prepared;
}

std::shared_ptr<void>
RenderSystem::CommitPreparedRuntimeBinding(
    VL::PreparedRuntimeBinding prepared) noexcept
{
    activeWorld.swap(prepared.world);
    worldSnapshotBuilder.Swap(
        prepared.snapshotBuilder);

    using std::swap;
    swap(
        currentRenderScene.worldGeneration,
        prepared.renderScene.worldGeneration);
    swap(
        currentRenderScene.frameIndex,
        prepared.renderScene.frameIndex);
    swap(
        currentRenderScene.camera,
        prepared.renderScene.camera);
    currentRenderScene.drawPackets.swap(
        prepared.renderScene.drawPackets);
    currentRenderScene.materialGroups.swap(
        prepared.renderScene.materialGroups);
    currentRenderScene.lights.swap(
        prepared.renderScene.lights);
    swap(
        currentRenderScene.environment,
        prepared.renderScene.environment);
    swap(
        currentRenderScene.debugViewMode,
        prepared.renderScene.debugViewMode);
    currentResolvedRenderScene.materialGroups.swap(
        prepared.resolvedRenderScene.materialGroups);
    speedTreeWindProfiles.Swap(
        prepared.speedTreeWindProfiles);
    swap(csmSettings, prepared.csmSettings);

    std::shared_ptr<void> retiredLightBuffer =
        frameResources.CommitPreparedLightCapacity(
            std::move(prepared.lightCapacity));
    hasRenderScene = true;
    windClockInitialized = false;
    currentWindTimeSeconds = 0.0;
    initializedRenderWorldGeneration =
        currentRenderScene.worldGeneration;
    nextSnapshotFrameIndex =
        prepared.nextSnapshotFrameIndex;
    shadowCascadeFrameData.valid = false;
    environmentUpdateState.Reset();
    environmentUpdateScheduler.Reset();
    environmentUpdateSourceCube.reset();
    // The old World package keeps its HDRI alive until the prepared retire
    // epoch. Drop descriptor-source mirrors now so they cannot outlive that
    // package; the next IBL use rewrites the acquired image's descriptor.
    environmentIblBaker.InvalidateEnvironmentCubeBindings();
    return retiredLightBuffer;
}

void RenderSystem::ClearPendingWorldSnapshots() noexcept
{
    worldSnapshotQueue.Clear();
}

void RenderSystem::PrepareRenderGraphReload(
    RenderGraph& candidateGraph)
{
    if (!activeWorld || !hasRenderScene)
    {
        throw std::runtime_error(
            "Cannot prepare a RenderGraph reload without an active runtime binding");
    }
    const VL::RendererDescriptorContext
        descriptorContext =
            BuildRendererDescriptorContext(
                VL::RendererResourceCache::GetInstance(),
                candidateGraph,
                GetLightBufferInfo());
    candidateGraph.RenderInitialize(
        *rendererBackend,
        descriptorContext);
}

std::string RenderSystem::GetResolvedShaderGenerationFingerprint() const
{
    std::string fingerprint;
    for (const VL::ResolvedMaterialGroup& materialGroup :
         currentResolvedRenderScene.materialGroups)
    {
        if (!materialGroup.material)
        {
            continue;
        }
        fingerprint += materialGroup.material->GetMaterialKey();
        fingerprint += ":";
        fingerprint +=
            materialGroup.material
                ->GetSurfaceShaderArtifact()
                .artifactGenerationKey;
        if (materialGroup.material->GetShadowShaderArtifact())
        {
            fingerprint += ":";
            fingerprint +=
                materialGroup.material
                    ->GetShadowShaderArtifact()
                    ->artifactGenerationKey;
        }
        fingerprint += ";";
    }
    return fingerprint;
}

uint64_t RenderSystem::GetActiveWorldGeneration() const noexcept
{
    return activeWorld
        ? activeWorld->GetGeneration()
        : 0;
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
    const SpeedTreeWindStateGPU* windState =
        speedTreeWindProfiles.FindGpuState(drawPacket.speedTreeWindProfileKey);
    frameResources.UpdateObjectUniformBuffer(
        swapChainImageIndex,
        objectResources,
        drawPacket,
        windState);
}

void RenderSystem::UploadLightsForPass(
    uint32_t swapChainImageIndex,
    const std::vector<VL::LightSnapshot>& lights)
{
    frameResources.UpdateLightBuffer(swapChainImageIndex, lights);
}

void RenderSystem::RecordEyeDescriptorBind()
{
    ++eyePerformanceFrameStats.eyeDescriptorBindCount;
}

void RenderSystem::RecordEyeDraw(size_t lutSampleCount)
{
    ++eyePerformanceFrameStats.eyeDrawCount;
    eyePerformanceFrameStats.eyeLutSampleCount += lutSampleCount;
}

bool RenderSystem::IsCsmEnabled() const
{
    return csmSettings.castShadows;
}

bool RenderSystem::IsShadowCascadeActive(
    uint32_t cascadeIndex) const
{
    return csmSettings.castShadows &&
        cascadeIndex < csmSettings.cascadeCount;
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
    if (csmSettings.castShadows)
    {
        if (!shadowCascadeFrameData.valid)
        {
            BuildShadowCascadeFrameData(1, 1);
        }
        ubo.lightViewProj = shadowCascadeFrameData.lightViewProj;
        ubo.cascadeSplits = shadowCascadeFrameData.cascadeSplits;
        ubo.shadowBias = shadowCascadeFrameData.bias;
        ubo.csmParameters = shadowCascadeFrameData.csmParameters;
    }
    else
    {
        // Zero cascade splits is the GPU-side disabled flag consumed by
        // CalculateCsmShadow; keep every shadow field explicitly neutral.
        ubo.cascadeSplits = Eigen::Vector4f::Zero();
        ubo.shadowBias.fill(Eigen::Vector4f::Zero());
        ubo.csmParameters = Eigen::Vector4f::Zero();
        for (Eigen::Matrix4f& lightViewProj : ubo.lightViewProj)
        {
            lightViewProj = Eigen::Matrix4f::Identity();
        }
    }
    ubo.cameraPosition = camera.position;
    // environmentSH 由 GPU 在环境代际 commit 时广播，常规 CPU 上传会跳过该区间。
    ubo.debugViewMode = currentRenderScene.debugViewMode;
    ubo.environmentIntensity = currentRenderScene.environment.intensity;
    ubo.environmentType = currentRenderScene.environment.type == VL::EnvironmentType::ProceduralSky ? 1 : 0;
    ubo.skyParameters = currentRenderScene.environment.skyParameters;
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
    ubo.csmParameters = shadowCascadeFrameData.csmParameters;
    // environmentSH 由 GPU 在环境代际 commit 时广播，常规 CPU 上传会跳过该区间。
    ubo.debugViewMode = currentRenderScene.debugViewMode;
    ubo.environmentIntensity = currentRenderScene.environment.intensity;
    ubo.environmentType = currentRenderScene.environment.type == VL::EnvironmentType::ProceduralSky ? 1 : 0;
    ubo.skyParameters = currentRenderScene.environment.skyParameters;
    {
        Eigen::Matrix3f rotT = ubo.view.block<3, 3>(0, 0);
        Eigen::Vector3f trans = ubo.view.block<3, 1>(0, 3);
        ubo.cameraPosition = -(rotT.transpose() * trans);
    }

    frameResources.UpdateGlobalUniformBuffer(commandBuffer, swapChainImageIndex, ubo);
}

void RenderSystem::BuildShadowCascadeFrameData(uint32_t passSizeWidth, uint32_t passSizeHeight)
{
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
    const float cascadeFar =
        std::max(
            cascadeNear + 0.1f,
            csmSettings.dynamicShadowDistance);

    shadowCascadeFrameData.view.fill(Eigen::Matrix4f::Identity());
    shadowCascadeFrameData.projection.fill(Eigen::Matrix4f::Identity());
    shadowCascadeFrameData.lightViewProj.fill(Eigen::Matrix4f::Identity());
    shadowCascadeFrameData.cascadeSplits = Eigen::Vector4f::Zero();
    shadowCascadeFrameData.bias.fill(Eigen::Vector4f::Zero());
    shadowCascadeFrameData.csmParameters = Eigen::Vector4f(
        csmSettings.cascadeTransitionFraction,
        csmSettings.shadowDistanceFadeoutFraction,
        static_cast<float>(csmSettings.cascadeCount),
        csmSettings.dynamicShadowDistance);

    float previousSplit = cascadeNear;
    for (uint32_t cascadeIndex = 0; cascadeIndex < csmSettings.cascadeCount; ++cascadeIndex)
    {
        const float p = static_cast<float>(cascadeIndex + 1) / static_cast<float>(csmSettings.cascadeCount);
        // UE 风格 Distribution Exponent：指数越大，越多分辨率集中到相机附近。
        // 最后一级始终精确落在 Dynamic Shadow Distance。
        const float splitFraction =
            std::pow(
                p,
                csmSettings.cascadeDistributionExponent);
        const float splitFar =
            cascadeNear +
            (cascadeFar - cascadeNear) * splitFraction;

        ShadowProjectionParams params = CalculateShadowMatrixForCameraRange(
            previousSplit,
            splitFar,
            passSizeWidth,
            passSizeHeight);
        shadowCascadeFrameData.view[cascadeIndex] = params.viewMatrix;
        shadowCascadeFrameData.projection[cascadeIndex] = params.projectionMatrix;
        shadowCascadeFrameData.lightViewProj[cascadeIndex] = params.projectionMatrix * params.viewMatrix;
        shadowCascadeFrameData.cascadeSplits[cascadeIndex] = splitFar;

        // 用户只编辑 UE 风格的全局 Bias。每级实际接收面 Bias 在这里派生，
        // Distribution 为 0 时各级相同，为 1 时按级联序号逐级增大。
        const float cascadeBiasScale =
            1.0f +
            csmSettings.shadowCascadeBiasDistribution *
                static_cast<float>(cascadeIndex);
        shadowCascadeFrameData.bias[cascadeIndex] =
            Eigen::Vector4f(
                csmSettings.shadowBias *
                    VL::CsmSettings::MaxReceiverDepthBias *
                    cascadeBiasScale,
                csmSettings.shadowSlopeBias *
                    VL::CsmSettings::MaxSlopeBiasMultiplier,
                0.0f,
                0.0f);
        previousSplit = splitFar;
    }

    for (uint32_t cascadeIndex = csmSettings.cascadeCount;
         cascadeIndex < VL::CsmSettings::MaxCascadeCount;
         ++cascadeIndex)
    {
        shadowCascadeFrameData.cascadeSplits[cascadeIndex] =
            cascadeFar;
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
    environmentUpdateState.Reset();
    environmentUpdateScheduler.Reset();
    environmentUpdateSourceCube.reset();
    RefreshEnvironmentUpdateDiagnostics();

    VL::RendererDescriptorContext descriptorContext = BuildRendererDescriptorContext();
    VL::RendererResourceCache& resourceCache = VL::RendererResourceCache::GetInstance();
    
    PrepareEnvironmentResources(); // 环境光照的资源准备

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

void RenderSystem::RefreshEyeDescriptorsIfNeeded()
{
    if (!eyeComputeReloadParticipant.NeedsDescriptorRefresh())
    {
        return;
    }

    // Eye LUT replacement swaps the World-local texture identity. Refresh the
    // external pass descriptors before recording the next frame; the actual
    // Vulkan allocations were prepared before the Compute owner swap.
    VL::RendererDescriptorContext descriptorContext =
        BuildRendererDescriptorContext();
    RenderGraph::GetInstance().RefreshRuntimeDescriptors(
        *rendererBackend,
        descriptorContext);
    eyeComputeReloadParticipant.MarkDescriptorRefreshHandled();
}
void RenderSystem::RecordAndSubmitCurrentRenderScene()
{
    eyePerformanceFrameStats = {};
    eyePerformanceFrameWithinBudget = ValidateEyePerformanceFrame(
        eyePerformanceBudget,
        eyePerformanceFrameStats,
        &eyePerformanceViolation);
    RefreshEyeDescriptorsIfNeeded();
    const RenderGraph& renderGraph = RenderGraph::GetInstance();

    if (uiRenderSnapshotQueue != nullptr)
    {
        std::shared_ptr<const VL::UiRenderSnapshot> latestUiSnapshot =
            uiRenderSnapshotQueue->ConsumeLatest();
        if (latestUiSnapshot != nullptr)
        {
            currentUiRenderSnapshot = std::move(latestUiSnapshot);
            uiOverlayRenderer.SynchronizeTextures(*currentUiRenderSnapshot);
        }
    }

    // Backend owns swapchain frame synchronization. RenderSystem only records
    // pass work into the command buffer it receives for this image.
    VL::RendererFrameContext frameContext = rendererBackend->BeginFrame(currentFrame);
    uiOverlayRenderer.CollectRetiredTextures();
    uint32_t frameIndex = frameContext.frameIndex;
    swapChainImageIndex = frameContext.swapchainImageIndex;
    vk::CommandBuffer commandBuffer = frameContext.commandBuffer;

    {
        VulkanDebug::ScopedRegion frameRegion(
            commandBuffer,
            "Frame:" + std::to_string(frameIndex) + " Image:" + std::to_string(swapChainImageIndex),
            VulkanDebug::DebugCategory::eDefault);

        // Tick every tree species once so shadow and geometry consume the same
        // per-object wind state for the complete frame.
        AdvanceSpeedTreeWindProfiles();
        // Procedural sky IBL needs the latest camera and sky parameters in global UBO,
        // but environmentSH is GPU-owned and must not be overwritten by this upload.
        UpdateUBOGlobal(commandBuffer);
        // Generate the procedural sky cubemap and write environmentSH before graphics
        // passes read global lighting data.
        RecordEnvironmentIbl(commandBuffer, swapChainImageIndex);

        const auto& renderPassOrdered = renderGraph.GetRenderpassesOrdered();
        for (size_t passIndex = 0; passIndex < renderPassOrdered.size(); ++passIndex)
        {
            const auto& renderPassName = renderPassOrdered[passIndex];
            std::string renderPassScopeName = "RenderPass:" + renderPassName;
            if (renderPassName == "deferredLighting")
            {
                renderPassScopeName = "Eye/Deferred";
            }
            else if (renderPassName == "sssHorizontal" ||
                     renderPassName == "sssVertical")
            {
                renderPassScopeName = "Eye/SSSFilter";
            }
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

        eyePerformanceFrameWithinBudget = ValidateEyePerformanceFrame(
            eyePerformanceBudget,
            eyePerformanceFrameStats,
            &eyePerformanceViolation);

        if (uiOverlayRenderer.IsInitialized() && currentUiRenderSnapshot != nullptr)
        {
            VulkanDebug::ScopedRegion uiRegion(
                commandBuffer,
                "UI Overlay",
                VulkanDebug::DebugCategory::eDefault);
            uiOverlayRenderer.Record(
                commandBuffer,
                swapChainImageIndex,
                *currentUiRenderSnapshot);
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
    return BuildRendererDescriptorContext(
        VL::RendererResourceCache::GetInstance(),
        RenderGraph::GetInstance(),
        GetLightBufferInfo());
}

VL::RendererDescriptorContext
RenderSystem::BuildRendererDescriptorContext(
    const VL::RendererResourceCache& resourceCache,
    RenderGraph& renderGraph,
    const std::vector<vk::DescriptorBufferInfo>&
        lightBufferInfos) const
{
    VL::RendererDescriptorContext descriptorContext;
    descriptorContext.globalUniformBufferInfos = &GetUBOGlobalBufferInfo();
    descriptorContext.lightBufferInfos =
        &lightBufferInfos;
    descriptorContext.resourceCache =
        &resourceCache;
    Renderpass* canonicalShadowPass =
        renderGraph.FindCanonicalShadowPass();
    if (canonicalShadowPass != nullptr)
    {
        std::shared_ptr<MaterialInstance> shadowMaterialInstance =
            canonicalShadowPass->materialInstance.lock();
        if (shadowMaterialInstance)
        {
            descriptorContext.commonOpaqueShadowMaterial =
                shadowMaterialInstance->GetBaseMaterial().lock();
        }
    }
    return descriptorContext;
}

void RenderSystem::AdvanceSpeedTreeWindProfiles()
{
    speedTreeWindProfiles.AdvanceTo(GetSpeedTreeWindTimeSeconds());
}

void RenderSystem::ShutdownRenderObject()
{
    if (shaderReloadCoordinator != nullptr)
    {
        shaderReloadCoordinator->SetUiOverlayParticipant(nullptr);
    }
    uiOverlayRenderer.Shutdown();
    currentUiRenderSnapshot.reset();
    ShutdownFrameResources();
}

void RenderSystem::InitializeFrameResources()
{
    if (rendererBackend == nullptr)
    {
        throw std::runtime_error("RenderSystem cannot initialize frame resources without a renderer backend");
    }
    if (pipelineFactory == nullptr)
    {
        throw std::runtime_error("RenderSystem cannot initialize procedural sky IBL without a pipeline factory");
    }

    frameResources.Initialize(*rendererBackend);
    frameResources.EnsureLightCapacity(currentRenderScene.lights.size(), *rendererBackend);
    proceduralSkyCubeGenerator.Initialize(
        *pipelineFactory,
        *rendererBackend);
    environmentIblBaker.Initialize(
        *pipelineFactory,
        *rendererBackend,
        frameResources.GetGlobalUniformBufferInfos());
    eyeComputeReloadParticipant.Initialize(
        *pipelineFactory,
        *rendererBackend);
    if (shaderReloadCoordinator != nullptr)
    {
        shaderReloadCoordinator->RegisterComputeParticipant(
            &proceduralSkyCubeGenerator);
        shaderReloadCoordinator->RegisterComputeParticipant(
            &skyShReloadParticipant);
        shaderReloadCoordinator->RegisterComputeParticipant(
            &prefilterReloadParticipant);
        shaderReloadCoordinator->RegisterComputeParticipant(
            &eyeComputeReloadParticipant);
    }
    environmentGpuTimer.Initialize(*rendererBackend);
}

void RenderSystem::ShutdownFrameResources()
{
    if (rendererBackend == nullptr)
    {
        return;
    }

    environmentGpuTimer.Shutdown(*rendererBackend);
    if (shaderReloadCoordinator != nullptr)
    {
        shaderReloadCoordinator->UnregisterComputeParticipant(
            &proceduralSkyCubeGenerator);
        shaderReloadCoordinator->UnregisterComputeParticipant(
            &skyShReloadParticipant);
        shaderReloadCoordinator->UnregisterComputeParticipant(
            &prefilterReloadParticipant);
        shaderReloadCoordinator->UnregisterComputeParticipant(
            &eyeComputeReloadParticipant);
    }
    environmentIblBaker.Shutdown(*rendererBackend);
    eyeComputeReloadParticipant.Shutdown();
    proceduralSkyCubeGenerator.Shutdown(*rendererBackend);
    environmentUpdateScheduler.Reset();
    environmentUpdateSourceCube.reset();
    
    frameResources.Shutdown(*rendererBackend);
}

void RenderSystem::ValidateFrameResourceDescriptors()
{
    if (!frameResources.IsInitialized())
    {
        throw std::runtime_error("RenderSystem cannot expose frame resource descriptors before frame resources are initialized");
    }
}

double RenderSystem::GetSpeedTreeWindTimeSeconds()
{
    if (!windClockInitialized)
    {
        windStartTime = std::chrono::steady_clock::now();
        windClockInitialized = true;
        currentWindTimeSeconds = 0.0;
        return currentWindTimeSeconds;
    }

    currentWindTimeSeconds = std::chrono::duration<double>(
        std::chrono::steady_clock::now() - windStartTime).count();
    return currentWindTimeSeconds;
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
        // Current limitation: SpeedTree world bounds describe the undeformed
        // mesh and exclude vertex-shader wind displacement. The foliage bounds
        // contract must eventually provide expanded caster bounds so strong
        // wind cannot clip branch shadows from the CSM light-space Z range.
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

std::shared_ptr<Texture> RenderSystem::GetActiveEnvironmentCube()
{
    return GetEnvironmentCube(
        currentRenderScene,
        VL::RendererResourceCache::GetInstance());
}

std::shared_ptr<Texture> RenderSystem::GetEnvironmentCube(
    const VL::RenderScene& renderScene,
    const VL::RendererResourceCache& resourceCache) const
{
    switch(renderScene.environment.type)
    {
        case VL::EnvironmentType::ProceduralSky:
            return proceduralSkyCubeGenerator.GetActiveEnvironmentCube();
        case VL::EnvironmentType::Hdri:
            const std::shared_ptr<Texture>* environmentCube = 
                resourceCache.GetWorldTexture(
                    "environmentCube");
            if(environmentCube == nullptr || *environmentCube == nullptr)
            {
                throw std::runtime_error("Environment cube not found");
            }
            return *environmentCube;
    }
    throw std::runtime_error("Unknown environment type");
}

void RenderSystem::PrepareEnvironmentResources()
{
    PrepareEnvironmentResources(
        currentRenderScene,
        VL::RendererResourceCache::GetInstance());
}

void RenderSystem::PrepareEnvironmentResources(
    const VL::RenderScene& renderScene,
    VL::RendererResourceCache& resourceCache) const
{
    std::shared_ptr<Texture> environmentCube =
        GetEnvironmentCube(renderScene, resourceCache);
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

void RenderSystem::RecordEnvironmentIbl(vk::CommandBuffer commandBuffer, uint32_t swapchainImageIndex)
{
    PROFILE_SCOPE("EnvironmentIbl");
    environmentGpuTimer.BeginFrame(commandBuffer, swapchainImageIndex);

    // Sky Pass 每帧直接读取最新参数；cubemap、SH 和 prefilter 只在源数据 dirty 时
    // 启动新代际，避免相机移动或单纯强度变化触发昂贵重建。
    environmentUpdateState.Observe(currentRenderScene.environment);
    if (environmentUpdateState.IsDirty())
    {
        const uint64_t requestedGeneration = environmentUpdateState.BeginUpdate();
        const bool startedNewGeneration = environmentUpdateScheduler.RequestUpdate(
            requestedGeneration,
            currentRenderScene.environment,
            environmentIblBaker.GetPrefilterMipCount());
        if (startedNewGeneration)
        {
            // 程序化天空先写 pending cubemap；HDRI 本身已经完整，可以直接作为冻结输入。
            environmentUpdateSourceCube =
                currentRenderScene.environment.type == VL::EnvironmentType::ProceduralSky
                ? proceduralSkyCubeGenerator.GetPendingEnvironmentCube()
                : GetActiveEnvironmentCube();
        }
    }

    VL::EnvironmentUpdateFramePlan framePlan = environmentUpdateScheduler.BuildFramePlan();
    if (!framePlan.HasWork())
    {
        RefreshEnvironmentUpdateDiagnostics();
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

    RefreshEnvironmentUpdateDiagnostics();
}

void RenderSystem::RefreshEnvironmentUpdateDiagnostics()
{
    VL::EnvironmentUpdateDiagnostics snapshot;
    snapshot.progress = environmentUpdateScheduler.GetProgress();
    snapshot.gpuTiming = environmentGpuTimer.GetSnapshot();

    std::lock_guard<std::mutex> lock(environmentDiagnosticsMutex);
    environmentDiagnostics = std::move(snapshot);
}
