#include "ui/uiSubsystem.h"
#include "ui/debugDrawOverlay.h"
#include "ui/gameplayHudView.h"
#include "ui/editorPointerPolicy.h"

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
#include "editor/selection/materialInstanceSelection.h"
#include "editor/runtime/materialInstanceEditorRuntime.h"
#include "editor/assets/editorJsonAssetLibrary.h"
#include "editor/assets/editorThumbnailLibrary.h"
#include "editor/ui/materialInstanceAssetEditorPanel.h"
#include "editor/ui/editorSceneActorTree.h"
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

UiSubsystem::UiSubsystem() = default;

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

    RuntimeResult<void> developerResult = InitializeDeveloperUi();
    if (developerResult.IsFailure())
    {
        Shutdown();
        return developerResult;
    }

    RuntimeResult<void> materialEditorResult =
        InitializeMaterialInstanceEditor();
    if (materialEditorResult.IsFailure())
    {
        diagnostics->ReportWarning(
            FormatRuntimeError(materialEditorResult.Error()));
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
    debugDrawLabels.reset();
    renderSnapshotQueue.Clear();
    if (rmlContext != nullptr)
    {
        ResetRuntimeInputState();
    }


    materialInstanceEditorPanel.reset();
    jsonAssetLibrary.reset();
    thumbnailLibrary.reset();
    materialInstanceEditorRuntime.reset();

    if (rmlContext != nullptr && editorDocument != nullptr)
    {
        rmlContext->UnloadDocument(editorDocument);
        editorDocument = nullptr;
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
    viewModelRefreshRequested = false;
    editorPointerCaptured = false;
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

    if (HandleGlobalShortcut(event))
    {
        return true;
    }

    bool consumed = false;
    if (!consumed && developerUiVisible)
    {
        consumed = ProcessDeveloperUiEvent(event);
    }

    UpdateInputOwnership();
    return consumed;
}

void UiSubsystem::Update(const UiViewModelSnapshot& viewModelSnapshot,
    UiViewModelBuildMode mode,
    bool editorSnapshotDue)
{
    PROFILE_SCOPE("UI.Update");
    if (!initialized)
    {
        return;
    }

    if (mode == UiViewModelBuildMode::None)
    {
        // 编辑器与 HUD 都不可见（沉浸模式）：只冻结 DrawDebug 叠层需要的最小字段，
        // 保留 lastViewModel 的编辑器字段以免 TickMaterialInstanceEditor 丢失蓝图类，
        // 并发布仅调试图元的快照覆盖 RT 保留的上一帧工作区画面。
        SetImmersiveDocumentStateIfRestored(viewModelSnapshot);
        lastViewModel.frameIndex = viewModelSnapshot.frameIndex;
        lastViewModel.deltaTimeSeconds = viewModelSnapshot.deltaTimeSeconds;
        lastViewModel.framesPerSecond = viewModelSnapshot.framesPerSecond;
        lastViewModel.offlineMode = viewModelSnapshot.offlineMode;
        lastViewModel.motionWorldGeneration = viewModelSnapshot.motionWorldGeneration;
        lastViewModel.motionDebug = viewModelSnapshot.motionDebug;
        lastViewModel.motionCamera = viewModelSnapshot.motionCamera;
        lastViewModel.debugDraw = viewModelSnapshot.debugDraw;
        lastViewModel.activeWorldPath = viewModelSnapshot.activeWorldPath;
        // 隐藏期间的脏事件不需要排队：重新可见的第一帧由可见性变化强制完整刷新。
        viewModelRefreshRequested = false;
        BuildAndPublishRenderSnapshot(false);
        UpdateInputOwnership();
        return;
    }

    UiViewModelSnapshot previous = std::move(lastViewModel);
    ScriptRuntimeView carriedScripts = std::move(previous.scripts);
    lastViewModel = viewModelSnapshot;
    if (mode == UiViewModelBuildMode::FullEditor && !editorSnapshotDue)
    {
        // 非刷新帧不重建脚本视图模型：保留上一份类/反馈/性能值，避免每帧复制 JSON。
        // HUD 文本在编辑器可见时本来就隐藏，因此这里没有可感知的文本延迟。
        lastViewModel.scripts = std::move(carriedScripts);
    }
    else if (mode == UiViewModelBuildMode::RuntimeHud)
    {
        // HUD 轻量视图只带 hudLines。编辑器仍可能在后台存在（配置隐藏但已启用），
        // TickMaterialInstanceEditor 依赖上一份蓝图类，不能被轻量视图清空。
        lastViewModel.scripts.classes = std::move(carriedScripts.classes);
        lastViewModel.scripts.componentClasses = std::move(carriedScripts.componentClasses);
        lastViewModel.scripts.performance = std::move(carriedScripts.performance);
        lastViewModel.scripts.feedback = std::move(carriedScripts.feedback);
        lastViewModel.scripts.buildAvailable = carriedScripts.buildAvailable;
        lastViewModel.scripts.buildBusy = carriedScripts.buildBusy;
        lastViewModel.scripts.autoCompile = carriedScripts.autoCompile;
        lastViewModel.scripts.buildOutput = std::move(carriedScripts.buildOutput);
        lastViewModel.scripts.canUndo = carriedScripts.canUndo;
        lastViewModel.scripts.canRedo = carriedScripts.canRedo;
    }
    SetImmersiveDocumentStateIfRestored(lastViewModel);
    ReconcileOfflineModeFeedback(lastViewModel, previous);
    lastViewModel.developerUiVisible = developerUiVisible;
    lastViewModel.locale = currentLocale;

    UiBindingUpdateScope bindingUpdate(updatingRuntimeBindings);
    TryHotReload();
    SyncBindingData();
    if (mode == UiViewModelBuildMode::FullEditor && editorSnapshotDue)
    {
        BuildDeveloperPanels();
    }
    {
        PROFILE_SCOPE("UI.RmlUpdate");
        rmlContext->Update();
    }
    // 逐帧发布绘制快照：控件自身的 hover/滚动/拖动仍由 RmlUi 每帧更新，
    // 只有面板数据（BuildDeveloperPanels）按节拍重建。
    BuildAndPublishRenderSnapshot(true);
    UpdateInputOwnership();
}

void UiSubsystem::SetImmersiveDocumentStateIfRestored(
    const UiViewModelSnapshot& viewModelSnapshot)
{
    if (immersiveLevelPath.empty() || !immersiveLoadCompleted)
    {
        return;
    }
    // 完成通知本身不代表成功；必须核对事务发布后的 World，禁止放大仍存活的模型预览。
    const bool restored = NormalizePathForComparison(viewModelSnapshot.activeWorldPath) ==
        NormalizePathForComparison(immersiveLevelPath);
    immersiveLevelPath.clear();
    immersiveLoadCompleted = false;
    if (restored) SetImmersiveView(true);
    else diagnostics->ReportWarning("Immersive mode cancelled: the level was not restored.");
}

bool UiSubsystem::ConsumeViewModelRefreshRequest() noexcept
{
    const bool requested = viewModelRefreshRequested;
    viewModelRefreshRequested = false;
    return requested;
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
    if (rmlContext != nullptr)
    {
        rmlContext->SetDimensions(Rml::Vector2i(static_cast<int>(width), static_cast<int>(height)));
    }
}

bool UiSubsystem::IsScenePickTarget(float mouseX, float mouseY) const
{
    if (!developerUiVisible || editorDocument == nullptr ||
        !std::isfinite(mouseX) || !std::isfinite(mouseY) ||
        mouseX < 0 || mouseY < 0 || mouseX >= desc.viewportWidth || mouseY >= desc.viewportHeight ||
        materialInstanceEditorPanel == nullptr || materialInstanceEditorPanel->HasModal() ||
        materialInstanceEditorPanel->IsWorkspaceDragging() ||
        !ContainsViewportPoint(sceneViewportRect, mouseX, mouseY))
    {
        return false;
    }
    const Rml::Element* hit = rmlContext->GetElementAtPoint(Rml::Vector2f(mouseX, mouseY));
    return materialInstanceEditorPanel->IsSceneViewportElement(hit);
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
            else if (materialInstanceEditorPanel != nullptr &&
                materialInstanceEditorPanel->PrepareImmersiveView(immersiveLevelPath))
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
    editorPointerCaptured = false;
    ApplyDocumentState();
}

void UiSubsystem::NotifyWorldChanged(
    const std::string& worldPath,
    uint64_t generation)
{
    RequestViewModelRefresh();
    selectedModelMaterials.reset();
    selectedMaterialSlotIndex.reset();
    sceneModels.clear();
    // 大纲行名可能在新 World 里重名，身份不做跨代复用。
    sceneActors.reset();
    selectedActorIdentity.clear();
    pendingSceneSelectionClear = false;
    pendingMaterialInstanceModelSelection.reset();
    pendingMaterialInstanceSelection.reset();
    if (materialInstanceEditorRuntime != nullptr)
    {
        materialInstanceEditorRuntime->NotifyWorldChanged(
            worldPath,
            generation);
    }
}

void UiSubsystem::NotifyWorldLoadCompleted()
{
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
    if (!initialized || materialInstanceEditorRuntime == nullptr)
    {
        return;
    }
    materialInstanceEditorRuntime->SetBlueprintClasses(lastViewModel.scripts.classes);
    materialInstanceEditorRuntime->SetBlueprintComponentClasses(lastViewModel.scripts.componentClasses);
    // 命令执行会改变草稿/历史/预览与命令回馈，下一帧强制重建面板数据。
    if (materialInstanceEditorRuntime->Tick())
    {
        RequestViewModelRefresh();
    }
    if (jsonAssetLibrary != nullptr) jsonAssetLibrary->Poll();
}

bool UiSubsystem::SelectSceneObjectFromPick(
    const Editor::Selection::MaterialInstanceSelection& selection)
{
    if (!developerUiEnabled || materialInstanceEditorRuntime == nullptr)
    {
        return false;
    }

    // 选中态是用户直接反馈：下一帧强制刷新，不能落后于 10 Hz 节拍。
    RequestViewModelRefresh();
    // 关卡拾取只发布对象/槽位选择，不创建 MI 文档或请求预览 World。
    // 打开材质必须来自材质槽或资产浏览器的显式操作。
    if (materialInstanceEditorPanel != nullptr) materialInstanceEditorPanel->ShowSceneDetails();
    pendingMaterialInstanceSelection.reset();
    pendingMaterialInstanceModelSelection.reset();

    EditorUi::MaterialInstanceModelSnapshot modelSnapshot;
    modelSnapshot.worldGeneration = selection.modelContext.worldGeneration;
    modelSnapshot.scenePath = selection.modelContext.scenePath;
    modelSnapshot.objectId = selection.modelContext.objectId;
    modelSnapshot.displayName = selection.modelContext.displayName;
    modelSnapshot.objectIdentity = selection.modelContext.objectIdentity;
    modelSnapshot.materials.reserve(selection.modelContext.materials.size());
    for (const Editor::Selection::MaterialInstanceModelMaterial& material :
         selection.modelContext.materials)
    {
        EditorUi::MaterialInstanceModelMaterialSnapshot materialSnapshot;
        materialSnapshot.objectId = material.objectId;
        materialSnapshot.materialSlotIndex = material.materialSlotIndex;
        materialSnapshot.materialSlotName = material.materialSlotName;
        materialSnapshot.materialInstancePath = material.materialInstancePath;
        modelSnapshot.materials.push_back(
            std::move(materialSnapshot));
    }
    selectedModelMaterials = std::move(modelSnapshot);
    selectedMaterialSlotIndex = selection.materialSlotIndex;
    // 视口拾取与大纲行共用同一份选中态：拾取到的网格在大纲里也要高亮。
    selectedActorIdentity = EditorUi::SceneActorIdentity(
        EditorUi::EditorSceneActorType::Mesh, selection.objectIdentity);
    if (selection.objectIdentity.rfind("__blueprint/", 0) == 0)
    {
        const auto id = selection.objectIdentity.substr(std::string_view("__blueprint/").size());
        selectedActorIdentity = "blueprintInstance|" + id.substr(0, id.find('/'));
    }
    pendingSceneSelectionClear = false;
    return true;
}

void UiSubsystem::SetMaterialInstanceSceneModels(
    const std::vector<Editor::Selection::MaterialInstanceModelContext>& models)
{
    sceneModels.clear();
    sceneModels.reserve(models.size());
    for (const Editor::Selection::MaterialInstanceModelContext& model : models)
    {
        EditorUi::MaterialInstanceModelSnapshot modelSnapshot;
        modelSnapshot.worldGeneration = model.worldGeneration;
        modelSnapshot.scenePath = model.scenePath;
        modelSnapshot.objectId = model.objectId;
        modelSnapshot.displayName = model.displayName;
        modelSnapshot.objectIdentity = model.objectIdentity;
        modelSnapshot.materials.reserve(model.materials.size());
        for (const Editor::Selection::MaterialInstanceModelMaterial& material :
             model.materials)
        {
            EditorUi::MaterialInstanceModelMaterialSnapshot materialSnapshot;
            materialSnapshot.objectId = material.objectId;
            materialSnapshot.materialSlotIndex = material.materialSlotIndex;
            materialSnapshot.materialSlotName = material.materialSlotName;
            materialSnapshot.materialInstancePath = material.materialInstancePath;
            modelSnapshot.materials.push_back(std::move(materialSnapshot));
        }
        sceneModels.push_back(std::move(modelSnapshot));
    }

    if (!selectedModelMaterials.has_value())
    {
        return;
    }

    const auto selected = std::find_if(
        sceneModels.begin(),
        sceneModels.end(),
        [this](const EditorUi::MaterialInstanceModelSnapshot& model)
        {
            return selectedModelMaterials->objectId != 0 &&
                model.objectId == selectedModelMaterials->objectId &&
                model.worldGeneration == selectedModelMaterials->worldGeneration;
        });
    if (selected == sceneModels.end())
    {
        selectedModelMaterials.reset();
        selectedMaterialSlotIndex.reset();
        return;
    }
    selectedModelMaterials = *selected;
}

// 大纲快照逐帧替换：只保存不可变数组的共享指针，选择态按稳定身份保留，
// 身份消失时清空，防止把旧 World 的选择静默套到同名新对象上。
void UiSubsystem::SetSceneActors(std::vector<EditorUi::EditorSceneActorSnapshot> actors)
{
    sceneActors = std::make_shared<const std::vector<EditorUi::EditorSceneActorSnapshot>>(
        std::move(actors));
    if (selectedActorIdentity.empty()) return;

    const auto& list = *sceneActors;
    const bool stillPresent = std::any_of(
        list.begin(),
        list.end(),
        [this](const EditorUi::EditorSceneActorSnapshot& actor)
        {
            return actor.identity == selectedActorIdentity;
        });
    if (stillPresent) return;
    selectedActorIdentity.clear();
    selectedModelMaterials.reset();
    selectedMaterialSlotIndex.reset();
}

void UiSubsystem::SetPerformanceSnapshot(PerformanceSnapshot snapshot)
{
    // 值语义复制：渲染线程之后更新计时器不会改写 UI 已冻结的这一帧。
    performanceSnapshot = std::move(snapshot);
}

std::optional<Editor::Selection::MaterialInstanceModelContext>
UiSubsystem::ConsumeMaterialInstanceModelSelectionRequest()
{
    std::optional<Editor::Selection::MaterialInstanceModelContext> result =
        std::move(pendingMaterialInstanceModelSelection);
    pendingMaterialInstanceModelSelection.reset();
    return result;
}

std::optional<Editor::Selection::MaterialInstanceSelection>
UiSubsystem::ConsumeMaterialInstanceSelectionRequest()
{
    std::optional<Editor::Selection::MaterialInstanceSelection> result =
        std::move(pendingMaterialInstanceSelection);
    pendingMaterialInstanceSelection.reset();
    return result;
}

bool UiSubsystem::ConsumeSceneSelectionClearRequest()
{
    // 与 selection 请求同帧消费一次：EngineLoop 据此清掉渲染端描边。
    const bool pending = pendingSceneSelectionClear;
    pendingSceneSelectionClear = false;
    return pending;
}

void UiSubsystem::ClearMaterialInstanceSceneSelection() noexcept
{
    RequestViewModelRefresh();
    selectedModelMaterials.reset();
    selectedMaterialSlotIndex.reset();
    selectedActorIdentity.clear();
    pendingSceneSelectionClear = false;
    pendingMaterialInstanceModelSelection.reset();
    pendingMaterialInstanceSelection.reset();
}

bool UiSubsystem::RequestModelSelection(
    const Editor::Selection::MaterialInstanceModelContext& model)
{
    if (!developerUiEnabled || model.worldGeneration == 0 ||
        model.objectId == 0 || model.objectIdentity.empty())
    {
        return false;
    }

    RequestViewModelRefresh();
    EditorUi::MaterialInstanceModelSnapshot modelSnapshot;
    modelSnapshot.worldGeneration = model.worldGeneration;
    modelSnapshot.scenePath = model.scenePath;
    modelSnapshot.objectId = model.objectId;
    modelSnapshot.displayName = model.displayName;
    modelSnapshot.objectIdentity = model.objectIdentity;
    modelSnapshot.materials.reserve(model.materials.size());
    for (const Editor::Selection::MaterialInstanceModelMaterial& material : model.materials)
    {
        EditorUi::MaterialInstanceModelMaterialSnapshot materialSnapshot;
        materialSnapshot.objectId = material.objectId;
        materialSnapshot.materialSlotIndex = material.materialSlotIndex;
        materialSnapshot.materialSlotName = material.materialSlotName;
        materialSnapshot.materialInstancePath = material.materialInstancePath;
        modelSnapshot.materials.push_back(std::move(materialSnapshot));
    }
    selectedModelMaterials = std::move(modelSnapshot);
    selectedMaterialSlotIndex.reset();
    pendingMaterialInstanceSelection.reset();
    pendingMaterialInstanceModelSelection = model;
    selectedActorIdentity = EditorUi::SceneActorIdentity(
        EditorUi::EditorSceneActorType::Mesh, model.objectIdentity);
    pendingSceneSelectionClear = false;
    return true;
}

bool UiSubsystem::RequestMaterialSelection(
    const Editor::Selection::MaterialInstanceSelection& selection)
{
    if (!developerUiEnabled || selection.worldGeneration == 0 ||
        selection.objectId == 0 || selection.objectIdentity.empty() ||
        selection.materialInstancePath.empty())
    {
        return false;
    }

    RequestViewModelRefresh();
    selectedMaterialSlotIndex = selection.materialSlotIndex;
    pendingMaterialInstanceModelSelection.reset();
    pendingMaterialInstanceSelection = selection;
    selectedActorIdentity = EditorUi::SceneActorIdentity(
        EditorUi::EditorSceneActorType::Mesh, selection.objectIdentity);
    pendingSceneSelectionClear = false;
    return true;
}

bool UiSubsystem::RequestActorSelection(const std::string& actorIdentity)
{
    if (!developerUiEnabled || actorIdentity.empty())
    {
        return false;
    }

    RequestViewModelRefresh();
    // 非网格 actor 没有 MI 文档：清掉编辑器侧的材质槽选择，并要求 EngineLoop
    // 清掉渲染端的 mesh 描边，让大纲高亮与视口描边保持同一个选中对象。
    selectedModelMaterials.reset();
    selectedMaterialSlotIndex.reset();
    pendingMaterialInstanceModelSelection.reset();
    pendingMaterialInstanceSelection.reset();
    selectedActorIdentity = actorIdentity;
    pendingSceneSelectionClear = true;
    return true;
}

bool UiSubsystem::ShouldUseRelativeMouseModeForGame() const
{
    if (!initialized) return false;
    if (materialInstanceEditorPanel != nullptr &&
        materialInstanceEditorPanel->IsAssetEditorVisible())
        return false;
    // 捕获由 Viewport 点击建立，不能再用绝对光标位置反向取消；
    // 相对鼠标移动期间 SDL 的绝对坐标可能落在 Viewport 外。
    return !developerUiVisible || sceneViewportInputActive;
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

RuntimeResult<void> UiSubsystem::InitializeDeveloperUi()
{
    if (!developerUiEnabled) return RuntimeResult<void>::Success();
    auto candidate = LoadEditorDocumentCandidate();
    if (candidate.IsFailure()) return RuntimeResult<void>::Failure(candidate.Error());
    editorDocument = candidate.Value();
    ApplyDocumentState();
    return RuntimeResult<void>::Success();
}

RuntimeResult<void> UiSubsystem::InitializeMaterialInstanceEditor()
{
    materialInstanceEditorPanel.reset();
    materialInstanceEditorRuntime.reset();
    jsonAssetLibrary.reset();
    thumbnailLibrary.reset();

    if (!developerUiEnabled)
    {
        return RuntimeResult<void>::Success();
    }

    std::filesystem::path resourceRoot;
    if (!desc.sceneRoot.empty())
    {
        resourceRoot = desc.sceneRoot.parent_path();
    }

    std::filesystem::path projectRoot;
    if (!desc.assetRoot.empty())
    {
        projectRoot = desc.assetRoot.parent_path();
    }

    auto runtime = std::make_unique<Editor::MaterialInstanceEditorRuntime>();
    RuntimeResult<void> result = runtime->Initialize(
        Editor::MaterialInstanceEditorRuntime::Config{
            std::move(resourceRoot),
            std::move(projectRoot),
            256,
            desc.previewAdapter});
    if (result.IsFailure())
    {
        return result;
    }

    materialInstanceEditorRuntime = std::move(runtime);
    materialInstanceEditorPanel =
        std::make_unique<EditorUi::MaterialInstanceAssetEditorPanel>(
            materialInstanceEditorRuntime.get());
    materialInstanceEditorPanel->SetSceneSelectionSink(this);
    materialInstanceEditorPanel->SetRuntimeActionSink(this);
    std::string error;
    jsonAssetLibrary = std::make_unique<Editor::EditorJsonAssetLibrary>();
    if (!jsonAssetLibrary->Initialize(desc.sceneRoot.parent_path(), error))
        return RuntimeResult<void>::Failure(MakeRuntimeError("UiSubsystem.AssetLibraryInvalid", error));
    thumbnailLibrary = std::make_unique<Editor::EditorThumbnailLibrary>();
    if (!thumbnailLibrary->Initialize(desc.sceneRoot.parent_path(),
        desc.assetRoot.parent_path() / ".cache" / "editor-thumbnails", error))
        return RuntimeResult<void>::Failure(MakeRuntimeError("UiSubsystem.ThumbnailLibraryInvalid", error));
    materialInstanceEditorPanel->SetJsonAssetRequestSink(this);
    if (!materialInstanceEditorPanel->Attach(*editorDocument, error))
        return RuntimeResult<void>::Failure(MakeRuntimeError("UiSubsystem.EditorDocumentInvalid", error));
    return RuntimeResult<void>::Success();
}

RuntimeResult<Rml::ElementDocument*> UiSubsystem::LoadEditorDocumentCandidate()
{
    const auto path = desc.assetRoot / "editor-workspace.rml";
    rmlSystemInterface.BeginCandidateValidation();
    // RmlUi 的 URL 基路径解析要求正斜杠；原生 Windows 路径会丢失 img 的相对目录。
    Rml::ElementDocument* candidate = rmlContext->LoadDocument(path.generic_string());
    std::string error;
    const bool parsed = rmlSystemInterface.EndCandidateValidation(error);
    const bool valid = candidate != nullptr && parsed &&
        EditorUi::MaterialInstanceAssetEditorPanel::ValidateDocument(*candidate, error);
    if (!valid)
    {
        if (candidate != nullptr) rmlContext->UnloadDocument(candidate);
        return RuntimeResult<Rml::ElementDocument*>::Failure(MakeRuntimeError(
            "UiSubsystem.EditorDocumentInvalid",
            error.empty() ? "Editor workspace could not be loaded." : error, path.string()));
    }
    return RuntimeResult<Rml::ElementDocument*>::Success(candidate);
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

    if (Rml::Element* hud = runtimeDocument->GetElementById("hud"))
        hud->SetProperty("display", developerUiVisible || immersiveView ? "none" : "block");
    if (editorDocument != nullptr)
    {
        if (developerUiVisible)
        {
            editorDocument->Show(Rml::ModalFlag::None, Rml::FocusFlag::None);
            // Hide 会将焦点交还运行时文档并将其置顶；无焦点 Show 不恢复层级。
            // 显式置顶才能避免透明运行时根节点挡住鼠标，同时不抢走游戏键盘焦点。
            editorDocument->PullToFront();
        }
        else editorDocument->Hide();
    }

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

    Rml::ElementDocument* editorCandidate = nullptr;
    if (developerUiEnabled)
    {
        auto editorResult = LoadEditorDocumentCandidate();
        if (editorResult.IsFailure())
        {
            rmlContext->UnloadDocument(documentResult.Value());
            hotReloadStatus = "hot reload rollback: editor invalid";
            diagnostics->ReportRuntimeError("UI hot reload rejected", editorResult.Error());
            return;
        }
        editorCandidate = editorResult.Value();
    }
    // 两个文档都通过解析与结构校验才替换；失败候选不改变最后有效工作区。
    if (editorCandidate != nullptr)
    {
        ResetRuntimeInputState();
        if (materialInstanceEditorPanel != nullptr) materialInstanceEditorPanel->Detach();
        rmlContext->UnloadDocument(editorDocument);
        editorDocument = editorCandidate;
        std::string error;
        if (materialInstanceEditorPanel != nullptr)
            materialInstanceEditorPanel->Attach(*editorDocument, error);
    }
    CommitDocumentCandidate(*documentResult.Value(), std::move(localizationResult.Value()));
    assetFingerprint = std::move(candidateFingerprint);
    hotReloadStatus = "hot reload: committed";
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
    const std::string worldText = std::filesystem::path(lastViewModel.activeWorldPath).stem().string();
    if (bindingData.worldText != worldText)
    {
        bindingData.worldText = worldText;
        dataModelHandle.DirtyVariable("world_text");
    }
    // GT 只消费冻结文本；RmlUi 插值负责转义，RT 仍只接收既有几何快照。
    const std::string gameplayText = BuildGameplayHudText(lastViewModel.scripts);
    if (bindingData.gameplayText != gameplayText)
    {
        bindingData.gameplayText = gameplayText;
        dataModelHandle.DirtyVariable("gameplay_text");
    }
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
    snapshot->sceneViewportRect = snapshot->sceneViewportEnabled && materialInstanceEditorPanel != nullptr
        ? ClipViewportRect(materialInstanceEditorPanel->GetSceneViewportRect(), desc.viewportWidth, desc.viewportHeight)
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
    if (lastViewModel.debugDraw && !lastViewModel.offlineMode &&
        (!developerUiVisible || (materialInstanceEditorPanel &&
            materialInstanceEditorPanel->IsMotionDebugViewport())))
    {
        AppendDebugDrawOverlay(*snapshot, *lastViewModel.debugDraw, lastViewModel.motionCamera, desc.srgbFramebuffer);
        debugDrawLabels->Render(*lastViewModel.debugDraw, lastViewModel.motionCamera,
            snapshot->sceneViewportRect, desc.viewportWidth, desc.viewportHeight);
        InsertDebugDrawCommands(*snapshot, firstDebugCommand);
    }
    if (includeUiDocuments)
    {
        rmlRenderInterface.AppendTextureSnapshots(snapshot->textures);
    }
    rmlRenderInterface.EndFrame();
    previousUiVertexCount = snapshot->vertices.size();
    previousUiIndexCount = snapshot->indices.size();
    previousUiDrawCount = snapshot->drawCommands.size();
    renderSnapshotQueue.Publish(std::move(snapshot));
}

void UiSubsystem::BuildDeveloperPanels()
{
    PROFILE_SCOPE("UI.BuildPanels");
    if (materialInstanceEditorPanel == nullptr || materialInstanceEditorRuntime == nullptr) return;
    EditorUi::MaterialInstanceEditorSnapshot snapshot = materialInstanceEditorRuntime->GetSnapshot();
    snapshot.logs = ProcessLog().Freeze();
    if (jsonAssetLibrary != nullptr)
    {
        snapshot.assetCatalog = jsonAssetLibrary->GetCatalog();
        snapshot.jsonDocument = jsonAssetLibrary->GetDocument();
        snapshot.assetCatalogLoading = jsonAssetLibrary->IsCatalogLoading();
        snapshot.jsonDocumentLoading = jsonAssetLibrary->IsDocumentLoading();
    }
    snapshot.sceneModels = sceneModels;
    if (thumbnailLibrary != nullptr)
    {
        thumbnailLibrary->SetPaused(!developerUiVisible || sceneViewportInputActive || editorPointerCaptured);
        thumbnailLibrary->Poll();
        snapshot.thumbnails = thumbnailLibrary->GetSnapshot();
    }
    rmlRenderInterface.PublishThumbnails(snapshot.thumbnails);
    rmlRenderInterface.PublishTexturePreview(snapshot.jsonDocument);
    snapshot.sceneActors = sceneActors;
    snapshot.selectedActorIdentity = selectedActorIdentity;
    snapshot.selectedModel = selectedModelMaterials;
    snapshot.selectedMaterialSlotIndex = selectedMaterialSlotIndex;
    snapshot.performance = performanceSnapshot;
    materialInstanceEditorPanel->Build(snapshot);
    materialInstanceEditorPanel->UpdateRuntimeView(lastViewModel);
    if (thumbnailLibrary != nullptr)
        thumbnailLibrary->RequestVisible(developerUiVisible ?
            materialInstanceEditorPanel->VisibleThumbnailPaths() : std::vector<std::string>{});
}

Editor::SceneDocumentService* UiSubsystem::GetSceneDocuments() const
{
    return materialInstanceEditorRuntime ? materialInstanceEditorRuntime->SceneDocuments() : nullptr;
}

void UiSubsystem::UpdateInputOwnership()
{
    inputOwnership.mode = developerUiVisible ? UiInputMode::GameAndUi : UiInputMode::GameOnly;
    inputOwnership.keyboardOwner = UiInputOwner::Game;
    inputOwnership.pointerOwner = UiInputOwner::Game;
    inputOwnership.controllerOwner = UiInputOwner::Game;
    inputOwnership.textInputOwner = UiInputOwner::None;

    if (developerUiVisible && editorDocument != nullptr)
    {
        const bool resourceEditor = materialInstanceEditorPanel != nullptr &&
            materialInstanceEditorPanel->IsAssetEditorVisible();
        if (resourceEditor) sceneViewportInputActive = false;
        const auto* hit = rmlContext->GetElementAtPoint(Rml::Vector2f(pointerX, pointerY));
        const auto* focus = rmlContext->GetFocusElement();
        const bool modal = materialInstanceEditorPanel != nullptr && materialInstanceEditorPanel->HasModal();
        const bool editorFocus = focus != nullptr && focus->GetOwnerDocument() == editorDocument;
        const bool sceneHit = materialInstanceEditorPanel != nullptr &&
            materialInstanceEditorPanel->IsSceneViewportElement(hit);
        const bool dragging = materialInstanceEditorPanel != nullptr &&
            materialInstanceEditorPanel->IsWorkspaceDragging();
        if (!sceneViewportInputActive && (modal || dragging || editorPointerCaptured ||
            (hit != nullptr && hit->GetOwnerDocument() == editorDocument && !sceneHit))
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
    if (materialInstanceEditorPanel != nullptr) materialInstanceEditorPanel->CancelWorkspaceDrag();
    if (rmlContext != nullptr)
    {
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
    editorPointerCaptured = false;
    sceneViewportInputActive = false;
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

bool UiSubsystem::ProcessDeveloperUiEvent(const PlatformEvent& event)
{
    if (editorDocument == nullptr) return false;
    const bool modal = materialInstanceEditorPanel != nullptr && materialInstanceEditorPanel->HasModal();
    const bool resourceEditor = materialInstanceEditorPanel != nullptr &&
        materialInstanceEditorPanel->IsAssetEditorVisible();
    if (resourceEditor) sceneViewportInputActive = false;
    const Rml::Element* focus = rmlContext->GetFocusElement();
    bool editorFocus = focus != nullptr && focus->GetOwnerDocument() == editorDocument;
    switch (event.type)
    {
    case PlatformEventType::MouseMove:
    case PlatformEventType::MouseButtonDown:
    case PlatformEventType::MouseButtonUp:
    case PlatformEventType::MouseWheel:
    {
        pointerX = event.mouseX;
        pointerY = event.mouseY;
        const auto* hit = rmlContext->GetElementAtPoint(Rml::Vector2f(pointerX, pointerY));
        const bool sceneHit = materialInstanceEditorPanel != nullptr &&
            materialInstanceEditorPanel->IsSceneViewportElement(hit);
        const bool editorHit = hit != nullptr && hit->GetOwnerDocument() == editorDocument && !sceneHit;
        if (ShouldReleaseScenePointerForEditor(event.type, editorHit))
            sceneViewportInputActive = false;
        const bool captured = editorPointerCaptured ||
            (materialInstanceEditorPanel != nullptr && materialInstanceEditorPanel->IsWorkspaceDragging());
        if ((sceneHit || (sceneViewportInputActive && !resourceEditor)) && !captured && !modal)
        {
            // 右键在运行时输入里没有任何绑定：相机看的是相对鼠标增量，拾取只认左键。
            // 视口命中的右键因此交给编辑器文档，右键上下文菜单才有 RmlUi 事件与坐标；
            // 返回 true 表示这次右键已被编辑器消费，同一事件不再走场景输入。
            if (event.mouseButton == PlatformMouseButton::Right)
            {
                ProcessRuntimeUiEvent(event);
                return true;
            }
            // 图像属于 editor DOM，但内部场景输入归游戏；不让点击把焦点交给文档根节点。
            if (event.type == PlatformEventType::MouseButtonDown)
            {
                sceneViewportInputActive = true;
                if (editorFocus) rmlContext->GetFocusElement()->Blur();
            }
            if (event.type == PlatformEventType::MouseMove) ProcessRuntimeUiEvent(event);
            return false;
        }
        if (event.type == PlatformEventType::MouseButtonDown)
        {
            sceneViewportInputActive = false;
            editorPointerCaptured = captured || editorHit || modal;
            if (!editorHit && !modal && editorFocus)
                rmlContext->GetFocusElement()->Blur();
        }
        const bool consumed = ProcessRuntimeUiEvent(event);
        if (event.type == PlatformEventType::MouseButtonUp && rmlPressedMouseButtons.empty())
            editorPointerCaptured = false;
        return consumed || editorHit || captured || modal;
    }
    case PlatformEventType::MouseLeave:
        if (materialInstanceEditorPanel) materialInstanceEditorPanel->CancelModelPreviewDrag();
        return ProcessRuntimeUiEvent(event) || editorPointerCaptured;
    case PlatformEventType::KeyUp:
        if (rmlPressedKeyboardKeys.count(event.key) != 0)
            return ProcessRuntimeUiEvent(event) || editorFocus || modal;
        return false;
    case PlatformEventType::KeyDown:
    case PlatformEventType::TextInput:
    case PlatformEventType::TextEditing:
    case PlatformEventType::GamepadButtonDown:
    case PlatformEventType::GamepadButtonUp:
    case PlatformEventType::GamepadAxisMotion:
        if (editorFocus || modal)
        {
            ProcessRuntimeUiEvent(event);
            return true;
        }
        return false;
    default:
        // 焦点与模态只独占输入；Quit 等生命周期事件必须继续交给 EngineLoop。
        return false;
    }
}

void UiSubsystem::RequestOpenJsonAsset(const std::string& path)
{
    RequestViewModelRefresh();
    if (jsonAssetLibrary != nullptr) jsonAssetLibrary->RequestOpen(path);
}

bool UiSubsystem::CancelBlueprintAssetDrag()
{
    return materialInstanceEditorPanel && materialInstanceEditorPanel->CancelBlueprintAssetDrag();
}

void UiSubsystem::RequestRefreshAssetCatalog()
{
    RequestViewModelRefresh();
    if (thumbnailLibrary != nullptr) thumbnailLibrary->Refresh();
    if (jsonAssetLibrary != nullptr) jsonAssetLibrary->RequestRefresh();
}

void UiSubsystem::RequestRuntimeAction(UiAction action)
{
    // 工具条/参数控件产生的运行时请求会改变当前显示值或模式：下一帧强制刷新。
    RequestViewModelRefresh();
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
        if (event.control && event.key == PlatformKey::S && developerUiVisible &&
            materialInstanceEditorPanel != nullptr &&
            !materialInstanceEditorPanel->HasModal())
        {
            materialInstanceEditorPanel->RequestSave();
            return true;
        }
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

void UiSubsystem::QueueAction(UiAction action)
{
    if (commandBus != nullptr)
    {
        commandBus->Queue(std::move(action));
    }
}

} // namespace VL
