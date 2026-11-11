#include "ui/uiSubsystem.h"
#include "ui/debugDrawOverlay.h"
#include "ui/gameplayHudView.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <sstream>
#include <string_view>
#include <utility>

#include <RmlUi/Core/Context.h>
#include <RmlUi/Core/Core.h>
#include <RmlUi/Core/Element.h>
#include <RmlUi/Core/ElementDocument.h>
#include <RmlUi/Core/Factory.h>
#include <RmlUi/Core/Input.h>
#include <RmlUi/Core/Elements/ElementFormControl.h>
#include <nlohmann/json.hpp>


#include "engine/diagnosticsSubsystem.h"
#include "engine/runtimeCommand.h"
#include "core/renderSession.h"
#include "platform/platformEvent.h"
#include "platform/platformWindow.h"
#include "ui/sceneObjectTypeSupport.h"
#include "ui/uiBindingUpdateScope.h"
#include "Profiler.h"
#include "ui/uiSubsystemInternal.h"

namespace VL
{

using namespace UiSubsystemDetail;

// 运行时 UI 的分节存储。编辑器资产/大纲节由 EditorUiShell 自己发布，
// 这里只保留不依赖编辑器载荷的运行时聚合。
struct UiSubsystem::Impl final
{
    UiRuntimeSectionStore sectionStore;
};

UiSubsystem::UiSubsystem()
    : impl(std::make_unique<Impl>())
{
}

UiSubsystem::~UiSubsystem()
{
    Shutdown();
}

RuntimeResult<void> UiSubsystem::Initialize(
    const UiSubsystemDesc& initializeDesc,
    PlatformWindow& platformWindow,
    CommandBus& initializeCommandBus,
    const DiagnosticsSubsystem& diagnosticsSubsystem)
{
    Shutdown();
    if (initializeDesc.viewportWidth == 0 || initializeDesc.viewportHeight == 0)
    {
        return RuntimeResult<void>::Failure(MakeRuntimeError(
            "UiSubsystem.InvalidViewport",
            "UI viewport dimensions must be positive."));
    }
    if (!std::filesystem::exists(initializeDesc.assetRoot))
    {
        return RuntimeResult<void>::Failure(MakeRuntimeError(
            "UiSubsystem.AssetRootMissing",
            "UI asset root does not exist.",
            initializeDesc.assetRoot.string()));
    }

    desc = initializeDesc;
    rmlRenderInterface.SetLinearColorOutput(desc.srgbFramebuffer);
    sceneViewportRect = {
        0,
        0,
        desc.viewportWidth,
        desc.viewportHeight};
    window = &platformWindow;
    commandBus = &initializeCommandBus;
    diagnostics = &diagnosticsSubsystem;
    currentLocale = desc.defaultLocale;
    developerUiEnabled = desc.developerUiEnabled;
    developerUiVisible = developerUiEnabled && desc.developerUiVisible;
    nextHotReloadCheck = std::chrono::steady_clock::now();
    LoadSceneOptions();

    RuntimeResult<void> rmlResult = InitializeRmlUi();
    if (rmlResult.IsFailure())
    {
        Shutdown();
        return rmlResult;
    }

    RuntimeResult<void> dataModelResult = InitializeDataModel();
    if (dataModelResult.IsFailure())
    {
        Shutdown();
        return dataModelResult;
    }

    RuntimeResult<void> fontResult = LoadFonts();
    if (fontResult.IsFailure())
    {
        Shutdown();
        return fontResult;
    }

    RuntimeResult<void> assetResult = CommitInitialAssets();
    if (assetResult.IsFailure())
    {
        Shutdown();
        return assetResult;
    }

    initialized = true;
    UpdateInputOwnership();
    diagnostics->ReportInfo(
        "UI subsystem initialized: RmlUi runtime UI enabled, developer UI " +
        std::string(developerUiEnabled ? "enabled" : "disabled"));
    return RuntimeResult<void>::Success();
}

void UiSubsystem::Shutdown()
{
    // 编辑器文档必须先于共享 RmlUi context 卸载；shell 只 detach 文档，
    // 不调用 Rml::Shutdown。
    SetEditorUiShell(nullptr);
    debugDrawLabels.reset();
    snapshotPublisher.Clear();
    uiUpdateScheduler = UiUpdateScheduler{};
    lastPublishedHadDebugOverlay = false;
    if (rmlContext != nullptr)
    {
        ResetRuntimeInputState();
    }

    if (rmlContext != nullptr && runtimeDocument != nullptr)
    {
        rmlContext->UnloadDocument(runtimeDocument);
        runtimeDocument = nullptr;
        rmlContext->Update();
    }
    if (rmlContext != nullptr)
    {
        Rml::RemoveContext(rmlContext->GetName());
        rmlContext = nullptr;
    }
    if (rmlInitialized)
    {
        // 只释放自己那一份；进程级关闭由 RmlUiRuntime 在计数归零时执行。
        VL::Ui::RmlUiRuntime::Release();
        rmlInitialized = false;
    }

    if (window != nullptr)
    {
        window->SetTextInputEnabled(false);
    }
    window = nullptr;
    commandBus = nullptr;
    diagnostics = nullptr;
    localizations.clear();
    assetFingerprint.clear();
    dataModelHandle = Rml::DataModelHandle();
    initialized = false;
    developerUiVisible = false;
    developerUiEnabled = false;
    pendingSectionRefresh = UiSectionAll;
    currentBuildMode = UiViewModelBuildMode::None;
    // 重新初始化后必须从“没有发布过任何分节”开始，否则旧版本会被误认为已应用。
    impl->sectionStore = UiRuntimeSectionStore{};
    appliedSectionVersions = UiSectionVersions{};
    appliedEditorSectionVersions = UiSectionVersions{};
    appliedPerformance = UiPerformanceSection{};
    lastPublishedOfflineMode = false;
    refreshStats = {};
    hudScripts = ScriptRuntimeView{};
    hotReloadStatus.clear();
    rmlKeyPressCounts.clear();
    rmlPressedKeyboardKeys.clear();
    rmlPressedMouseButtons.clear();
    rmlPressedGamepadButtons.clear();
    gamepadConnected = false;
    rmlAxisLeftDown = false;
    rmlAxisRightDown = false;
    rmlAxisUpDown = false;
    rmlAxisDownDown = false;
}

bool UiSubsystem::HandlePlatformEvent(const PlatformEvent& event)
{
    if (!initialized)
    {
        return false;
    }

    if (event.type == PlatformEventType::MouseMove ||
        event.type == PlatformEventType::MouseButtonDown ||
        event.type == PlatformEventType::MouseButtonUp ||
        event.type == PlatformEventType::MouseWheel)
    {
        pointerX = event.mouseX;
        pointerY = event.mouseY;
    }

    if (event.type == PlatformEventType::WindowResized)
    {
        Resize(event.width, event.height);
        return false;
    }

    if (event.type == PlatformEventType::GamepadConnected ||
        event.type == PlatformEventType::GamepadDisconnected)
    {
        if (event.type == PlatformEventType::GamepadDisconnected)
        {
            ResetRmlGamepadInputState();
        }
        gamepadConnected = event.gamepadConnected;
        UpdateInputOwnership();
        return false;
    }

    if (event.type == PlatformEventType::WindowFocusGained ||
        event.type == PlatformEventType::WindowFocusLost)
    {
        ProcessRuntimeUiEvent(event);
        UpdateInputOwnership();
        return false;
    }

    bool consumed = false;
    if (editorUiShell != nullptr && developerUiVisible &&
        editorUiShell->HandleGlobalShortcut(event, consumed) && consumed)
    {
        return true;
    }

    if (HandleGlobalShortcut(event))
    {
        return true;
    }

    if (editorUiShell != nullptr && developerUiVisible)
    {
        consumed = false;
        editorUiShell->HandlePlatformEvent(event, consumed);
    }

    UpdateInputOwnership();
    return consumed;
}

void UiSubsystem::Update(const UiViewModelSectionUpdate& sections,
    const UiViewModelFrameValues& frameValues,
    UiViewModelBuildMode mode,
    bool editorSnapshotDue)
{
    PROFILE_SCOPE("UI.Update");
    if (!initialized)
    {
        return;
    }
    currentBuildMode = mode;

    if (mode == UiViewModelBuildMode::None)
    {
        // 编辑器与 HUD 都不可见（沉浸模式）：只冻结 DrawDebug 叠层需要的最小字段，
        // 保留 lastViewModel 的编辑器字段以免 TickMaterialInstanceEditor 丢失蓝图类，
        // 并发布仅调试图元的快照覆盖 RT 保留的上一帧工作区画面。
        SetImmersiveDocumentStateIfRestored(frameValues.activeWorldPath);
        ApplyFrameValues(frameValues);
        // 隐藏期间没有分节发布：重新可见的第一帧由引擎的全部节重建覆盖。
        // 调度器记住“隐藏”，使该帧必然被判定为 surfaceChanged。
        uiUpdateScheduler.MarkHidden();
        BuildAndPublishRenderSnapshot(false);
        UpdateInputOwnership();
        return;
    }

    const UiUpdateScheduler::Clock::time_point now =
        UiUpdateScheduler::Clock::now();

    if (mode == UiViewModelBuildMode::RuntimeHud)
    {
        // HUD 只有两条文本绑定：脚本节不参与，逐帧只冻结 hudLines。
        // 编辑器投影里的类/反馈字段保持不变，TickMaterialInstanceEditor 不会丢蓝图类。
        ApplyFrameValues(frameValues);
        SetImmersiveDocumentStateIfRestored(frameValues.activeWorldPath);
        hudScripts = frameValues.hudView;
        UiBindingUpdateScope bindingUpdate(updatingRuntimeBindings);
        TryHotReload();
        // World 路径变化仍要立刻反映到 HUD 标题；只在真正变化时重脏 binding。
        // 绑定变化本身就是一次内容突变，必须强制本轮更新，不能等兜底节拍。
        const bool worldTextChanged = SyncWorldTextBinding();
        const bool gameplayTextChanged = SyncGameplayTextBinding(hudScripts);
        if (worldTextChanged || gameplayTextChanged)
        {
            uiUpdateScheduler.MarkMutation(UiMutationKind::ForcedRefresh);
        }
        const UiUpdateDecision decision = uiUpdateScheduler.Evaluate(
            UiUpdateSurface::RuntimeHud, false, IsDebugDrawOverlayAnimating(), now);
        if (decision.updateRml)
        {
            PROFILE_SCOPE("UI.RmlUpdate");
            rmlContext->Update();
            uiUpdateScheduler.MarkUpdated(UiUpdateSurface::RuntimeHud, now);
            ++refreshStats.rmlUpdates;
        }
        if (decision.publishSnapshot)
        {
            BuildAndPublishRenderSnapshot(true);
        }
        else
        {
            snapshotPublisher.Skip();
            ++refreshStats.skippedUpdates;
        }
        UpdateInputOwnership();
        return;
    }

    // FullEditor：逐帧只更新冻结值/分节数据；RmlUi 上下文与绘制快照只在
    // 有输入、DOM 突变、分节变化或 10 Hz 兜底到期时推进，静止帧保留上一份快照。
    ApplyFrameValues(frameValues);
    SetImmersiveDocumentStateIfRestored(frameValues.activeWorldPath);
    PublishStatusSection(frameValues);
    if (editorSnapshotDue)
    {
        if (sections.scripts &&
            impl->sectionStore.PublishScripts(std::move(*sections.scripts), sections.scriptsRevision))
            RecordSectionPublish(UiSectionId::Scripts);
        if (sections.animation &&
            impl->sectionStore.PublishAnimation(std::move(*sections.animation)))
            RecordSectionPublish(UiSectionId::Animation);
        if (sections.performance &&
            impl->sectionStore.PublishPerformance(std::move(*sections.performance)))
            RecordSectionPublish(UiSectionId::Performance);
        if (sections.viewportCamera &&
            impl->sectionStore.PublishViewportCamera(std::move(*sections.viewportCamera)))
            RecordSectionPublish(UiSectionId::ViewportCamera);
        if (sections.environmentLighting &&
            impl->sectionStore.PublishEnvironmentLighting(std::move(*sections.environmentLighting)))
            RecordSectionPublish(UiSectionId::EnvironmentLighting);
        if (sections.offline &&
            impl->sectionStore.PublishOffline(std::move(*sections.offline)))
            RecordSectionPublish(UiSectionId::Offline);
    }

    const UiSectionVersions versions = impl->sectionStore.Versions();
    UiSectionMask changed = versions.ChangedFrom(appliedSectionVersions);
    ApplyChangedSections(changed);
    appliedSectionVersions = versions;

    // 编辑器分节/面板由 shell 自己发布和构建；这里只合并版本，让一次
    // 面板/Dom 改动同样强制本帧 RmlUi 更新与快照发布。
    if (editorUiShell != nullptr)
    {
        editorUiShell->UpdateSections(
            lastViewModel,
            appliedPerformance,
            editorSnapshotDue,
            changed);
        const UiSectionVersions editorVersions = editorUiShell->Versions();
        changed |= editorVersions.ChangedFrom(appliedEditorSectionVersions);
        appliedEditorSectionVersions = editorVersions;
    }

    UiBindingUpdateScope bindingUpdate(updatingRuntimeBindings);
    TryHotReload();
    const UiUpdateDecision decision = uiUpdateScheduler.Evaluate(
        UiUpdateSurface::Workspace, changed != 0, IsDebugDrawOverlayAnimating(), now);
    if (decision.updateRml)
    {
        PROFILE_SCOPE("UI.RmlUpdate");
        rmlContext->Update();
        uiUpdateScheduler.MarkUpdated(UiUpdateSurface::Workspace, now);
        ++refreshStats.rmlUpdates;
    }
    if (decision.publishSnapshot)
    {
        // 需要更新（或 DrawDebug 逐帧变化）时才重建并发布绘制快照；
        // 面板数据（BuildDeveloperPanels）仍按分节变化独立重建。
        BuildAndPublishRenderSnapshot(true);
    }
    else
    {
        // 跳过帧不发布：RT 保留上一次消费到的不可变快照，继续绘制同样的几何。
        snapshotPublisher.Skip();
        ++refreshStats.skippedUpdates;
    }
    UpdateInputOwnership();
}

void UiSubsystem::ApplyFrameValues(const UiViewModelFrameValues& frameValues)
{
    lastViewModel.frameIndex = frameValues.frameIndex;
    lastViewModel.deltaTimeSeconds = frameValues.deltaTimeSeconds;
    lastViewModel.framesPerSecond = frameValues.framesPerSecond;
    lastViewModel.cameraMoveSpeed = frameValues.cameraMoveSpeed;
    lastViewModel.activeWorldPath = frameValues.activeWorldPath;
    lastViewModel.offlineMode = frameValues.offlineMode;
    lastViewModel.motionWorldGeneration = frameValues.motionWorldGeneration;
    lastViewModel.motionDebug = frameValues.motionDebug;
    lastViewModel.motionCamera = frameValues.motionCamera;
    lastViewModel.debugDraw = frameValues.debugDraw;
}

void UiSubsystem::ApplyChangedSections(UiSectionMask changed)
{
    const UiRuntimeViewModelSections& published = impl->sectionStore.Sections();
    if ((changed & UiSectionBit(UiSectionId::Scripts)) != 0)
    {
        lastViewModel.scripts = published.scripts.data->scripts;
        SyncGameplayTextBinding(lastViewModel.scripts);
    }
    if ((changed & UiSectionBit(UiSectionId::Animation)) != 0)
    {
        const UiAnimationPreviewSection& animation = *published.animation.data;
        lastViewModel.animationWorldGeneration = animation.worldGeneration;
        lastViewModel.animationClips = animation.clips;
        lastViewModel.animationClip = animation.clip;
        lastViewModel.animationTime = animation.time;
        lastViewModel.animationDuration = animation.duration;
        lastViewModel.animationPlaying = animation.playing;
        lastViewModel.animationJointCount = animation.jointCount;
        lastViewModel.animationFeatureChannels = animation.featureChannels;
        lastViewModel.animationFeatures = animation.features;
        lastViewModel.animationFeatureStatus = animation.featureStatus;
    }
    if ((changed & UiSectionBit(UiSectionId::ViewportCamera)) != 0)
    {
        const UiViewportCameraSection& view = *published.viewportCamera.data;
        lastViewModel.hasCamera = view.hasCamera;
        lastViewModel.cameraExposure = view.cameraExposure;
        lastViewModel.debugViewMode = view.debugViewMode;
        lastViewModel.toneMappingMode = view.toneMappingMode;
        // motionDebug/debugDraw/motionCamera 是逐帧冻结值（ApplyFrameValues 已写入）；
        // 分节只在面板可见状态变化时提供同一帧的值，不在这里回退成旧指针。
        lastViewModel.cameraMoveSpeed = view.cameraMoveSpeed;
    }
    if ((changed & UiSectionBit(UiSectionId::EnvironmentLighting)) != 0)
    {
        const UiEnvironmentLightingSection& environment = *published.environmentLighting.data;
        lastViewModel.bloomStrength = environment.bloomStrength;
        lastViewModel.bloomThreshold = environment.bloomThreshold;
        lastViewModel.bloomKnee = environment.bloomKnee;
        lastViewModel.bloomClamp = environment.bloomClamp;
        lastViewModel.csmCastShadows = environment.csmCastShadows;
        lastViewModel.csmDynamicShadowDistance = environment.csmDynamicShadowDistance;
        lastViewModel.csmDynamicShadowCascades = environment.csmDynamicShadowCascades;
        lastViewModel.csmCascadeDistributionExponent =
            environment.csmCascadeDistributionExponent;
        lastViewModel.csmCascadeTransitionFraction = environment.csmCascadeTransitionFraction;
        lastViewModel.csmShadowDistanceFadeoutFraction =
            environment.csmShadowDistanceFadeoutFraction;
        lastViewModel.csmShadowBias = environment.csmShadowBias;
        lastViewModel.csmShadowSlopeBias = environment.csmShadowSlopeBias;
        lastViewModel.csmShadowCascadeBiasDistribution =
            environment.csmShadowCascadeBiasDistribution;
        lastViewModel.environmentIntensity = environment.environmentIntensity;
        lastViewModel.sunIntensity = environment.sunIntensity;
        lastViewModel.environmentActiveGeneration = environment.environmentActiveGeneration;
        lastViewModel.environmentPendingGeneration = environment.environmentPendingGeneration;
        lastViewModel.environmentUpdateStage = environment.environmentUpdateStage;
        lastViewModel.environmentCubemapFacesCompleted =
            environment.environmentCubemapFacesCompleted;
        lastViewModel.environmentCubemapFaceCount = environment.environmentCubemapFaceCount;
        lastViewModel.environmentShUpdatesCompleted = environment.environmentShUpdatesCompleted;
        lastViewModel.environmentPrefilterMipsCompleted =
            environment.environmentPrefilterMipsCompleted;
        lastViewModel.environmentPrefilterMipCount = environment.environmentPrefilterMipCount;
        lastViewModel.environmentUsesPreviousResources =
            environment.environmentUsesPreviousResources;
        lastViewModel.environmentGpuTimingSupported = environment.environmentGpuTimingSupported;
        lastViewModel.environmentCubemapGpuMs = environment.environmentCubemapGpuMs;
        lastViewModel.environmentShGpuMs = environment.environmentShGpuMs;
        lastViewModel.environmentPrefilterGpuMs = environment.environmentPrefilterGpuMs;
        lastViewModel.environmentCommitGpuMs = environment.environmentCommitGpuMs;
    }
    if ((changed & UiSectionBit(UiSectionId::Offline)) != 0)
    {
        const UiOfflineSection& offline = *published.offline.data;
        // UI 侧“已请求模式/提示文本”不在冻结载荷里，只在这里与引擎结果对账。
        UiViewModelSnapshot previous;
        previous.offlineMode = lastPublishedOfflineMode;
        previous.offlineRequestedMode = lastViewModel.offlineRequestedMode;
        previous.offlineUiNotice = lastViewModel.offlineUiNotice;
        lastViewModel.offlineMode = offline.offlineMode;
        lastViewModel.offlineState = offline.state;
        lastViewModel.offlineSamples = offline.samples;
        lastViewModel.offlineCompletedSamples = offline.completedSamples;
        lastViewModel.offlineBounces = offline.bounces;
        lastViewModel.offlineLightSamples = offline.lightSamples;
        lastViewModel.offlineSeed = offline.seed;
        lastViewModel.offlineAov = offline.aov;
        lastViewModel.offlineExposure = offline.exposure;
        lastViewModel.offlineError = offline.error;
        lastViewModel.offlineModeFeedback = offline.modeFeedback;
        ReconcileOfflineModeFeedback(lastViewModel, previous);
        lastPublishedOfflineMode = offline.offlineMode;
    }
    if ((changed & UiSectionBit(UiSectionId::Performance)) != 0)
    {
        // Performance 页自己的取样器在每次到期窗口调用 BuildPerformance；
        // 这里只保存冻结值，不触发面板 Build。
        appliedPerformance = *published.performance.data;
    }
    if ((changed & UiSectionBit(UiSectionId::Status)) != 0)
    {
        const UiStatusSection& status = *published.status.data;
        lastViewModel.locale = status.locale;
        lastViewModel.developerUiVisible = status.developerUiVisible;
        lastViewModel.activeWorldPath = status.activeWorldPath;
        SyncWorldTextBinding();
    }
}

bool UiSubsystem::SyncWorldTextBinding()
{
    const std::string worldText =
        std::filesystem::path(lastViewModel.activeWorldPath).stem().string();
    if (bindingData.worldText != worldText)
    {
        bindingData.worldText = worldText;
        dataModelHandle.DirtyVariable("world_text");
        ++refreshStats.bindingDirtyWrites;
        return true;
    }
    return false;
}

bool UiSubsystem::SyncGameplayTextBinding(const ScriptRuntimeView& scripts)
{
    // GT 只消费冻结文本；RmlUi 插值负责转义，RT 仍只接收既有几何快照。
    const std::string gameplayText = BuildGameplayHudText(scripts);
    if (bindingData.gameplayText != gameplayText)
    {
        bindingData.gameplayText = gameplayText;
        dataModelHandle.DirtyVariable("gameplay_text");
        ++refreshStats.bindingDirtyWrites;
        return true;
    }
    return false;
}

void UiSubsystem::PublishStatusSection(const UiViewModelFrameValues& frameValues)
{
    UiStatusSection status;
    status.locale = currentLocale;
    status.hotReloadStatus = hotReloadStatus;
    status.developerUiVisible = developerUiVisible;
    status.activeWorldPath = frameValues.activeWorldPath;
    if (impl->sectionStore.PublishStatus(std::move(status)))
    {
        RecordSectionPublish(UiSectionId::Status);
    }
}

void UiSubsystem::SetImmersiveDocumentStateIfRestored(
    const std::string& activeWorldPath)
{
    if (immersiveLevelPath.empty() || !immersiveLoadCompleted)
    {
        return;
    }
    // 完成通知本身不代表成功；必须核对事务发布后的 World，禁止放大仍存活的模型预览。
    const bool restored = NormalizePathForComparison(activeWorldPath) ==
        NormalizePathForComparison(immersiveLevelPath);
    immersiveLevelPath.clear();
    immersiveLoadCompleted = false;
    if (restored) SetImmersiveView(true);
    else diagnostics->ReportWarning("Immersive mode cancelled: the level was not restored.");
}

void UiSubsystem::Resize(uint32_t width, uint32_t height)
{
    if (width == 0 || height == 0)
    {
        return;
    }

    desc.viewportWidth = width;
    desc.viewportHeight = height;
    sceneViewportRect = {0, 0, width, height};
    // 尺寸变化后场景视口矩形/布局都已过期，必须重跑一次 RmlUi 更新并重发快照。
    uiUpdateScheduler.MarkMutation(UiMutationKind::Resize);
    if (rmlContext != nullptr)
    {
        rmlContext->SetDimensions(Rml::Vector2i(static_cast<int>(width), static_cast<int>(height)));
    }
    if (editorUiShell != nullptr)
        editorUiShell->Resize(width, height);
}

bool UiSubsystem::IsScenePickTarget(float mouseX, float mouseY) const
{
    return developerUiVisible && editorUiShell != nullptr &&
        editorUiShell->IsScenePickTarget(mouseX, mouseY);
}

void UiSubsystem::ApplyAction(const UiAction& action)
{
    // 任何模式/本地化/可见性动作都可能改变面板显示内容：下一帧强制重建，
    // 不等 10 Hz 节拍。
    RequestViewModelRefresh();
    switch (action.type)
    {
    case UiActionType::ToggleDeveloperUi:
        if (developerUiEnabled)
        {
            if (!immersiveLevelPath.empty())
            {
                immersiveLevelPath.clear();
                immersiveLoadCompleted = false;
            }
            else if (!developerUiVisible) SetImmersiveView(false);
            else if (editorUiShell != nullptr &&
                editorUiShell->PrepareImmersiveView(immersiveLevelPath))
            {
                if (immersiveLevelPath.empty()) SetImmersiveView(true);
                else
                {
                    immersiveLoadCompleted = false;
                    UiAction load;
                    load.type = UiActionType::LoadWorld;
                    load.stringValue = immersiveLevelPath;
                    RequestRuntimeAction(std::move(load));
                }
            }
        }
        ApplyDocumentState();
        break;
    case UiActionType::SetLocale:
        if (localizations.find(action.stringValue) != localizations.end())
        {
            currentLocale = action.stringValue;
            SyncBindingData();
            dataModelHandle.DirtyAllVariables();
            // 本地化切换重写了所有静态文本：必须立即完整更新并重发快照。
            uiUpdateScheduler.MarkMutation(UiMutationKind::LocaleCommit);
        }
        break;
    default:
        break;
    }
    UpdateInputOwnership();
}

void UiSubsystem::SetImmersiveView(bool enabled)
{
    immersiveView = enabled;
    developerUiVisible = !enabled;
    // 退出沉浸模式的第一帧必须立即恢复完整工作区，不能等待节拍窗口。
    RequestViewModelRefresh();
    ResetRuntimeInputState();
    ApplyDocumentState();
}

void UiSubsystem::NotifyWorldChanged(
    const std::string& worldPath,
    uint64_t generation)
{
    // World 换代等于全部输入换了一份：所有分节强制重建。
    RequestViewModelRefresh();
    // 分节版本可能碰巧相同（同名对象/空场景），但面板的 World 相关内容已经失效，
    // 仍要强制一次 RmlUi 更新与快照重发。
    uiUpdateScheduler.MarkMutation(UiMutationKind::WorldChange);
    if (editorUiShell != nullptr)
        editorUiShell->NotifyWorldChanged(worldPath, generation);
}

void UiSubsystem::NotifyWorldLoadCompleted()
{
    // World 事务完成后脚本/大纲/资产回馈都可能换了一份，整体强制一帧。
    RequestViewModelRefresh();
    if (!immersiveLevelPath.empty()) immersiveLoadCompleted = true;
    if (!bindingData.sceneLoading)
    {
        return;
    }
    bindingData.sceneLoading = false;
}

void UiSubsystem::TickMaterialInstanceEditor()
{
    if (!initialized || editorUiShell == nullptr)
        return;
    editorUiShell->Tick(
        lastViewModel.deltaTimeSeconds,
        lastViewModel,
        appliedSectionVersions.Version(UiSectionId::Scripts));
}

bool UiSubsystem::SelectSceneObjectFromPick(
    const MaterialInstanceSelection& selection)
{
    return developerUiEnabled && editorUiShell != nullptr &&
        editorUiShell->SelectSceneObjectFromPick(selection);
}

void UiSubsystem::SetMaterialInstanceSceneModels(UiEditorSnapshotPayload payload)
{
    if (editorUiShell != nullptr)
        editorUiShell->SetMaterialInstanceSceneModels(std::move(payload));
}

// 大纲快照按内容替换：同一份数组的 shared_ptr 跨刷新复用，选择态按稳定身份保留，
// 身份消失时清空，防止把旧 World 的选择静默套到同名新对象上。
void UiSubsystem::SetSceneActors(UiEditorSnapshotPayload payload)
{
    if (editorUiShell != nullptr)
        editorUiShell->SetSceneActors(std::move(payload));
}

std::optional<MaterialInstanceModelContext>
UiSubsystem::ConsumeMaterialInstanceModelSelectionRequest()
{
    return editorUiShell != nullptr
        ? editorUiShell->ConsumeMaterialInstanceModelSelectionRequest()
        : std::nullopt;
}

std::optional<MaterialInstanceSelection>
UiSubsystem::ConsumeMaterialInstanceSelectionRequest()
{
    return editorUiShell != nullptr
        ? editorUiShell->ConsumeMaterialInstanceSelectionRequest()
        : std::nullopt;
}

bool UiSubsystem::ConsumeSceneSelectionClearRequest()
{
    // 与 selection 请求同帧消费一次：EngineLoop 据此清掉渲染端描边。
    return editorUiShell != nullptr &&
        editorUiShell->ConsumeSceneSelectionClearRequest();
}

void UiSubsystem::ClearMaterialInstanceSceneSelection() noexcept
{
    if (editorUiShell != nullptr)
        editorUiShell->ClearMaterialInstanceSceneSelection();
}

bool UiSubsystem::RequestModelSelection(
    const MaterialInstanceModelContext& model)
{
    return developerUiEnabled && editorUiShell != nullptr &&
        editorUiShell->RequestModelSelection(model);
}

bool UiSubsystem::RequestMaterialSelection(
    const MaterialInstanceSelection& selection)
{
    return developerUiEnabled && editorUiShell != nullptr &&
        editorUiShell->RequestMaterialSelection(selection);
}

bool UiSubsystem::RequestActorSelection(const std::string& actorIdentity)
{
    return developerUiEnabled && editorUiShell != nullptr &&
        editorUiShell->RequestActorSelection(actorIdentity);
}

bool UiSubsystem::ShouldUseRelativeMouseModeForGame() const
{
    if (!initialized) return false;
    if (editorUiShell != nullptr && editorUiShell->IsAssetEditorVisible())
        return false;
    // 捕获由 Viewport 点击建立，不能再用绝对光标位置反向取消；
    // 相对鼠标移动期间 SDL 的绝对坐标可能落在 Viewport 外。
    return !developerUiVisible ||
        (editorUiShell != nullptr && editorUiShell->IsSceneViewportInputActive());
}

bool UiSubsystem::CanRenderThumbnails() const
{
    return developerUiVisible && editorUiShell != nullptr &&
        !editorUiShell->IsSceneViewportInputActive() &&
        !editorUiShell->IsPointerCaptured();
}

bool UiSubsystem::ShouldGameReceiveKeyboard() const
{
    return !initialized || inputOwnership.keyboardOwner == UiInputOwner::Game ||
        inputOwnership.keyboardOwner == UiInputOwner::None;
}

bool UiSubsystem::ShouldGameReceivePointer() const
{
    return !initialized || inputOwnership.pointerOwner == UiInputOwner::Game ||
        inputOwnership.pointerOwner == UiInputOwner::None;
}

} // namespace VL
