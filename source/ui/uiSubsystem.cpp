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
#include "engine/editorInteractionPolicy.h"
#include "engine/runtimeCommand.h"
#include "core/renderSession.h"
#include "platform/platformEvent.h"
#include "platform/platformWindow.h"
#include "ui/sceneObjectTypeSupport.h"
#include "ui/uiBindingUpdateScope.h"
#include "Profiler.h"

namespace VL
{

namespace
{

constexpr float GamepadNavigationThreshold = 0.55f;

std::string NormalizePathForComparison(const std::filesystem::path& path)
{
    if (path.empty())
    {
        return {};
    }

    std::error_code error;
    std::filesystem::path normalizedPath = std::filesystem::absolute(path, error);
    if (error)
    {
        normalizedPath = path;
    }
    std::string normalized = normalizedPath.lexically_normal().generic_string();
#ifdef _WIN32
    for (char& character : normalized)
    {
        character = static_cast<char>(std::tolower(static_cast<unsigned char>(character)));
    }
#endif
    return normalized;
}

// UI 动作只声明它可能改变的视图模型分节。事件驱动路径仍然即时生效，
// 但不会让一次相机参数修改顺带重建资产/大纲/脚本三类昂贵数据。
UiSectionMask SectionsForUiAction(UiActionType type)
{
    switch (type)
    {
    case UiActionType::ToggleDeveloperUi:
    case UiActionType::SetLocale:
    case UiActionType::Quit:
        return UiSectionBit(UiSectionId::Status);
    case UiActionType::SetCameraMoveSpeed:
    case UiActionType::SetDebugViewMode:
    case UiActionType::SetToneMappingMode:
    case UiActionType::SetCameraExposure:
        return UiSectionBit(UiSectionId::ViewportCamera);
    case UiActionType::NavigateModelPreview:
    case UiActionType::ControlAnimationPreview:
    case UiActionType::ControlScriptAnimation:
    case UiActionType::ControlMotionDebug:
    case UiActionType::ControlMotionReplay:
    case UiActionType::ControlDebugDraw:
    case UiActionType::FrameEditorActor:
        return UiSectionBit(UiSectionId::ViewportCamera) |
            UiSectionBit(UiSectionId::Animation) |
            UiSectionBit(UiSectionId::SceneOutliner);
    case UiActionType::SetOfflineMode:
    case UiActionType::SetOfflineSetting:
    case UiActionType::SetOfflineAov:
    case UiActionType::SetOfflineExposure:
    case UiActionType::ControlOfflineSession:
    case UiActionType::ExportOffline:
        return UiSectionBit(UiSectionId::Offline);
    case UiActionType::SetBloomStrength:
    case UiActionType::SetBloomThreshold:
    case UiActionType::SetBloomKnee:
    case UiActionType::SetBloomClamp:
    case UiActionType::SetCsmCastShadows:
    case UiActionType::SetCsmDynamicShadowDistance:
    case UiActionType::SetCsmDynamicShadowCascades:
    case UiActionType::SetCsmCascadeDistributionExponent:
    case UiActionType::SetCsmCascadeTransitionFraction:
    case UiActionType::SetCsmShadowDistanceFadeoutFraction:
    case UiActionType::SetCsmShadowBias:
    case UiActionType::SetCsmShadowSlopeBias:
    case UiActionType::SetCsmShadowCascadeBiasDistribution:
    case UiActionType::SaveCsmSettingsToScene:
    case UiActionType::SetEnvironmentIntensity:
    case UiActionType::SetSunIntensity:
        return UiSectionBit(UiSectionId::EnvironmentLighting);
    case UiActionType::SetActorVisible:
    case UiActionType::SetActorProperty:
        return UiSectionBit(UiSectionId::SceneOutliner) |
            UiSectionBit(UiSectionId::EnvironmentLighting) |
            UiSectionBit(UiSectionId::ViewportCamera);
    case UiActionType::ControlScripts:
    case UiActionType::ReloadScripts:
    case UiActionType::CompileScripts:
    case UiActionType::SetAutoCompileScripts:
    case UiActionType::SetScriptTimeScale:
    case UiActionType::AddScriptBlueprint:
    case UiActionType::SetScriptProperty:
    case UiActionType::SetScriptComponent:
    case UiActionType::AddScriptComponent:
    case UiActionType::RemoveScriptComponent:
    case UiActionType::SetScriptLogicComponent:
    case UiActionType::AddScriptLogicComponent:
    case UiActionType::RemoveScriptLogicComponent:
    case UiActionType::RenameScriptBlueprint:
    case UiActionType::DuplicateScriptBlueprint:
    case UiActionType::DeleteScriptBlueprint:
    case UiActionType::SetScriptPhysics:
    case UiActionType::UndoSceneEdit:
    case UiActionType::RedoSceneEdit:
    case UiActionType::CreateScriptBlueprintAsset:
    case UiActionType::OpenScriptSource:
        return UiSectionBit(UiSectionId::Scripts) |
            UiSectionBit(UiSectionId::Assets) |
            UiSectionBit(UiSectionId::SceneOutliner);
    case UiActionType::LoadWorld:
    case UiActionType::PreviewModel:
    case UiActionType::PreviewRetarget:
    case UiActionType::PreviewMaterial:
    case UiActionType::PreviewBlueprint:
    case UiActionType::TransformBlueprintPreview:
        // World/预览换代：世界路径、相机、资产与大纲都可能整体替换。
        return UiSectionAll;
    }
    return UiSectionAll;
}

bool IsSceneOptionLess(const UiSceneOption& left, const UiSceneOption& right)
{
    std::string leftSortKey = left.label;
    std::string rightSortKey = right.label;
    for (char& character : leftSortKey)
    {
        character = static_cast<char>(std::tolower(static_cast<unsigned char>(character)));
    }
    for (char& character : rightSortKey)
    {
        character = static_cast<char>(std::tolower(static_cast<unsigned char>(character)));
    }

    // 场景显示名是面向用户的标签，排序时忽略 ASCII 大小写，避免小写资源名沉到列表末尾。
    if (leftSortKey != rightSortKey)
    {
        return leftSortKey < rightSortKey;
    }
    if (left.label != right.label)
    {
        return left.label < right.label;
    }
    return left.path < right.path;
}

Rml::Input::KeyIdentifier ToRmlKey(PlatformKey key)
{
    switch (key)
    {
    case PlatformKey::Tab: return Rml::Input::KI_TAB;
    case PlatformKey::Left: return Rml::Input::KI_LEFT;
    case PlatformKey::Right: return Rml::Input::KI_RIGHT;
    case PlatformKey::Up: return Rml::Input::KI_UP;
    case PlatformKey::Down: return Rml::Input::KI_DOWN;
    case PlatformKey::PageUp: return Rml::Input::KI_PRIOR;
    case PlatformKey::PageDown: return Rml::Input::KI_NEXT;
    case PlatformKey::Home: return Rml::Input::KI_HOME;
    case PlatformKey::End: return Rml::Input::KI_END;
    case PlatformKey::Insert: return Rml::Input::KI_INSERT;
    case PlatformKey::Delete: return Rml::Input::KI_DELETE;
    case PlatformKey::Backspace: return Rml::Input::KI_BACK;
    case PlatformKey::Space: return Rml::Input::KI_SPACE;
    case PlatformKey::Enter: return Rml::Input::KI_RETURN;
    case PlatformKey::Escape: return Rml::Input::KI_ESCAPE;
    case PlatformKey::A: return Rml::Input::KI_A;
    case PlatformKey::S: return Rml::Input::KI_S;
    case PlatformKey::C: return Rml::Input::KI_C;
    case PlatformKey::V: return Rml::Input::KI_V;
    case PlatformKey::X: return Rml::Input::KI_X;
    case PlatformKey::Y: return Rml::Input::KI_Y;
    case PlatformKey::Z: return Rml::Input::KI_Z;
    case PlatformKey::F1: return Rml::Input::KI_F1;
    case PlatformKey::F10: return Rml::Input::KI_F10;
    case PlatformKey::F11: return Rml::Input::KI_F11;
    case PlatformKey::Num0: return Rml::Input::KI_0;
    case PlatformKey::Num1: return Rml::Input::KI_1;
    case PlatformKey::Num2: return Rml::Input::KI_2;
    case PlatformKey::Num3: return Rml::Input::KI_3;
    case PlatformKey::Num4: return Rml::Input::KI_4;
    case PlatformKey::Num5: return Rml::Input::KI_5;
    case PlatformKey::Num6: return Rml::Input::KI_6;
    case PlatformKey::Num7: return Rml::Input::KI_7;
    case PlatformKey::Num8: return Rml::Input::KI_8;
    case PlatformKey::Num9: return Rml::Input::KI_9;
    default: return Rml::Input::KI_UNKNOWN;
    }
}

int ToRmlModifiers(const PlatformEvent& event)
{
    int modifiers = 0;
    if (event.control) modifiers |= Rml::Input::KM_CTRL;
    if (event.shift) modifiers |= Rml::Input::KM_SHIFT;
    if (event.alt) modifiers |= Rml::Input::KM_ALT;
    if (event.super) modifiers |= Rml::Input::KM_META;
    return modifiers;
}

int ToRmlMouseButton(PlatformMouseButton button)
{
    switch (button)
    {
    case PlatformMouseButton::Left: return 0;
    case PlatformMouseButton::Right: return 1;
    case PlatformMouseButton::Middle: return 2;
    case PlatformMouseButton::X1: return 3;
    case PlatformMouseButton::X2: return 4;
    default: return -1;
    }
}

// 这些事件交给 RmlUi 后可能改变 hover/pressed/focus/滚动/文本或拖动状态；
// 第 3 层调度必须在同一帧强制一次上下文更新，不能只等 10 Hz 兜底。
bool IsRmlInputEvent(PlatformEventType type)
{
    switch (type)
    {
    case PlatformEventType::KeyDown:
    case PlatformEventType::KeyUp:
    case PlatformEventType::TextInput:
    case PlatformEventType::TextEditing:
    case PlatformEventType::MouseMove:
    case PlatformEventType::MouseButtonDown:
    case PlatformEventType::MouseButtonUp:
    case PlatformEventType::MouseWheel:
    case PlatformEventType::MouseLeave:
    case PlatformEventType::GamepadButtonDown:
    case PlatformEventType::GamepadButtonUp:
    case PlatformEventType::GamepadAxisMotion:
        return true;
    default:
        return false;
    }
}

Rml::Input::KeyIdentifier ToRmlGamepadKey(PlatformGamepadButton button)
{
    switch (button)
    {
    case PlatformGamepadButton::South: return Rml::Input::KI_RETURN;
    case PlatformGamepadButton::East: return Rml::Input::KI_ESCAPE;
    case PlatformGamepadButton::DpadUp: return Rml::Input::KI_UP;
    case PlatformGamepadButton::DpadDown: return Rml::Input::KI_DOWN;
    case PlatformGamepadButton::DpadLeft: return Rml::Input::KI_LEFT;
    case PlatformGamepadButton::DpadRight: return Rml::Input::KI_RIGHT;
    default: return Rml::Input::KI_UNKNOWN;
    }
}


std::vector<std::filesystem::path> BuildPlatformFontCandidates()
{
    std::vector<std::filesystem::path> fonts;
#if defined(_WIN32)
    fonts.emplace_back("C:/Windows/Fonts/segoeui.ttf");
    fonts.emplace_back("C:/Windows/Fonts/NotoSansSC-VF.ttf");
    fonts.emplace_back("C:/Windows/Fonts/msyh.ttc");
    fonts.emplace_back("C:/Windows/Fonts/arial.ttf");
#elif defined(__APPLE__)
    fonts.emplace_back("/System/Library/Fonts/SFNS.ttf");
    fonts.emplace_back("/System/Library/Fonts/PingFang.ttc");
#else
    fonts.emplace_back("/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf");
    fonts.emplace_back("/usr/share/fonts/opentype/noto/NotoSansCJK-Regular.ttc");
#endif
    return fonts;
}

uint64_t GetFileFingerprint(const std::filesystem::directory_entry& entry)
{
    const uint64_t writeTime = static_cast<uint64_t>(entry.last_write_time().time_since_epoch().count());
    const uint64_t size = entry.is_regular_file() ? static_cast<uint64_t>(entry.file_size()) : 0;
    return writeTime ^ (size + 0x9e3779b97f4a7c15ull + (writeTime << 6u) + (writeTime >> 2u));
}

} // namespace

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
        lastViewModel.editorGridVisible = view.editorGridVisible;
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
    // Play/Pause 下视口点击属于蓝图运行时，不能再被编辑器拾取截走。
    const bool scriptsEditing = !lastViewModel.scripts.available ||
        lastViewModel.scripts.state == ScriptPlayState::Editing;
    if (!EditorInteraction::SceneEditingEnabled(
            scriptsEditing, developerUiVisible, lastViewModel.offlineMode))
    {
        return false;
    }
    return editorUiShell != nullptr &&
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

RuntimeResult<void> UiSubsystem::InitializeRmlUi()
{
    rmlSystemInterface.Initialize(*window, *diagnostics);
    Rml::SetSystemInterface(&rmlSystemInterface);
    // 全局渲染接口只是兜底；真正的每窗口接口在 CreateContext 时传入（RmlUi 支持每 context 接口），
    // 这正是多窗口能共用一套 RmlUi 全局状态的原因。
    Rml::SetRenderInterface(&rmlRenderInterface);
    // 进程级初始化由 RmlUiRuntime 持有（引用计数），而不是由这个"每窗口一份"的对象持有：
    // 否则第二个窗口的 Shutdown 会把 RmlUi 从第一个底下抽走。
    try
    {
        VL::Ui::RmlUiRuntime::Acquire();
    }
    catch (const std::exception& error)
    {
        return RuntimeResult<void>::Failure(MakeRuntimeError(
            "UiSubsystem.RmlInitializeFailed", error.what()));
    }
    rmlInitialized = true;

    // context 名必须唯一；窗口身份是唯一稳定的区分依据（此前硬编码 "vulkanlearn"，
    // 第二个窗口会直接撞名并拿到 nullptr）。
    rmlContext = Rml::CreateContext(
        VL::Ui::RmlUiRuntime::ContextNameFor(windowIndex),
        Rml::Vector2i(
            static_cast<int>(desc.viewportWidth),
            static_cast<int>(desc.viewportHeight)),
        &rmlRenderInterface,
        &rmlTextInputHandler);
    if (rmlContext == nullptr)
    {
        return RuntimeResult<void>::Failure(MakeRuntimeError(
            "UiSubsystem.RmlContextFailed",
            "RmlUi failed to create the runtime context."));
    }
    debugDrawLabels = std::make_unique<DebugDrawLabels>();
    if (!debugDrawLabels->Initialize(rmlContext->GetName() + "-debug-labels", rmlRenderInterface))
        return RuntimeResult<void>::Failure(MakeRuntimeError("UiSubsystem.DebugLabelsFailed", "RmlUi debug label context failed"));
    return RuntimeResult<void>::Success();
}

RuntimeResult<void> UiSubsystem::InitializeDataModel()
{
    Rml::DataModelConstructor constructor = rmlContext->CreateDataModel("game_ui");
    if (!constructor)
    {
        return RuntimeResult<void>::Failure(MakeRuntimeError(
            "UiSubsystem.DataModelFailed",
            "RmlUi failed to create the game_ui data model."));
    }

    if (!constructor.Bind("world_text", &bindingData.worldText))
    {
        return RuntimeResult<void>::Failure(MakeRuntimeError(
            "UiSubsystem.DataModelFailed", "Could not bind HUD world label."));
    }
    if (!constructor.Bind("gameplay_text", &bindingData.gameplayText))
        return RuntimeResult<void>::Failure(MakeRuntimeError(
            "UiSubsystem.DataModelFailed", "Could not bind gameplay HUD text."));

    dataModelHandle = constructor.GetModelHandle();
    return RuntimeResult<void>::Success();
}

RuntimeResult<void> UiSubsystem::LoadFonts()
{
    std::vector<std::filesystem::path> candidates = desc.fontFaces;
    std::vector<std::filesystem::path> platformCandidates = BuildPlatformFontCandidates();
    candidates.insert(candidates.end(), platformCandidates.begin(), platformCandidates.end());

    bool loadedPrimary = false;
    for (const std::filesystem::path& candidate : candidates)
    {
        std::filesystem::path fontPath = candidate;
        if (!fontPath.is_absolute())
        {
            fontPath = desc.assetRoot / fontPath;
        }
        if (!std::filesystem::exists(fontPath))
        {
            continue;
        }

        const bool fallback = loadedPrimary;
        if (Rml::LoadFontFace(fontPath.string(), fallback))
        {
            loadedPrimary = true;
        }
    }

    if (!loadedPrimary)
    {
        return RuntimeResult<void>::Failure(MakeRuntimeError(
            "UiSubsystem.FontMissing",
            "No configured or platform fallback UI font could be loaded.",
            desc.assetRoot.string()));
    }
    return RuntimeResult<void>::Success();
}

void UiSubsystem::LoadSceneOptions()
{
    bindingData.sceneOptions.clear();
    if (desc.sceneRoot.empty())
    {
        diagnostics->ReportWarning("UI scene selector disabled because sceneRoot is empty.");
        return;
    }

    std::error_code error;
    std::filesystem::recursive_directory_iterator iterator(
        desc.sceneRoot,
        std::filesystem::directory_options::skip_permission_denied,
        error);
    std::filesystem::recursive_directory_iterator end;
    if (error)
    {
        diagnostics->ReportWarning(
            "UI scene selector could not scan scene root: " + desc.sceneRoot.string());
        return;
    }

    while (iterator != end)
    {
        const std::filesystem::directory_entry& entry = *iterator;
        if (entry.is_regular_file(error) && entry.path().extension() == ".json")
        {
            try
            {
                std::ifstream stream(entry.path());
                nlohmann::json sceneJson;
                stream >> sceneJson;
                const bool isSceneAsset =
                    sceneJson.is_object() &&
                    sceneJson.value("type", std::string()) == "scene" &&
                    sceneJson.contains("objects") &&
                    sceneJson["objects"].is_array() &&
                    HasSupportedSceneObjectTypes(sceneJson);
                if (isSceneAsset)
                {
                    UiSceneOption option;
                    option.label = sceneJson.value(
                        "name",
                        entry.path().stem().string());
                    std::filesystem::path relativePath = std::filesystem::relative(
                        entry.path(),
                        desc.sceneRoot.parent_path(),
                        error);
                    if (error)
                    {
                        error.clear();
                        relativePath = std::filesystem::path("scenes") / entry.path().filename();
                    }
                    option.path = relativePath.generic_string();
                    option.normalizedPhysicalPath = NormalizePathForComparison(entry.path());
                    bindingData.sceneOptions.push_back(std::move(option));
                }
                else if (sceneJson.is_object() &&
                    sceneJson.value("type", std::string()) == "scene")
                {
                    diagnostics->ReportWarning(
                        "UI scene selector skipped an unsupported scene asset: " +
                        entry.path().string());
                }
            }
            catch (const std::exception& exception)
            {
                diagnostics->ReportWarning(
                    "UI scene selector skipped invalid JSON: " +
                    entry.path().string() + " (" + exception.what() + ")");
            }
        }
        error.clear();
        iterator.increment(error);
        if (error)
        {
            diagnostics->ReportWarning(
                "UI scene selector stopped while scanning: " + desc.sceneRoot.string());
            break;
        }
    }

    std::sort(
        bindingData.sceneOptions.begin(),
        bindingData.sceneOptions.end(),
        IsSceneOptionLess);
}

RuntimeResult<UiSubsystem::LocalizationDatabase> UiSubsystem::LoadLocalizationCandidate() const
{
    std::ifstream file(desc.localizationPath);
    if (!file.is_open())
    {
        return RuntimeResult<LocalizationDatabase>::Failure(MakeRuntimeError(
            "UiSubsystem.LocalizationOpenFailed",
            "Could not open UI localization file.",
            desc.localizationPath.string()));
    }

    try
    {
        nlohmann::json json;
        file >> json;
        if (!json.is_object())
        {
            return RuntimeResult<LocalizationDatabase>::Failure(MakeRuntimeError(
                "UiSubsystem.LocalizationInvalidRoot",
                "UI localization root must be an object.",
                desc.localizationPath.string()));
        }

        LocalizationDatabase database;
        for (const auto& localeEntry : json.items())
        {
            if (!localeEntry.value().is_object())
            {
                return RuntimeResult<LocalizationDatabase>::Failure(MakeRuntimeError(
                    "UiSubsystem.LocalizationInvalidLocale",
                    "Each UI locale must be an object.",
                    desc.localizationPath.string(),
                    localeEntry.key()));
            }

            LocalizationTable table;
            for (const auto& stringEntry : localeEntry.value().items())
            {
                if (!stringEntry.value().is_string())
                {
                    return RuntimeResult<LocalizationDatabase>::Failure(MakeRuntimeError(
                        "UiSubsystem.LocalizationInvalidString",
                        "Each UI localization value must be a string.",
                        desc.localizationPath.string(),
                        localeEntry.key() + "." + stringEntry.key()));
                }
                table.emplace(stringEntry.key(), stringEntry.value().get<std::string>());
            }
            database.emplace(localeEntry.key(), std::move(table));
        }

        if (database.find(desc.defaultLocale) == database.end())
        {
            return RuntimeResult<LocalizationDatabase>::Failure(MakeRuntimeError(
                "UiSubsystem.DefaultLocaleMissing",
                "The configured default UI locale is not present in the localization file.",
                desc.localizationPath.string(),
                desc.defaultLocale));
        }
        return RuntimeResult<LocalizationDatabase>::Success(std::move(database));
    }
    catch (const std::exception& exception)
    {
        return RuntimeResult<LocalizationDatabase>::Failure(MakeRuntimeError(
            "UiSubsystem.LocalizationParseFailed",
            exception.what(),
            desc.localizationPath.string()));
    }
}

RuntimeResult<Rml::ElementDocument*> UiSubsystem::LoadDocumentCandidate()
{
    UiBindingUpdateScope bindingUpdate(updatingRuntimeBindings);
    Rml::Factory::ClearStyleSheetCache();
    Rml::Factory::ClearTemplateCache();

    rmlSystemInterface.BeginCandidateValidation();
    Rml::ElementDocument* candidate = rmlContext->LoadDocument(desc.documentPath.generic_string());
    std::string parseError;
    const bool parseValid = rmlSystemInterface.EndCandidateValidation(parseError);
    if (candidate == nullptr || !parseValid)
    {
        if (candidate != nullptr)
        {
            rmlContext->UnloadDocument(candidate);
            rmlContext->Update();
        }
        return RuntimeResult<Rml::ElementDocument*>::Failure(MakeRuntimeError(
            "UiSubsystem.DocumentParseFailed",
            parseError.empty() ? "RmlUi returned no document." : parseError,
            desc.documentPath.string()));
    }

    std::string validationError;
    if (!ValidateDocument(*candidate, validationError))
    {
        rmlContext->UnloadDocument(candidate);
        rmlContext->Update();
        return RuntimeResult<Rml::ElementDocument*>::Failure(MakeRuntimeError(
            "UiSubsystem.DocumentValidationFailed",
            validationError,
            desc.documentPath.string()));
    }

    candidate->Hide();
    return RuntimeResult<Rml::ElementDocument*>::Success(candidate);
}

RuntimeResult<void> UiSubsystem::CommitInitialAssets()
{
    auto localizationResult = LoadLocalizationCandidate();
    if (localizationResult.IsFailure())
    {
        return RuntimeResult<void>::Failure(localizationResult.Error());
    }
    auto documentResult = LoadDocumentCandidate();
    if (documentResult.IsFailure())
    {
        return RuntimeResult<void>::Failure(documentResult.Error());
    }

    CommitDocumentCandidate(*documentResult.Value(), std::move(localizationResult.Value()));
    assetFingerprint = BuildAssetFingerprint();
    hotReloadStatus = desc.hotReload ? "hot reload: watching" : "hot reload: disabled";
    return RuntimeResult<void>::Success();
}

void UiSubsystem::CommitDocumentCandidate(
    Rml::ElementDocument& candidate,
    LocalizationDatabase localizationCandidate)
{
    UiBindingUpdateScope bindingUpdate(updatingRuntimeBindings);
    ResetRuntimeInputState();
    Rml::ElementDocument* previousDocument = runtimeDocument;
    runtimeDocument = &candidate;
    localizations = std::move(localizationCandidate);
    if (localizations.find(currentLocale) == localizations.end())
    {
        currentLocale = desc.defaultLocale;
    }

    Rml::ReleaseTextures(&rmlRenderInterface);
    runtimeDocument->Show();
    ApplyDocumentState();
    SyncBindingData();
    dataModelHandle.DirtyAllVariables();
    rmlContext->Update();

    if (previousDocument != nullptr)
    {
        rmlContext->UnloadDocument(previousDocument);
        rmlContext->Update();
    }
}

bool UiSubsystem::ValidateDocument(Rml::ElementDocument& document, std::string& outError) const
{
    static const char* requiredIds[] = {
        "ui-root",
        "hud"
    };
    for (const char* requiredId : requiredIds)
    {
        if (document.GetElementById(requiredId) == nullptr)
        {
            outError = std::string("Required UI element is missing: #") + requiredId;
            return false;
        }
    }
    outError.clear();
    return true;
}

void UiSubsystem::ApplyDocumentState()
{
    if (runtimeDocument == nullptr)
    {
        return;
    }

    // 文档 show/hide 改变可见 DOM 树：下一帧必须更新上下文并重发快照。
    uiUpdateScheduler.MarkMutation(UiMutationKind::DocumentVisibility);
    if (Rml::Element* hud = runtimeDocument->GetElementById("hud"))
        hud->SetProperty("display", developerUiVisible || immersiveView ? "none" : "block");
    if (editorUiShell != nullptr)
        editorUiShell->SetVisible(developerUiVisible);

}

void UiSubsystem::TryHotReload()
{
    if (!desc.hotReload || std::chrono::steady_clock::now() < nextHotReloadCheck)
    {
        return;
    }
    nextHotReloadCheck = std::chrono::steady_clock::now() + std::chrono::milliseconds(500);
    PROFILE_SCOPE("UI.HotReloadScan");

    AssetFingerprint candidateFingerprint = BuildAssetFingerprint();
    if (!HasAssetFingerprintChanged(candidateFingerprint))
    {
        return;
    }

    auto localizationResult = LoadLocalizationCandidate();
    if (localizationResult.IsFailure())
    {
        hotReloadStatus = "hot reload rollback: localization invalid";
        diagnostics->ReportRuntimeError("UI hot reload rejected", localizationResult.Error());
        return;
    }
    auto documentResult = LoadDocumentCandidate();
    if (documentResult.IsFailure())
    {
        hotReloadStatus = "hot reload rollback: document invalid";
        diagnostics->ReportRuntimeError("UI hot reload rejected", documentResult.Error());
        return;
    }

    if (developerUiEnabled && editorUiShell != nullptr)
    {
        auto editorResult = editorUiShell->ReloadDocument();
        if (editorResult.IsFailure())
        {
            rmlContext->UnloadDocument(documentResult.Value());
            hotReloadStatus = "hot reload rollback: editor invalid";
            diagnostics->ReportRuntimeError("UI hot reload rejected", editorResult.Error());
            return;
        }
        ResetRuntimeInputState();
    }
    CommitDocumentCandidate(*documentResult.Value(), std::move(localizationResult.Value()));
    assetFingerprint = std::move(candidateFingerprint);
    hotReloadStatus = "hot reload: committed";
    // 文档/本地化已经整份替换：下一帧按正常路径再更新一次并重发快照。
    uiUpdateScheduler.MarkMutation(UiMutationKind::HotReload);
    diagnostics->ReportInfo("UI hot reload committed: " + desc.documentPath.string());
}

UiSubsystem::AssetFingerprint UiSubsystem::BuildAssetFingerprint() const
{
    AssetFingerprint fingerprint;
    std::error_code error;
    std::filesystem::recursive_directory_iterator iterator(
        desc.assetRoot,
        std::filesystem::directory_options::skip_permission_denied,
        error);
    std::filesystem::recursive_directory_iterator end;
    while (!error && iterator != end)
    {
        const std::filesystem::directory_entry& entry = *iterator;
        if (entry.is_regular_file(error))
        {
            fingerprint.emplace(entry.path().string(), GetFileFingerprint(entry));
        }
        iterator.increment(error);
    }
    return fingerprint;
}

bool UiSubsystem::HasAssetFingerprintChanged(const AssetFingerprint& candidate) const
{
    if (candidate.size() != assetFingerprint.size())
    {
        return true;
    }
    for (const auto& entry : candidate)
    {
        auto current = assetFingerprint.find(entry.first);
        if (current == assetFingerprint.end() || current->second != entry.second)
        {
            return true;
        }
    }
    return false;
}

void UiSubsystem::SyncBindingData()
{
    // 仅用于文档/语言整体重建：逐帧路径按分节版本只刷新对应绑定。
    SyncWorldTextBinding();
    SyncGameplayTextBinding(currentBuildMode == UiViewModelBuildMode::RuntimeHud
        ? hudScripts
        : lastViewModel.scripts);
}

void UiSubsystem::BuildAndPublishRenderSnapshot(bool includeUiDocuments)
{
    PROFILE_SCOPE("UI.RenderSnapshot");
    std::shared_ptr<UiRenderSnapshot> snapshot = std::make_shared<UiRenderSnapshot>();
    // 冻结快照不能跨 GT/RT 重用；仅借用上一帧容量估计，避免每个 draw 都重新分配。
    // 隐藏帧不渲染文档，不能沿用工作区那份（可达数万顶点）的容量估计，
    // 否则空快照也会每帧预留整块工作区几何。
    if (includeUiDocuments)
    {
        snapshot->vertices.reserve(previousUiVertexCount);
        snapshot->indices.reserve(previousUiIndexCount);
        snapshot->drawCommands.reserve(previousUiDrawCount);
    }
    snapshot->frameIndex = uiFrameIndex++;
    snapshot->viewportWidth = desc.viewportWidth;
    snapshot->viewportHeight = desc.viewportHeight;
    // 隐藏帧不能保留场景视口：RT 只应看到（可选的）DrawDebug 叠层。
    snapshot->sceneViewportEnabled = includeUiDocuments && IsSceneViewportEnabled();
    snapshot->sceneViewportRect = snapshot->sceneViewportEnabled &&
        editorUiShell != nullptr
        ? ClipViewportRect(
            editorUiShell->GetSceneViewportRect(),
            desc.viewportWidth,
            desc.viewportHeight)
        : UiViewportRect{
        0,
        0,
        desc.viewportWidth,
        desc.viewportHeight};
    sceneViewportRect = snapshot->sceneViewportRect;

    // BeginFrame/EndFrame 仍要成对调用：调试标签 context 的 Render 依赖渲染接口
    // 正处于帧捕获状态。隐藏帧只是不渲染 HUD/工作区文档。
    rmlRenderInterface.BeginFrame(*snapshot);
    if (includeUiDocuments)
    {
        rmlContext->Render();
    }
    const size_t firstDebugCommand = snapshot->drawCommands.size();
    // 沉浸显示与编辑器共用同一 DrawDebug 适配；控制台仍可在开发者 UI 隐藏时
    // 打开调试图形，因此隐藏帧也要保留这个叠层。
    const bool debugOverlayVisible = IsDebugDrawOverlayVisible();
    if (debugOverlayVisible)
    {
        AppendDebugDrawOverlay(*snapshot, *lastViewModel.debugDraw, lastViewModel.motionCamera, desc.srgbFramebuffer);
        debugDrawLabels->Render(*lastViewModel.debugDraw, lastViewModel.motionCamera,
            snapshot->sceneViewportRect, desc.viewportWidth, desc.viewportHeight);
        InsertDebugDrawCommands(*snapshot, firstDebugCommand);
    }
    lastPublishedHadDebugOverlay = debugOverlayVisible &&
        !(lastViewModel.debugDraw->lines.empty() &&
            lastViewModel.debugDraw->points.empty() &&
            lastViewModel.debugDraw->texts.empty());
    if (includeUiDocuments)
    {
        rmlRenderInterface.AppendTextureSnapshots(snapshot->textures);
    }
    rmlRenderInterface.EndFrame();
    previousUiVertexCount = snapshot->vertices.size();
    previousUiIndexCount = snapshot->indices.size();
    previousUiDrawCount = snapshot->drawCommands.size();
    snapshotPublisher.Publish(std::move(snapshot));
    ++refreshStats.snapshotPublishes;
}

bool UiSubsystem::IsDebugDrawOverlayVisible() const
{
    if (!lastViewModel.debugDraw || lastViewModel.offlineMode)
    {
        return false;
    }
    // 与 BuildAndPublishRenderSnapshot 的追加条件保持同一份判定。
    if (!developerUiVisible)
    {
        return true;
    }
    return editorUiShell != nullptr && editorUiShell->IsMotionDebugViewport();
}

bool UiSubsystem::IsDebugDrawOverlayAnimating() const
{
    if (!IsDebugDrawOverlayVisible())
    {
        return false;
    }
    const Debug::Snapshot& view = *lastViewModel.debugDraw;
    if (!view.lines.empty() || !view.points.empty() || !view.texts.empty())
    {
        return true;
    }
    // 上一份快照含调试几何、这一帧已过期清空：必须重发一次空叠层。
    return lastPublishedHadDebugOverlay;
}

void UiSubsystem::UpdateInputOwnership()
{
    inputOwnership.mode = developerUiVisible ? UiInputMode::GameAndUi : UiInputMode::GameOnly;
    inputOwnership.keyboardOwner = UiInputOwner::Game;
    inputOwnership.pointerOwner = UiInputOwner::Game;
    inputOwnership.controllerOwner = UiInputOwner::Game;
    inputOwnership.textInputOwner = UiInputOwner::None;

    if (developerUiVisible && editorUiShell != nullptr)
    {
        if (editorUiShell->IsAssetEditorVisible())
            editorUiShell->SetSceneViewportInputActive(false);
        const bool sceneViewportInputActive =
            editorUiShell->IsSceneViewportInputActive();
        const auto* hit = rmlContext->GetElementAtPoint(Rml::Vector2f(pointerX, pointerY));
        const auto* focus = rmlContext->GetFocusElement();
        const bool modal = editorUiShell->HasModal();
        const bool editorFocus = editorUiShell->IsEditorDocumentElement(focus);
        const bool sceneHit = editorUiShell->IsSceneViewportElement(hit);
        const bool dragging = editorUiShell->IsWorkspaceDragging();
        if (!sceneViewportInputActive &&
            (modal || dragging || editorUiShell->IsPointerCaptured() ||
             (hit != nullptr && editorUiShell->IsEditorDocumentElement(hit) &&
              !sceneHit))
            )
            inputOwnership.pointerOwner = UiInputOwner::DeveloperUi;
        if (modal || (!sceneViewportInputActive && editorFocus))
        {
            inputOwnership.keyboardOwner = UiInputOwner::DeveloperUi;
            if (gamepadConnected) inputOwnership.controllerOwner = UiInputOwner::DeveloperUi;
        }
        if (editorFocus && dynamic_cast<const Rml::ElementFormControl*>(focus) != nullptr)
            inputOwnership.textInputOwner = UiInputOwner::DeveloperUi;
    }
}

bool UiSubsystem::PressRmlKey(Rml::Input::KeyIdentifier key, bool repeat, int modifiers)
{
    if (rmlContext == nullptr || key == Rml::Input::KI_UNKNOWN)
    {
        return true;
    }

    if (repeat)
    {
        return rmlContext->ProcessKeyDown(key, modifiers);
    }

    auto keyIt = rmlKeyPressCounts.find(key);
    if (keyIt != rmlKeyPressCounts.end())
    {
        ++keyIt->second;
        return true;
    }

    rmlKeyPressCounts.emplace(key, 1u);
    return rmlContext->ProcessKeyDown(key, modifiers);
}

bool UiSubsystem::ReleaseRmlKey(Rml::Input::KeyIdentifier key, int modifiers)
{
    if (rmlContext == nullptr || key == Rml::Input::KI_UNKNOWN)
    {
        return true;
    }

    auto keyIt = rmlKeyPressCounts.find(key);
    if (keyIt == rmlKeyPressCounts.end())
    {
        return true;
    }
    if (keyIt->second > 1u)
    {
        --keyIt->second;
        return true;
    }

    rmlKeyPressCounts.erase(keyIt);
    return rmlContext->ProcessKeyUp(key, modifiers);
}

bool UiSubsystem::SetRmlAxisDirection(
    bool pressed,
    bool& currentState,
    Rml::Input::KeyIdentifier key)
{
    if (currentState == pressed)
    {
        return true;
    }

    currentState = pressed;
    return pressed ? PressRmlKey(key, false) : ReleaseRmlKey(key);
}

bool UiSubsystem::ProcessRmlGamepadAxisEvent(const PlatformEvent& event)
{
    bool propagates = true;
    if (event.gamepadAxis == PlatformGamepadAxis::LeftX)
    {
        propagates = SetRmlAxisDirection(
            event.gamepadAxisValue <= -GamepadNavigationThreshold,
            rmlAxisLeftDown,
            Rml::Input::KI_LEFT) && propagates;
        propagates = SetRmlAxisDirection(
            event.gamepadAxisValue >= GamepadNavigationThreshold,
            rmlAxisRightDown,
            Rml::Input::KI_RIGHT) && propagates;
    }
    else if (event.gamepadAxis == PlatformGamepadAxis::LeftY)
    {
        propagates = SetRmlAxisDirection(
            event.gamepadAxisValue <= -GamepadNavigationThreshold,
            rmlAxisUpDown,
            Rml::Input::KI_UP) && propagates;
        propagates = SetRmlAxisDirection(
            event.gamepadAxisValue >= GamepadNavigationThreshold,
            rmlAxisDownDown,
            Rml::Input::KI_DOWN) && propagates;
    }
    return !propagates;
}

void UiSubsystem::ResetRuntimeInputState()
{
    if (editorUiShell != nullptr)
        editorUiShell->CancelTransientInteraction();
    ResetRmlInputState();
}

void UiSubsystem::ResetRmlInputState()
{
    if (rmlContext != nullptr)
    {
        // 释放按键/鼠标并离开：hover/pressed 状态已变化，下一帧必须更新。
        uiUpdateScheduler.MarkInputEvent();
        for (const auto& keyEntry : rmlKeyPressCounts)
        {
            rmlContext->ProcessKeyUp(keyEntry.first, 0);
        }
        for (PlatformMouseButton mouseButton : rmlPressedMouseButtons)
        {
            const int button = ToRmlMouseButton(mouseButton);
            if (button >= 0)
            {
                rmlContext->ProcessMouseButtonUp(button, 0);
            }
        }
        rmlContext->ProcessMouseLeave();
    }

    rmlTextInputHandler.CancelComposition();
    rmlKeyPressCounts.clear();
    rmlPressedKeyboardKeys.clear();
    rmlPressedMouseButtons.clear();
    rmlPressedGamepadButtons.clear();
    rmlAxisLeftDown = false;
    rmlAxisRightDown = false;
    rmlAxisUpDown = false;
    rmlAxisDownDown = false;
}

void UiSubsystem::ResetRmlGamepadInputState()
{
    for (PlatformGamepadButton gamepadButton : rmlPressedGamepadButtons)
    {
        ReleaseRmlKey(ToRmlGamepadKey(gamepadButton));
    }
    rmlPressedGamepadButtons.clear();
    SetRmlAxisDirection(false, rmlAxisLeftDown, Rml::Input::KI_LEFT);
    SetRmlAxisDirection(false, rmlAxisRightDown, Rml::Input::KI_RIGHT);
    SetRmlAxisDirection(false, rmlAxisUpDown, Rml::Input::KI_UP);
    SetRmlAxisDirection(false, rmlAxisDownDown, Rml::Input::KI_DOWN);
}

bool UiSubsystem::ProcessRuntimeUiEvent(const PlatformEvent& event)
{
    if (rmlContext == nullptr)
    {
        return false;
    }

    if (IsRmlInputEvent(event.type))
    {
        uiUpdateScheduler.MarkInputEvent();
    }

    const int modifiers = ToRmlModifiers(event);
    bool propagates = true;
    switch (event.type)
    {
    case PlatformEventType::KeyDown:
    {
        const Rml::Input::KeyIdentifier key = ToRmlKey(event.key);
        bool processKey = event.repeat;
        if (!event.repeat)
        {
            processKey = rmlPressedKeyboardKeys.insert(event.key).second;
        }
        if (processKey)
        {
            propagates = PressRmlKey(key, event.repeat, modifiers);
            if (event.key == PlatformKey::Enter && !rmlTextInputHandler.IsComposing())
            {
                propagates = rmlContext->ProcessTextInput('\n') && propagates;
            }
        }
        break;
    }
    case PlatformEventType::KeyUp:
        if (rmlPressedKeyboardKeys.erase(event.key) > 0)
        {
            propagates = ReleaseRmlKey(ToRmlKey(event.key), modifiers);
        }
        break;
    case PlatformEventType::TextInput:
        if (rmlTextInputHandler.CommitComposition(event.text))
        {
            propagates = false;
        }
        else
        {
            propagates = rmlContext->ProcessTextInput(event.text);
        }
        break;
    case PlatformEventType::TextEditing:
        propagates = !rmlTextInputHandler.UpdateComposition(
            event.text,
            event.textEditingStart,
            event.textEditingLength);
        break;
    case PlatformEventType::MouseMove:
        propagates = rmlContext->ProcessMouseMove(
            static_cast<int>(std::lround(event.mouseX)),
            static_cast<int>(std::lround(event.mouseY)),
            modifiers);
        break;
    case PlatformEventType::MouseButtonDown:
    case PlatformEventType::MouseButtonUp:
    {
        const int mouseX = static_cast<int>(std::lround(event.mouseX));
        const int mouseY = static_cast<int>(std::lround(event.mouseY));
        const bool mouseMovePropagates = rmlContext->ProcessMouseMove(mouseX, mouseY, modifiers);
        const int button = ToRmlMouseButton(event.mouseButton);
        bool buttonPropagates = true;
        if (button >= 0 && event.type == PlatformEventType::MouseButtonDown)
        {
            if (rmlPressedMouseButtons.insert(event.mouseButton).second)
            {
                buttonPropagates = rmlContext->ProcessMouseButtonDown(button, modifiers);
            }
        }
        else if (button >= 0 && rmlPressedMouseButtons.erase(event.mouseButton) > 0)
        {
            buttonPropagates = rmlContext->ProcessMouseButtonUp(button, modifiers);
        }
        propagates = mouseMovePropagates && buttonPropagates;
        break;
    }
    case PlatformEventType::MouseWheel:
    {
        const bool mouseMovePropagates = rmlContext->ProcessMouseMove(
            static_cast<int>(std::lround(event.mouseX)),
            static_cast<int>(std::lround(event.mouseY)),
            modifiers);
        const bool wheelPropagates = rmlContext->ProcessMouseWheel(
            Rml::Vector2f(-event.wheelX, -event.wheelY),
            modifiers);
        propagates = mouseMovePropagates && wheelPropagates;
        break;
    }
    case PlatformEventType::MouseLeave:
        propagates = rmlContext->ProcessMouseLeave();
        break;
    case PlatformEventType::GamepadButtonDown:
    case PlatformEventType::GamepadButtonUp:
    {
        const Rml::Input::KeyIdentifier key = ToRmlGamepadKey(event.gamepadButton);
        if (key != Rml::Input::KI_UNKNOWN && event.type == PlatformEventType::GamepadButtonDown)
        {
            if (rmlPressedGamepadButtons.insert(event.gamepadButton).second)
            {
                propagates = PressRmlKey(key, false);
            }
        }
        else if (key != Rml::Input::KI_UNKNOWN &&
            rmlPressedGamepadButtons.erase(event.gamepadButton) > 0)
        {
            propagates = ReleaseRmlKey(key);
        }
        break;
    }
    case PlatformEventType::GamepadAxisMotion:
        return ProcessRmlGamepadAxisEvent(event);
    case PlatformEventType::WindowFocusLost:
        ResetRuntimeInputState();
        break;
    case PlatformEventType::WindowFocusGained:
        break;
    default:
        break;
    }
    return !propagates;
}

void UiSubsystem::RequestOpenJsonAsset(const std::string& path)
{
    if (editorUiShell != nullptr)
        editorUiShell->RequestOpenJsonAsset(path);
}

bool UiSubsystem::CancelBlueprintAssetDrag()
{
    return editorUiShell != nullptr && editorUiShell->CancelBlueprintAssetDrag();
}

void UiSubsystem::RequestRefreshAssetCatalog()
{
    if (editorUiShell != nullptr)
        editorUiShell->RequestRefreshAssetCatalog();
}

void UiSubsystem::RequestRuntimeAction(UiAction action)
{
    // 工具条/参数控件产生的运行时请求只重脏它可能改变的节：相机参数不会
    // 顺带重建资产库/大纲/脚本快照。模式切换等事件仍在下一帧立即生效。
    RequestViewModelRefresh(SectionsForUiAction(action.type));
    if (action.type == UiActionType::LoadWorld)
    {
        // 冻结 World 快照保存物理绝对路径，目录条目使用资源相对路径。
        // 返回关卡仍须命中已知场景，再统一提交目录路径，不能因两种表示不同而静默丢弃。
        const std::string requestedPath = NormalizePathForComparison(action.stringValue);
        const bool immersiveRequest = !immersiveLevelPath.empty() &&
            action.stringValue == immersiveLevelPath;
        if (!immersiveRequest) immersiveLevelPath.clear();
        bool knownScene = false;
        for (const UiSceneOption& option : bindingData.sceneOptions)
        {
            if (option.path != action.stringValue && option.normalizedPhysicalPath != requestedPath)
                continue;
            if (immersiveRequest) immersiveLevelPath = option.normalizedPhysicalPath;
            action.stringValue = option.path;
            knownScene = true;
            break;
        }
        if (!knownScene || bindingData.sceneLoading)
        {
            immersiveLevelPath.clear();
            immersiveLoadCompleted = false;
            return;
        }
        bindingData.sceneLoading = true;
    }
    if (action.type == UiActionType::SetOfflineMode)
    {
        lastViewModel.offlineRequestedMode = action.intValue;
        lastViewModel.offlineUiNotice.clear();
    }
    QueueAction(std::move(action));
}
bool UiSubsystem::HandleGlobalShortcut(const PlatformEvent& event)
{
    if (event.type == PlatformEventType::KeyDown && !event.repeat)
    {
        if ((event.key == PlatformKey::F11 || event.key == PlatformKey::F1) && developerUiEnabled)
        {
            UiAction action;
            action.type = UiActionType::ToggleDeveloperUi;
            QueueAction(std::move(action));
            return true;
        }
    }

    return false;
}

RuntimeResult<void> UiSubsystem::SetEditorUiShell(IEditorUiShell* shell)
{
    if (editorUiShell == shell)
        return RuntimeResult<void>::Success();

    if (editorUiShell != nullptr)
        editorUiShell->Shutdown();
    editorUiShell = nullptr;
    // A replacement shell restarts its section counters at zero; never carry the
    // previous shell's applied versions across the detach/attach boundary.
    appliedEditorSectionVersions = UiSectionVersions{};

    if (shell == nullptr)
    {
        UpdateInputOwnership();
        return RuntimeResult<void>::Success();
    }
    if (rmlContext == nullptr)
    {
        return RuntimeResult<void>::Failure(MakeRuntimeError(
            "UiSubsystem.EditorShellWithoutContext",
            "Editor UI shell requires an initialized runtime UI context."));
    }

    UiEditorShellInit init;
    init.context = rmlContext;
    init.renderInterface = &rmlRenderInterface;
    init.bridge = this;
    init.diagnostics = diagnostics;
    init.assetRoot = desc.assetRoot;
    init.sceneRoot = desc.sceneRoot;
    init.previewAdapter = desc.previewAdapter;
    init.enabled = developerUiEnabled;
    init.visible = developerUiVisible;

    RuntimeResult<void> result = shell->Initialize(init);
    if (result.IsFailure())
        return result;

    editorUiShell = shell;
    editorUiShell->Resize(desc.viewportWidth, desc.viewportHeight);
    UpdateInputOwnership();
    return RuntimeResult<void>::Success();
}

bool UiSubsystem::ProcessRmlEvent(const PlatformEvent& event)
{
    return ProcessRuntimeUiEvent(event);
}

void UiSubsystem::MarkPanelBuildMutation()
{
    uiUpdateScheduler.MarkMutation(UiMutationKind::PanelBuild);
}

void UiSubsystem::BeginDocumentValidation()
{
    rmlSystemInterface.BeginCandidateValidation();
}

bool UiSubsystem::EndDocumentValidation(std::string& outError)
{
    return rmlSystemInterface.EndCandidateValidation(outError);
}

void UiSubsystem::RecordSectionPublish(UiSectionId id)
{
    ++refreshStats.sectionPublishes;
    ++refreshStats.sectionPublishesById[static_cast<size_t>(id)];
}

void UiSubsystem::RecordPanelBuild()
{
    ++refreshStats.panelBuilds;
}

void UiSubsystem::RecordRuntimeViewUpdate()
{
    ++refreshStats.runtimeViewUpdates;
}

void UiSubsystem::ClearEditorUiShell(IEditorUiShell* shell)
{
    if (editorUiShell != shell)
        return;
    editorUiShell = nullptr;
    appliedEditorSectionVersions = UiSectionVersions{};
    UpdateInputOwnership();
}

void UiSubsystem::QueueAction(UiAction action)
{
    if (commandBus != nullptr)
    {
        commandBus->Queue(std::move(action));
    }
}

} // namespace VL
