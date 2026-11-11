#include "engine/ui/engineUiViewModel.h"

#include <memory>

#include "animation/motionDebug.h"
#include "animation/skeletalPlayback.h"
#include "controller.h"
#include "debug/drawDebug.h"
#include "engine/runtimeCommandExecutor.h"
#include "renderSystem.h"
#include "sceneNode.h"
#include "script/scriptBuildService.h"
#include "script/scriptRuntime.h"
#include "world/world.h"
#include "world/worldManager.h"

namespace VL
{

namespace
{

// 开发者 UI 隐藏且 motion debug 关闭时复用的冻结空视图：避免每帧为
// CaptureDebugDraw 构造一个禁用的 MotionDebugView，也避免缓存随场景增长。
const std::shared_ptr<const Animation::MotionDebugView>& DisabledMotionDebugView()
{
    static const std::shared_ptr<const Animation::MotionDebugView> view =
        std::make_shared<const Animation::MotionDebugView>();
    return view;
}

// 调试叠层的 UI 可见状态签名。快照几何每帧都新分配，但面板只消费
// 开关/类别/owner/计数这些字段；用它们的签名判断“这一节是否真的变了”。
uint64_t UiDebugDrawSignature(const Debug::Snapshot& snapshot)
{
    UiSectionRevision revision;
    revision.Mix(snapshot.enabled ? 1u : 0u)
        .Mix(snapshot.visibleCount)
        .Mix(snapshot.storedCount);
    for (const char character : snapshot.ownerFilter)
    {
        revision.Mix(static_cast<uint64_t>(static_cast<unsigned char>(character)));
    }
    for (const auto& category : snapshot.categories)
    {
        revision.Mix(category.enabled ? 1u : 0u).Mix(category.count);
        for (const char character : category.name)
        {
            revision.Mix(static_cast<uint64_t>(static_cast<unsigned char>(character)));
        }
    }
    for (const auto& owner : snapshot.owners)
    {
        for (const char character : owner)
        {
            revision.Mix(static_cast<uint64_t>(static_cast<unsigned char>(character)));
        }
    }
    return revision.Value();
}

} // namespace

void FillUiViewModelLiveFrame(
    UiViewModelFrameValues& frame,
    UiViewModelBuildMode buildMode,
    const WorldHandle& activeWorld,
    const EngineUiViewModelContext& context)
{
    frame.activeWorldPath = activeWorld.scenePath;
    const RenderSystem& renderSystem = *context.renderSystem;
    frame.offlineMode = renderSystem.IsOfflineMode();
    if (buildMode == UiViewModelBuildMode::RuntimeHud)
    {
        // 运行时 HUD 每帧只冻结游戏文本；类/性能 JSON 属于编辑器面板，不进入这里。
        frame.hudView = context.scriptRuntime->HudView(activeWorld.generation);
    }
    const std::shared_ptr<World>& world = context.worldManager->GetActiveWorld();
    if (!world || frame.offlineMode)
    {
        return;
    }
    // DrawDebug 叠层在沉浸模式仍然显示，因此逐帧冻结相机动图；Motion 的
    // 历史/匹配模型只在开启时构建，关闭时复用一个冻结空视图，避免每个
    // FullEditor 帧都分配一份禁用视图并让相机分节每 10 Hz 误判为变化。
    frame.motionWorldGeneration = world->GetGeneration();
    if (world->IsMotionDebugEnabled())
    {
        frame.motionDebug = world->CaptureMotionDebug();
    }
    else
    {
        frame.motionDebug = DisabledMotionDebugView();
    }
    frame.debugDraw = world->CaptureDebugDraw(*frame.motionDebug);
    if (const auto& camera = world->GetRenderCamera())
    {
        for (int axis = 0; axis < 3; ++axis)
        {
            frame.motionCamera.position[axis] = camera->GetPosition()[axis];
            frame.motionCamera.forward[axis] = camera->GetForwardVector()[axis];
            frame.motionCamera.right[axis] = camera->GetRightVector()[axis];
            frame.motionCamera.up[axis] = camera->GetUpVector()[axis];
        }
        frame.motionCamera.horizontalFovDegrees = camera->GetHFOV();
        frame.motionCamera.clipNear = camera->GetClipNear();
        frame.motionCamera.clipFar = camera->GetClipFar();
    }
}

void BuildUiScriptsSection(
    UiScriptsSection& section,
    const WorldHandle& activeWorld,
    const EngineUiViewModelContext& context)
{
    const ScriptRuntimeSubsystem& scriptRuntime = *context.scriptRuntime;
    const std::shared_ptr<World>& world = context.worldManager->GetActiveWorld();

    section.scripts = scriptRuntime.View(activeWorld.generation);
    section.scripts.buildAvailable = context.scriptBuild && context.scriptBuild->Available();
    section.scripts.buildBusy = context.scriptBuild && context.scriptBuild->Busy();
    section.scripts.autoCompile = context.autoCompileScripts;
    section.scripts.buildOutput = *context.scriptBuildOutput;
    if (world && context.canUndo)
    {
        // CanUndo 内部做 weakly_canonical，是隐藏状态下最贵的一段；只在脚本节
        // 真正重建时求值（选中/命令/世界换代/编译状态事件），静止帧不重复执行。
        section.scripts.canUndo = context.canUndo(world->GetScenePath(), false);
        section.scripts.canRedo = context.canUndo(world->GetScenePath(), true);
    }
}

void BuildUiAnimationSection(
    UiAnimationPreviewSection& section,
    const WorldHandle& activeWorld,
    const EngineUiViewModelContext& context)
{
    const std::shared_ptr<World>& world = context.worldManager->GetActiveWorld();
    if (!world)
    {
        return;
    }
    // 动画时间是时间相关值：允许随 10 Hz 刷新节拍变化；暂停且数值不变时
    // 分节发布会按值比较，不提升版本。
    CaptureUiAnimationPreview(section, *world, activeWorld.generation, context);
}

void BuildUiPerformanceSection(
    UiPerformanceSection& section,
    const EngineUiViewModelContext& context)
{
    // 与 perf json/JSONL 共用同一份冻结快照；这里只做一次值拷贝，
    // 面板随后按自己的 5 Hz 节奏读取，不逐帧接触 RenderSystem。
    section.performance = context.renderSystem->CapturePerformanceSnapshot();
    // 脚本时间/计数器同样属于时间序列；它们不进入事件驱动的脚本分节，
    // 否则 Log 的 Scripts 页在静止帧会显示过期的时间。
    section.scriptPerformance = context.scriptRuntime->PerformanceView();
}

void BuildUiViewportCameraSection(
    UiViewportCameraSection& section,
    const UiViewModelFrameValues& frameValues,
    const EngineUiViewModelContext& context)
{
    const RenderSystem& renderSystem = *context.renderSystem;
    const std::shared_ptr<World>& world = context.worldManager->GetActiveWorld();
    section.debugViewMode = renderSystem.GetDebugViewMode();
    section.toneMappingMode = renderSystem.GetToneMappingMode();
    section.editorGridVisible = renderSystem.IsEditorGridVisible();
    section.motionWorldGeneration = frameValues.motionWorldGeneration;
    section.motionDebug = frameValues.motionDebug;
    section.debugDraw = frameValues.debugDraw;
    if (frameValues.debugDraw)
    {
        section.debugDrawSignature = UiDebugDrawSignature(*frameValues.debugDraw);
    }
    section.motionCamera = frameValues.motionCamera;
    section.cameraMoveSpeed = context.controller != nullptr
        ? context.controller->GetMoveVelocity()
        : 1000.0f;
    if (world && world->GetCamera())
    {
        section.hasCamera = true;
        section.cameraExposure = world->GetCamera()->GetExposure();
    }
}

void BuildUiEnvironmentLightingSection(
    UiEnvironmentLightingSection& section,
    const WorldHandle& activeWorld,
    const EngineUiViewModelContext& context)
{
    (void)activeWorld;
    const RenderSystem& renderSystem = *context.renderSystem;
    const std::shared_ptr<World>& world = context.worldManager->GetActiveWorld();
    section.bloomStrength = renderSystem.GetBloomStrength();
    section.bloomThreshold = renderSystem.GetBloomThreshold();
    section.bloomKnee = renderSystem.GetBloomKnee();
    section.bloomClamp = renderSystem.GetBloomClamp();
    const CsmSettings& csmSettings = renderSystem.GetCsmSettings();
    section.csmCastShadows = csmSettings.castShadows;
    section.csmDynamicShadowDistance = csmSettings.dynamicShadowDistance;
    section.csmDynamicShadowCascades = csmSettings.cascadeCount;
    section.csmCascadeDistributionExponent = csmSettings.cascadeDistributionExponent;
    section.csmCascadeTransitionFraction = csmSettings.cascadeTransitionFraction;
    section.csmShadowDistanceFadeoutFraction = csmSettings.shadowDistanceFadeoutFraction;
    section.csmShadowBias = csmSettings.shadowBias;
    section.csmShadowSlopeBias = csmSettings.shadowSlopeBias;
    section.csmShadowCascadeBiasDistribution = csmSettings.shadowCascadeBiasDistribution;
    section.environmentIntensity = renderSystem.GetEnvironmentIntensity();
    section.sunIntensity = 1.0f;
    if (world && world->GetPrimaryDirectionalLight())
    {
        section.sunIntensity = world->GetPrimaryDirectionalLight()->GetIntensity();
    }
    const EnvironmentUpdateDiagnostics environmentDiagnostics =
        renderSystem.GetEnvironmentUpdateDiagnostics();
    section.environmentActiveGeneration = environmentDiagnostics.progress.activeGeneration;
    section.environmentPendingGeneration = environmentDiagnostics.progress.pendingGeneration;
    section.environmentUpdateStage = ToString(environmentDiagnostics.progress.stage);
    section.environmentCubemapFacesCompleted =
        environmentDiagnostics.progress.cubemapFacesCompleted;
    section.environmentCubemapFaceCount = environmentDiagnostics.progress.cubemapFaceCount;
    section.environmentShUpdatesCompleted = environmentDiagnostics.progress.shUpdatesCompleted;
    section.environmentPrefilterMipsCompleted =
        environmentDiagnostics.progress.prefilterMipsCompleted;
    section.environmentPrefilterMipCount = environmentDiagnostics.progress.prefilterMipCount;
    section.environmentUsesPreviousResources =
        environmentDiagnostics.progress.usingPreviousResources;
    section.environmentGpuTimingSupported = environmentDiagnostics.gpuTiming.supported;
    section.environmentCubemapGpuMs = environmentDiagnostics.gpuTiming
        .Get(EnvironmentGpuProduct::Cubemap).lastMilliseconds;
    section.environmentShGpuMs = environmentDiagnostics.gpuTiming
        .Get(EnvironmentGpuProduct::SphericalHarmonics).lastMilliseconds;
    section.environmentPrefilterGpuMs = environmentDiagnostics.gpuTiming
        .Get(EnvironmentGpuProduct::Prefilter).lastMilliseconds;
    section.environmentCommitGpuMs = environmentDiagnostics.gpuTiming
        .Get(EnvironmentGpuProduct::Commit).lastMilliseconds;
}

void BuildUiOfflineSection(
    UiOfflineSection& section,
    const RuntimeCommandExecutionResult& commandResult,
    const EngineUiViewModelContext& context)
{
    const RenderSystem& renderSystem = *context.renderSystem;
    section.offlineMode = renderSystem.IsOfflineMode();
    section.state = static_cast<int>(renderSystem.GetOfflineState());
    section.samples = renderSystem.GetOfflineSettings().samples;
    section.bounces = renderSystem.GetOfflineSettings().bounces;
    section.lightSamples = renderSystem.GetOfflineSettings().lightSamples;
    section.seed = renderSystem.GetOfflineSettings().seed;
    section.aov = static_cast<int>(renderSystem.GetOfflineAov());
    section.exposure = renderSystem.GetOfflineExposure();
    section.error = renderSystem.GetOfflineError();
    if (const auto result = renderSystem.CaptureOfflineResult())
    {
        section.completedSamples = result->film->completedSamples;
    }
    section.modeFeedback = commandResult.offlineUiModeFeedback;
}

void CaptureUiAnimationPreview(
    UiAnimationPreviewSection& section,
    const World& world,
    uint64_t generation,
    const EngineUiViewModelContext& context)
{
    std::unique_ptr<UiAnimationPreviewCache>& cacheSlot = *context.animationPreviewCache;
    for (const auto& entry : world.GetMeshObjects())
    {
        if (!entry.second.animation ||
            entry.second.sceneObjectIdentity != "preview_model")
        {
            continue;
        }
        const auto& player = *entry.second.animation;
        const auto& animation = player.Asset().AnimationAsset();
        if (!cacheSlot)
        {
            cacheSlot = std::make_unique<UiAnimationPreviewCache>();
        }
        UiAnimationPreviewCache& cache = *cacheSlot;
        // 先比较代数：换代后的旧 player 指针不能解引用，也不能作为缓存命中依据。
        const bool cacheHit = cache.worldGeneration == generation &&
            cache.player == &player &&
            cache.clipIds.size() == animation.clips.size();
        if (!cacheHit)
        {
            cache.player = &player;
            cache.worldGeneration = generation;
            cache.clipIds.clear();
            cache.clipIds.reserve(animation.clips.size());
            for (const auto& clip : animation.clips)
            {
                cache.clipIds.push_back(clip.id);
            }
            // schema 只由骨架决定；通道名只由 schema 决定。二者随播放器身份缓存。
            cache.schema = Animation::PreviewFeatureSchema(animation.mesh);
            cache.featureChannels = Animation::FeatureChannelNames(cache.schema);
        }
        section.worldGeneration = generation;
        section.clips = cache.clipIds;
        section.clip = player.SelectedClip();
        section.time = player.Time();
        section.duration = player.Duration();
        section.playing = player.IsPlaying();
        section.jointCount = player.Asset().mesh.joints.size();
        if (!animation.clips.empty())
        {
            const auto& clip = animation.clips.at(player.SelectedClip());
            section.featureChannels = cache.featureChannels;
            if (Animation::CanExtractClipFeatures(
                    clip, cache.schema, player.Time(), player.IsLooping()))
            {
                section.features = Animation::ExtractClipFeatures(
                    animation.mesh, clip, cache.schema, player.Time(), player.IsLooping());
                section.featureStatus = "Root heading | cm, cm/s | pre-IK";
            }
            else
            {
                section.featureStatus = "Incomplete nonloop feature window";
            }
        }
        break;
    }
}

} // namespace VL
