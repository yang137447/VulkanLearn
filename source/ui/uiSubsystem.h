#pragma once

#include <array>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include <RmlUi/Core/DataModelHandle.h>
#include <RmlUi/Core/Input.h>

#include "core/runtimeResult.h"
#include "editor/ui/materialInstanceAssetEditorPanel.h"
#include "platform/platformEvent.h"
#include "ui/rmlUiInterfaces.h"
#include "ui/rmlUiRuntime.h"
#include "ui/uiAction.h"
#include "ui/uiRenderSnapshot.h"
#include "ui/uiRenderSnapshotQueue.h"
#include "ui/uiViewModelRefresh.h"
#include "ui/viewport/editorViewportGeometry.h"

namespace Rml
{
class Context;
class ElementDocument;
class Event;
}

namespace VL
{

class CommandBus;
class DiagnosticsSubsystem;
class PlatformWindow;

namespace Editor
{
    class MaterialInstanceEditorRuntime;
    class SceneDocumentService;
    class EditorJsonAssetLibrary;
    class EditorThumbnailLibrary;
namespace Selection
{
struct MaterialInstanceSelection;
}
namespace Preview
{
class IMaterialInstancePreviewAdapter;
}
}

namespace EditorUi
{
}

// Describes UI assets, viewport, localization, fonts, and runtime feature switches.
// RuntimeConfig supplies it; UiSubsystem owns the resulting widget contexts.
struct UiSubsystemDesc
{
    uint32_t viewportWidth = 0;
    uint32_t viewportHeight = 0;
    bool srgbFramebuffer = true;
    std::filesystem::path assetRoot;
    std::filesystem::path documentPath;
    std::filesystem::path localizationPath;
    std::filesystem::path sceneRoot;
    std::string defaultLocale;
    std::vector<std::filesystem::path> fontFaces;
    bool hotReload = true;
    bool developerUiEnabled = true;
    bool developerUiVisible = false;
    // EngineLoop 注入 renderer-owned adapter；UiSubsystem 不拥有也不直接调用它。
    Editor::Preview::IMaterialInstancePreviewAdapter* previewAdapter = nullptr;
};

struct UiSceneOption
{
    Rml::String label;
    Rml::String path;
    bool active = false;
    std::string normalizedPhysicalPath;

