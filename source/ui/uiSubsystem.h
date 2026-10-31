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
#include "ui/uiUpdateScheduler.h"
#include "ui/uiViewModelRefresh.h"
#include "ui/uiViewModelSections.h"
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
    // 分节视图模型的消费入口。sections 只携带本帧“重建过”的 GT 分节（脚本/动画/
    // 性能/相机/环境/离线）；资产/大纲/状态由 UiSubsystem 自己发布。mode 决定构建档位；
    // editorSnapshotDue 表示 10 Hz 窗口到期或事件强制，只有此时才发布资产/大纲节、
    // 重建面板数据并刷新 UpdateRuntimeView。
    // RmlUi 上下文更新与绘制快照发布还要再过 UiUpdateScheduler：静止帧两者都跳过，
    // RT 继续绘制上一份不可变快照；输入/DOM 突变/分节变化/10 Hz 兜底才会重做。
    void Update(const UiViewModelSectionUpdate& sections,
        const UiViewModelFrameValues& frameValues,
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
    // MI 模型列表与大纲 actor 列表都是不可变数组的共享指针：同一份内容跨刷新复用，
    // 只有指针或选中态变化才提升大纲分节版本，避免每 100 ms 重挂面板 DOM。
    void SetMaterialInstanceSceneModels(
        std::shared_ptr<const std::vector<EditorUi::MaterialInstanceModelSnapshot>> models);
    void SetSceneActors(
        std::shared_ptr<const std::vector<EditorUi::EditorSceneActorSnapshot>> actors);
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
    // 选中/模式切换/文档命令等事件按“受影响的分节”置脏；EngineLoop 在稳定帧边界
    // 消费掩码，只重建这些节，不受 10 Hz 节拍等待。隐藏期间事件不排队：
    // 重新可见的第一帧本身就会被视为全部节强制刷新。
    UiSectionMask ConsumeViewModelRefreshSections() noexcept
    {
        const UiSectionMask requested = pendingSectionRefresh;
        pendingSectionRefresh = 0;
        return requested;
    }
    // 诊断计数（--gpu-stats 每秒读取一次）：证明静止帧不重建分节、不重建面板，
    // 也不推进 RmlUi 上下文/不重发绘制快照。
    struct RefreshStats
    {
        uint64_t sectionPublishes = 0;
        uint64_t panelBuilds = 0;
        uint64_t runtimeViewUpdates = 0;
        uint64_t bindingDirtyWrites = 0;
        // 第 3 层：rmlUpdates + skippedUpdates 就是可见帧数；snapshotPublishes
        // 通常等于 rmlUpdates，只有 DrawDebug 逐帧变化时才会更多。
        uint64_t rmlUpdates = 0;
        uint64_t snapshotPublishes = 0;
        uint64_t skippedUpdates = 0;
        // 按分节拆开，--gpu-stats 用它说明“每秒到底哪些节在变”。
        std::array<uint64_t, UiSectionCount> sectionPublishesById{};
    };
    RefreshStats ConsumeRefreshStats() noexcept
    {
        const RefreshStats result = refreshStats;
        refreshStats = {};
        return result;
    }
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
    UiRenderSnapshotQueue& GetRenderSnapshotQueue() { return snapshotPublisher.Queue(); }

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
    // 分节脏集只刷新对应绑定：world_text 属于状态节，gameplay_text 属于脚本/HUD 节。
    // 返回 true 表示绑定值确实变化（已 DirtyVariable），调用方需要一次 RmlUi 更新。
    bool SyncWorldTextBinding();
    bool SyncGameplayTextBinding(const ScriptRuntimeView& scripts);
    // 发布 UiSubsystem 拥有的三节（状态/资产/大纲）。只有内容变化才提升版本。
    void PublishStatusSection(const UiViewModelFrameValues& frameValues);
    void PublishAssetsSection();
    void PublishSceneOutlinerSection();
    void RecordSectionPublish(UiSectionId id) noexcept
    {
        ++refreshStats.sectionPublishes;
        ++refreshStats.sectionPublishesById[static_cast<size_t>(id)];
    }
    // 把本帧变化的分节写入投影（UiViewModelSnapshot）并做对应的绑定重脏。
    void ApplyChangedSections(UiSectionMask changed);
    void ApplyFrameValues(const UiViewModelFrameValues& frameValues);
    void MarkSceneOutlinerDirty() noexcept { ++outlinerRevision; }
    // includeUiDocuments=false 时只发布 DrawDebug 叠层（沉浸模式仍显示调试图元）；
    // 仍会发布一份快照，从而清掉 RT 保留的上一帧工作区画面。
    void BuildAndPublishRenderSnapshot(bool includeUiDocuments);
    // DrawDebug 是否属于当前可见输出（与快照构建里的追加条件保持一致）。
    bool IsDebugDrawOverlayVisible() const;
    // 调试几何逐帧变化：即使 RmlUi 上下文不动也必须重发快照。
    // “上一份快照含调试几何”也要继续发布，才能让过期图元消失。
    bool IsDebugDrawOverlayAnimating() const;
    void SetImmersiveDocumentStateIfRestored(const std::string& activeWorldPath);
    void RequestViewModelRefresh(
        UiSectionMask sections = UiSectionAll) noexcept
    {
        pendingSectionRefresh |= sections;
        // 事件声明了要重建的分节，就必然要走过一次 RmlUi 更新；即使版本比较
        // 最终没变化（例如重复命令），也强制本轮不再跳过，避免“事件已消费、
        // 界面却等到下个 10 Hz 窗口”的延迟。
        uiUpdateScheduler.MarkMutation(UiMutationKind::ForcedRefresh);
    }
    // 面板的 Build 输入来自资产/大纲/脚本/状态四节；用户点击等面板本地交互也会
    // 请求一次构建，避免“数据没变但界面状态变了”的交互被门禁吞掉。
    void RequestPanelBuild() noexcept
    {
        panelBuildRequested = true;
        uiUpdateScheduler.MarkMutation(UiMutationKind::PanelBuild);
    }
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
    // 大纲/MI 模型列表都是不可变数组：只有内容或选中态变化才提升大纲节版本。
    std::shared_ptr<const std::vector<EditorUi::MaterialInstanceModelSnapshot>>
        sceneModels;
    std::shared_ptr<const std::vector<EditorUi::EditorSceneActorSnapshot>>
        sceneActors;
    // 分节存储是视图模型的唯一权威：九个节各自持有不可变载荷与版本。
    UiViewModelSectionStore sectionStore;
    // 已应用到面板/绑定的版本集合；ChangedFrom 产生本帧的脏集。
    UiSectionVersions appliedSectionVersions;
    UiAssetsSection appliedAssets;
    UiPerformanceSection appliedPerformance;
    // 大纲节版本源：actor/模型指针替换或选中态变化时推进；不引入第二份选择状态。
    uint64_t outlinerRevision = 0;
    // 上一份离线节里的模式，用于判断本次发布是否真的发生了模式切换。
    bool lastPublishedOfflineMode = false;
    // 面板本地交互（点击/滚轮/键盘）请求一次构建；不包含悬停移动。
    bool panelBuildRequested = true;
    RefreshStats refreshStats;
    UiSectionMask pendingSectionRefresh = UiSectionAll;
    // RuntimeHud 档位的逐帧脚本文本；不覆盖编辑器投影里的类/反馈字段。
    ScriptRuntimeView hudScripts;
    UiViewModelBuildMode currentBuildMode = UiViewModelBuildMode::None;
    // TickMaterialInstanceEditor 只在脚本节版本变化时把蓝图类转交给资产文档服务。
    uint64_t blueprintClassesVersion = 0;
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
    // 第 3 层发布闸门：跳过帧保留 lastPublished 的同一份不可变快照。
    UiRenderSnapshotPublisher snapshotPublisher;
    // 可见 UI 是否需要 RmlUi 更新/快照发布；纯逻辑，测试见 uiUpdateSchedulerTests。
    UiUpdateScheduler uiUpdateScheduler;
    // 上一份已发布快照是否含 DrawDebug 几何：用于让过期调试图元在一帧内消失。
    bool lastPublishedHadDebugOverlay = false;
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