    bool operator==(const UiSceneOption& other) const
    {
        return label == other.label && path == other.path && active == other.active &&
            normalizedPhysicalPath == other.normalizedPhysicalPath;
    }
};

// GT 拥有运行时与编辑器 RML 文档；RT 只消费冻结的 draw snapshot。
// It publishes typed actions and immutable snapshots, but does not mutate renderer internals.
class UiSubsystem : public EditorUi::IMaterialInstanceSceneSelectionSink,
    public EditorUi::IEditorRuntimeActionSink,
    public EditorUi::IEditorJsonAssetRequestSink
{
public:
    UiSubsystem();
    ~UiSubsystem();

    UiSubsystem(const UiSubsystem&) = delete;
    UiSubsystem& operator=(const UiSubsystem&) = delete;

    RuntimeResult<void> Initialize(
        const UiSubsystemDesc& desc,
        PlatformWindow& platformWindow,
        CommandBus& commandBus,
        const DiagnosticsSubsystem& diagnosticsSubsystem);
    void Shutdown();

    bool HandlePlatformEvent(const PlatformEvent& event);
    // mode 决定构建哪些数据；editorSnapshotDue 表示本帧到达编辑器快照刷新节拍
    // （或事件强制），只有此时才重建面板 DOM。可见性隐藏时 mode != FullEditor，
    // 面板构建与 RML 主更新都会跳过；隐藏帧仍发布空/仅 DrawDebug 的渲染快照，
    // 避免 RT 保留上一帧工作区画面。
    void Update(const UiViewModelSnapshot& viewModelSnapshot,
        UiViewModelBuildMode mode,
        bool editorSnapshotDue);
    void Resize(uint32_t width, uint32_t height);
    void ApplyAction(const UiAction& action);
    void NotifyWorldChanged(const std::string& worldPath, uint64_t generation);
    void NotifyWorldLoadCompleted();
    // 只由 EngineLoop 在稳定帧边界调用，避免 UI 回调触碰 live MaterialInstance。
    void TickMaterialInstanceEditor();
    Editor::SceneDocumentService* GetSceneDocuments() const;
    Editor::MaterialInstanceEditorRuntime* GetEditorCommandRuntime() const
    { return materialInstanceEditorRuntime.get(); }
    Editor::EditorThumbnailLibrary* GetThumbnailLibrary() const { return thumbnailLibrary.get(); }
    bool CanRenderThumbnails() const
    { return developerUiVisible && !sceneViewportInputActive && !editorPointerCaptured; }
    // 视口拾取只负责打开文档并记录导航来源，实际编辑仍经由 EditorCommand。
    bool SelectSceneObjectFromPick(
        const Editor::Selection::MaterialInstanceSelection& selection);
    void SetMaterialInstanceSceneModels(
        const std::vector<Editor::Selection::MaterialInstanceModelContext>& models);
    // 世界大纲的冻结 actor 列表：EngineLoop 在稳定帧边界从 active World 拍出，
    // 与 SetMaterialInstanceSceneModels 同一位置发布，UI 只接收值语义副本。
    void SetSceneActors(std::vector<EditorUi::EditorSceneActorSnapshot> actors);
    // 性能快照与 actor 列表同一位置发布；UI 只接收按值复制的 CPU/GPU 数字，
    // 不持有计时器或 Vulkan 查询对象。每帧一份，供 Performance 页按 5 Hz 取样。
    void SetPerformanceSnapshot(VL::PerformanceSnapshot snapshot);
    std::optional<Editor::Selection::MaterialInstanceModelContext>
        ConsumeMaterialInstanceModelSelectionRequest();
    std::optional<Editor::Selection::MaterialInstanceSelection>
        ConsumeMaterialInstanceSelectionRequest();
    // 大纲选中非网格 actor 时要求清掉渲染端的 mesh 选中描边，避免出现
    // "大纲高亮灯光、视口还描着上一个网格" 的双选中态。
    bool ConsumeSceneSelectionClearRequest();
    void ClearMaterialInstanceSceneSelection() noexcept;

    bool RequestModelSelection(
        const Editor::Selection::MaterialInstanceModelContext& model) override;
    bool RequestMaterialSelection(
        const Editor::Selection::MaterialInstanceSelection& selection) override;
    bool RequestActorSelection(const std::string& actorIdentity) override;
    bool CancelBlueprintAssetDrag();
    void RequestRuntimeAction(UiAction action) override;
    void RequestOpenJsonAsset(const std::string& assetPath) override;
    void RequestRefreshAssetCatalog() override;

    bool IsInitialized() const { return initialized; }
    bool IsDeveloperUiVisible() const { return developerUiVisible; }
    bool IsDeveloperUiEnabled() const { return developerUiEnabled; }
    // 运行时 HUD 只在开发者 UI 与沉浸模式都不可见时显示（与 ApplyDocumentState 一致）。
    bool IsRuntimeHudVisible() const
    {
        return initialized && !developerUiVisible && !immersiveView;
    }
    // 选中/模式切换/文档命令等事件置脏；EngineLoop 在稳定帧边界消费后强制一次
    // 完整编辑器快照重建，不受 10 Hz 节拍等待。隐藏期间事件不排队：
    // 重新可见的第一帧本身就会被视为强制刷新。
    bool ConsumeViewModelRefreshRequest() noexcept;
    uint32_t GetViewportWidth() const { return desc.viewportWidth; }
    uint32_t GetViewportHeight() const { return desc.viewportHeight; }
    const UiViewportRect& GetSceneViewportRect() const { return sceneViewportRect; }
    bool IsSceneViewportEnabled() const
    {
        // 资源标签同样消费 renderer-owned 离屏图像；不能因资产编辑器可见而关闭输出。
        return developerUiVisible;
    }
    std::optional<UiViewportPoint> GetSceneViewportPoint(float x, float y) const
    {
        return MapViewportPoint(sceneViewportRect, x, y);
    }
    bool IsScenePickTarget(float mouseX, float mouseY) const;
    bool ShouldUseRelativeMouseModeForGame() const;
    bool ShouldGameReceiveKeyboard() const;
    bool ShouldGameReceivePointer() const;
    const UiInputOwnershipSnapshot& GetInputOwnership() const { return inputOwnership; }
    UiRenderSnapshotQueue& GetRenderSnapshotQueue() { return renderSnapshotQueue; }

private:
    void SetImmersiveView(bool enabled);
    using LocalizationTable = std::unordered_map<std::string, std::string>;
    using LocalizationDatabase = std::unordered_map<std::string, LocalizationTable>;
    using AssetFingerprint = std::unordered_map<std::string, uint64_t>;

    struct BindingData
    {
        Rml::String worldText;
        Rml::String gameplayText;
        bool sceneLoading = false;
        std::vector<UiSceneOption> sceneOptions;
    };

    RuntimeResult<void> InitializeRmlUi();
    RuntimeResult<void> InitializeDataModel();
    RuntimeResult<void> InitializeDeveloperUi();
    RuntimeResult<void> InitializeMaterialInstanceEditor();
    RuntimeResult<void> LoadFonts();
    void LoadSceneOptions();
    RuntimeResult<LocalizationDatabase> LoadLocalizationCandidate() const;
    RuntimeResult<Rml::ElementDocument*> LoadDocumentCandidate();
    RuntimeResult<Rml::ElementDocument*> LoadEditorDocumentCandidate();
    RuntimeResult<void> CommitInitialAssets();
    void CommitDocumentCandidate(
        Rml::ElementDocument& candidate,
        LocalizationDatabase localizationCandidate);
    bool ValidateDocument(Rml::ElementDocument& document, std::string& outError) const;
    void ApplyDocumentState();
    void TryHotReload();
    AssetFingerprint BuildAssetFingerprint() const;
    bool HasAssetFingerprintChanged(const AssetFingerprint& candidate) const;

    void SyncBindingData();
    // includeUiDocuments=false 时只发布 DrawDebug 叠层（沉浸模式仍显示调试图元）；
    // 仍会发布一份快照，从而清掉 RT 保留的上一帧工作区画面。
    void BuildAndPublishRenderSnapshot(bool includeUiDocuments);
    void SetImmersiveDocumentStateIfRestored(const UiViewModelSnapshot& viewModelSnapshot);
    void RequestViewModelRefresh() noexcept { viewModelRefreshRequested = true; }
    std::unique_ptr<class DebugDrawLabels> debugDrawLabels;
    void BuildDeveloperPanels();
    void UpdateInputOwnership();

    bool ProcessRuntimeUiEvent(const PlatformEvent& event);
    bool ProcessDeveloperUiEvent(const PlatformEvent& event);
    bool PressRmlKey(Rml::Input::KeyIdentifier key, bool repeat, int modifiers = 0);
    bool ReleaseRmlKey(Rml::Input::KeyIdentifier key, int modifiers = 0);
    bool SetRmlAxisDirection(
        bool pressed,
        bool& currentState,
        Rml::Input::KeyIdentifier key);
    bool ProcessRmlGamepadAxisEvent(const PlatformEvent& event);
    void ResetRuntimeInputState();
    void ResetRmlGamepadInputState();
    bool HandleGlobalShortcut(const PlatformEvent& event);
    void QueueAction(UiAction action);
    UiSubsystemDesc desc;
    PlatformWindow* window = nullptr;
    CommandBus* commandBus = nullptr;
    const DiagnosticsSubsystem* diagnostics = nullptr;
    std::unique_ptr<Editor::MaterialInstanceEditorRuntime>
        materialInstanceEditorRuntime;
    std::unique_ptr<EditorUi::MaterialInstanceAssetEditorPanel>
        materialInstanceEditorPanel;
    std::unique_ptr<Editor::EditorJsonAssetLibrary> jsonAssetLibrary;
    std::unique_ptr<Editor::EditorThumbnailLibrary> thumbnailLibrary;
    std::optional<EditorUi::MaterialInstanceModelSnapshot>
        selectedModelMaterials;
    std::optional<uint32_t> selectedMaterialSlotIndex;
    std::vector<EditorUi::MaterialInstanceModelSnapshot> sceneModels;
    // 大纲列表是逐帧替换的不可变数组：面板只借用同一个 shared_ptr，不做深拷贝。
    std::shared_ptr<const std::vector<EditorUi::EditorSceneActorSnapshot>> sceneActors;
    // 每帧从 RenderSystem 复制的性能快照；面板按自己的刷新节奏读取副本。
    PerformanceSnapshot performanceSnapshot;
    // 大纲选中态是稳定身份（"mesh|…"、"pointLight|…"），World 换代时清空。
    std::string selectedActorIdentity;
    bool pendingSceneSelectionClear = false;
    std::optional<Editor::Selection::MaterialInstanceModelContext>
        pendingMaterialInstanceModelSelection;
    std::optional<Editor::Selection::MaterialInstanceSelection>
        pendingMaterialInstanceSelection;
    Rml::Context* rmlContext = nullptr;
    Rml::ElementDocument* runtimeDocument = nullptr;
    Rml::ElementDocument* editorDocument = nullptr;
    // 本子系统服务的窗口序号。它唯一的用途是给 RmlUi context 起唯一名字
    // （`Rml::CreateContext` 要求名字唯一），因此必须是稳定身份而不是指针值。
    // 多窗口接线时由创建方按窗口注册表填入。
    size_t windowIndex = 0;
    RmlUiSystemInterface rmlSystemInterface;
    RmlUiTextInputHandler rmlTextInputHandler;
    RmlUiRenderInterface rmlRenderInterface;
    Rml::DataModelHandle dataModelHandle;
    BindingData bindingData;
    LocalizationDatabase localizations;
    AssetFingerprint assetFingerprint;
    UiRenderSnapshotQueue renderSnapshotQueue;
    UiViewModelSnapshot lastViewModel;
    UiInputOwnershipSnapshot inputOwnership;
    std::unordered_map<Rml::Input::KeyIdentifier, uint32_t> rmlKeyPressCounts;
    std::unordered_set<PlatformKey> rmlPressedKeyboardKeys;
    std::unordered_set<PlatformMouseButton> rmlPressedMouseButtons;
    std::unordered_set<PlatformGamepadButton> rmlPressedGamepadButtons;
    std::chrono::steady_clock::time_point nextHotReloadCheck;
    uint64_t uiFrameIndex = 0;
    size_t previousUiVertexCount = 0;
    size_t previousUiIndexCount = 0;
    size_t previousUiDrawCount = 0;
    std::string currentLocale;
    std::string hotReloadStatus;
    UiViewportRect sceneViewportRect;
    bool initialized = false;
    bool rmlInitialized = false;
    bool updatingRuntimeBindings = false;
    bool viewModelRefreshRequested = false;
    bool developerUiEnabled = false;
    bool developerUiVisible = false;
    bool immersiveView = false;
    std::string immersiveLevelPath;
    bool immersiveLoadCompleted = false;
    float pointerX = 0.0f;
    float pointerY = 0.0f;
    bool editorPointerCaptured = false;
    // Viewport 捕获后不再用绝对光标位置决定输入归属，避免相对鼠标移出时误释放。
    bool sceneViewportInputActive = false;
    bool gamepadConnected = false;
    bool rmlAxisLeftDown = false;
    bool rmlAxisRightDown = false;
    bool rmlAxisUpDown = false;
    bool rmlAxisDownDown = false;
};

} // namespace VL
