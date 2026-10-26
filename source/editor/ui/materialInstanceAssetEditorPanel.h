#pragma once

#include <array>
#include <cstddef>
#include <memory>
#include <string>
#include <utility>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include <RmlUi/Core/EventListener.h>

#include "editor/ui/materialInstanceEditorView.h"
#include "editor/ui/editorDockingController.h"
#include "editor/ui/editorContentBrowserController.h"
#include "editor/ui/editorJsonInspectorController.h"
#include "editor/ui/editorResourceEditorController.h"
#include "editor/ui/retargetAssetEditorPanel.h"
#include "editor/ui/editorSceneActorTree.h"
#include "editor/ui/editorLogPanel.h"
#include "editor/ui/editorPerformancePanel.h"
#include "editor/ui/blueprintViewportInteraction.h"
#include "editor/ui/tressfxHairPreview.h"
#include "ui/uiAction.h"
#include "ui/uiRenderSnapshot.h"
#include "ui/widgets/widgetDocument.h"

namespace Rml
{
class Element;
class ElementDocument;
}

namespace VL::Animation { struct MotionCandidate; }

namespace VL::EditorUi
{

// 运行时控制仍走 UiAction，与资产文档命令保持不同的所有权和保存语义。
class IEditorRuntimeActionSink
{
public:
    virtual ~IEditorRuntimeActionSink() = default;
    virtual void RequestRuntimeAction(UiAction action) = 0;
};

// RML 工作区的 GT 展示适配器；消费冻结快照，仅提交命令，不调用文档服务或 renderer。
class MaterialInstanceAssetEditorPanel final : public Rml::EventListener
{
public:
    explicit MaterialInstanceAssetEditorPanel(IEditorCommandSink* commandSink = nullptr);
    ~MaterialInstanceAssetEditorPanel() override;
    static bool ValidateDocument(Rml::ElementDocument& document, std::string& error);
    bool Attach(Rml::ElementDocument& document, std::string& error);
    void Detach();
    void SetSceneSelectionSink(IMaterialInstanceSceneSelectionSink* sink);
    void SetRuntimeActionSink(IEditorRuntimeActionSink* sink);
    void SetJsonAssetRequestSink(IEditorJsonAssetRequestSink* sink) { jsonAssetRequestSink = sink; }
    void ShowMaterialDetails() { viewingJson = false; resourceEditorVisible = false; requestedJsonPath.clear(); activateMaterialDocument = true; }
    void ShowSceneDetails() { viewingJson = false; requestedJsonPath.clear(); activateMaterialDocument = false; }
    void Build(const MaterialInstanceEditorSnapshot& snapshot);
    // Performance 页按自己的 5 Hz 取样：数据节每 10 Hz 发布一次，但不需要因此
    // 重建整份面板 DOM，所以单独暴露两个取样入口。
    void UpdatePerformance(const PerformanceSnapshot& performance);
    void UpdateScriptPerformance(const nlohmann::json& scriptPerformance);
    void UpdateRuntimeView(const UiViewModelSnapshot& view);
    std::vector<std::string> VisibleThumbnailPaths() const { return contentBrowser.VisibleThumbnailPaths(); }
    void RequestSave();
    void RequestSaveAll();
    bool PrepareImmersiveView(std::string& levelToLoad);
    void SetWorkspaceLayoutPath(std::filesystem::path path) { docking.SetPersistencePath(std::move(path)); }
    UiViewportRect GetSceneViewportRect() const { return docking.GetSceneViewportRect(); }
    bool IsSceneViewportElement(const Rml::Element* element) const;
    // 面板可见性由停靠视图拥有；菜单、测试和 UiSubsystem 都经由这一层访问。
    bool IsPanelVisible(const std::string& panel) const { return docking.IsPanelVisible(panel); }
    bool SetPanelVisible(const std::string& panel, bool visible) { return docking.SetPanelVisible(panel, visible); }
    bool IsMenuOpen(const std::string& menu) const { return openMenu == menu; }
    // 右键上下文菜单只有一份状态；目标见 ContextMenuTarget（稳定地址，不是快照下标）。
    bool IsContextMenuOpen() const { return contextMenuKind != ContextMenuKind::None; }
    const std::string& ContextMenuTarget() const { return contextMenuTarget; }
    bool IsAboutVisible() const { return aboutVisible; }
    // 分组展开状态是会话 UI 状态：不进入资产文档，也不随停靠布局保存。
    bool IsSectionExpanded(const std::string& key) const { return collapsedSections.count(key) == 0; }
    // 变换工具与坐标系只是编辑器本地呈现状态，没有 gizmo 或 transform 编辑后端。
    const std::string& ViewportTransformTool() const { return viewportTransformTool; }
    const std::string& ViewportTransformSpace() const { return viewportTransformSpace; }
    const std::vector<std::string>& MenuLevelPaths() const { return menuLevelPaths; }
    bool IsResourceEditorVisible() const;
    bool IsMotionDebugViewport() const;
    bool IsAssetEditorVisible() const { return IsResourceEditorVisible() || IsMaterialEditorVisible(); }
    bool IsWorkspaceDragging() const { return docking.IsDragging(); }
    bool CancelBlueprintAssetDrag()
    {
        const bool active = !actorDragPath.empty() || blueprintGizmoDragging;
        actorDragPath.clear();
        CancelBlueprintViewportDrag();
        return active;
    }
    size_t WorkspacePropertyWriteCount() const { return docking.PropertyWriteCount(); }
    size_t PanelPropertyWriteCount() const { return widgetDocument.PropertyWriteCount(); }
    void CancelWorkspaceDrag() { docking.CancelDrag(); CancelModelPreviewDrag(); }
    void CancelModelPreviewDrag()
    {
        previewDragButton = textureDragButton = -1;
        actorDragPath.clear();
        CancelBlueprintViewportDrag();
    }
    bool HasModal() const { return !pendingClosePath.empty() || aboutVisible || saveDialogVisible; }
    void ProcessEvent(Rml::Event& event) override;

private:
    // 上下文菜单的目标类别。目标只用稳定地址表达（actor 稳定身份 / 资产完整路径）：
    // 菜单条目要到下一次点击才执行，而快照下标只在当帧有效，用下标会指到另一个对象。
    enum class ContextMenuKind
    {
        None,
        Actor,
        BrowserAsset,
        BlueprintClass,
        Viewport
    };
    struct PendingEdit
    {
        std::string key;
        EditorCommandBatchItem item;
    };
    void Submit(EditorCommandEnvelope command);
    void SubmitPathCommand(EditorCommandType type);
    void QueueEdit(std::string key, EditorCommandBatchItem item);
    void FlushPendingEdits(bool save = false);
    void RefreshLists();
    void RefreshOutliner();
    void RebuildOutlinerRows(const std::vector<EditorSceneActorSnapshot>& actors);
    bool OutlinerRowsNeedRebuild(const std::vector<EditorSceneActorSnapshot>& actors) const;
    EditorSceneActorWindow OutlinerWindow() const;
    void MeasureOutlinerRowHeight();
    std::string OutlinerMountedVisuals() const;
    std::string OutlinerRowsMarkup(const EditorSceneActorWindow& window) const;
    void RefreshDetails();
    void RefreshControls();
    void OpenSaveDialog(const std::string& path);
    void RefreshSaveDialog();
    void ConfirmSave();
    void RefreshActorValues(const EditorSceneActorSnapshot* actor);
    void RefreshScriptCollisionControls(size_t index, const nlohmann::json& node,
        const std::string& prefix, bool disabled);
    void RefreshScriptConstraintControls(const nlohmann::json& physics,
        const std::string& prefix, bool disabled);
    std::string ActorSectionMarkup(const EditorSceneActorSnapshot& actor);
    std::string PrimaryDirectionalLightControls(const EditorSceneActorSnapshot& actor) const;
    const std::vector<EditorSceneActorSnapshot>& OutlinerActors() const;
    const EditorSceneActorSnapshot* SelectedActor() const;
    void SelectOutlinerActor(const EditorSceneActorSnapshot& actor);
    void RefreshWorkspace();
    void RefreshMenus();
    void RefreshLevelMenu();
    void RefreshViewportToolbar();
    void RefreshScriptControls();
    void RefreshMotionMatching();
    void RefreshMotionMatchingCandidate(size_t slot, const Animation::MotionCandidate* candidate);
    void RefreshMotionMatchingQuery();
    void RequestScriptEdit(UiActionType type, nlohmann::json payload = nlohmann::json::object());
    const EditorAssetDocumentResultPayload* BlueprintDocument(const std::string& path) const;
    void RefreshBlueprintAssetEditor();
    void RefreshBlueprintPreview();
    void RefreshBlueprintViewport();
    bool ProcessBlueprintViewportInput(Rml::Event& event);
    void CancelBlueprintViewportDrag(bool restorePreview = true);
    void PublishBlueprintViewportPreview(bool reset = false);
    const EditorSceneActorSnapshot* BlueprintPreviewActor() const;
    const EditorSceneCameraActor* BlueprintPreviewCamera() const;
    bool BlueprintViewportEditable() const;
    bool RequestBlueprintHistory(bool redo);
    void RefreshBlueprintHistoryControls();
    bool blueprintCommandAwaitingSnapshot = false;
    uint64_t blueprintCommandRevision = 0;
    std::string blueprintCommandPath;
    uint64_t blueprintCommandFeedbackSequence = 0;
    std::string blueprintViewportTool = "select";
    BlueprintViewportGizmo blueprintViewportGizmo;
    BlueprintViewportGizmo blueprintDragGizmo;
    bool blueprintGizmoDragging = false;
    size_t blueprintGizmoAxis = 0;
    Eigen::Vector2f blueprintGizmoStart = Eigen::Vector2f::Zero();
    Eigen::Vector2f blueprintGizmoOffset = Eigen::Vector2f::Zero();
    uint64_t blueprintGizmoGeneration = 0;
    uint64_t blueprintGizmoRevision = 0;
    std::string blueprintGizmoPath;
    std::string blueprintGizmoComponent;
    nlohmann::json blueprintGizmoDraft = nullptr;
    std::vector<EditorScriptComponentBounds> blueprintGizmoOriginalBounds;
    bool blueprintSnapEnabled = false;
    float blueprintMoveSnap = 10;
    float blueprintRotateSnap = 15;
    float blueprintScaleSnap = 0.1f;
    void RequestBlueprintPreview();
    uint64_t CurrentBlueprintPreviewGeneration() const;
    uint64_t requestedBlueprintPreviewRevision = 0;
    std::string blueprintPreviewVisualSignature;
    double blueprintPreviewChangeTime = 0;
    bool blueprintPreviewPending = false;
    void HandleBlueprintAssetEvent(Rml::Element& element, const std::string& action, bool click);
    std::string BlueprintLogicTree(const nlohmann::json&) const;
    std::string BlueprintLogicDetails(const nlohmann::json&) const;
    void RefreshBlueprintLogicComponents(const nlohmann::json&, bool);
    bool HandleBlueprintLogicEvent(Rml::Element&, const std::string&, bool, nlohmann::json&);
    std::string SceneLogicStructure(const EditorSceneActorSnapshot&) const;
    std::string SceneLogicComponentsMarkup(const EditorSceneActorSnapshot&) const;
    void RefreshSceneLogicComponents(const EditorSceneActorSnapshot&);
    bool HandleSceneLogicEvent(Rml::Element&, const std::string&, bool);
    bool HandleSceneCompositePropertyEvent(Rml::Element&, const std::string&, bool);
    void QueueBlueprintAssetDraft(nlohmann::json candidate);
    void FlushBlueprintAssetEdits();
    void SubmitBlueprintAssetCommand(EditorCommandType type, const std::string& path);
    std::string blueprintAssetIdentity;
    std::string blueprintAssetSelectionPath;
    std::string blueprintAssetSelectedComponent;
    std::string blueprintAssetSelectedLogicComponent;
    std::string pendingBlueprintPath;
    uint64_t pendingBlueprintRevision = 0;
    nlohmann::json pendingBlueprintDraft = nullptr;
    EditorAssetKind pendingCloseKind = EditorAssetKind::MaterialInstance;
    bool discardBlueprintThenClose = false;
    bool discardBlueprintCloseSubmitted = false;
    bool scriptPlacementVisible = false;
    bool scriptPlacementCreateAsset = false;
    void OpenBlueprintClassSource(const std::string& className);
    void BeginBlueprintClassAsset(const std::string& className);
    bool ProcessBlueprintAssetDrag(Rml::Event& event);
    std::string actorDragPath;
    std::unordered_set<std::string> scriptEditRequests;
    uint64_t actorDragGeneration = 0;
    void RefreshViewportAxisIndicator();
    void RefreshOutlinerVisuals();
    void RefreshActorVisualization();
    void SetViewportToolbarClass(const std::string& id, bool active);
    std::string WindowMenuMarkup() const;
    void ToggleMenu(const std::string& menu);
    void CloseMenus();
    void ApplyMenuState();
    bool IsInsideMenuBar(const Rml::Element* element) const;
    void OpenContextMenu(Rml::Event& event);
    void CloseContextMenu();
    void ApplyContextMenuState();
    std::string ContextMenuItemsMarkup() const;
    bool IsInsideContextMenu(const Rml::Element* element) const;
    const EditorSceneActorSnapshot* ContextMenuActor() const;
    bool FindCatalogAssetKind(const std::string& path, EditorAssetKind& kind,
        std::string& name) const;
    void ToggleContextActorVisibility();
    void OpenContextActorMaterial(size_t slotIndex);
    void OpenContextBrowserAsset();
    void SetMarkup(const std::string& id, std::string markup);
    void SetControlValue(const std::string& id, const std::string& value);
    void SetDisabled(const std::string& id, bool disabled);
    void ToggleSection(const std::string& key);
    void ApplySectionState();
    void HandleClick(Rml::Element& element, const std::string& action);
    void HandleChange(Rml::Element& element, const std::string& action, const Rml::Event* event = nullptr);
    void CloseDocument(const std::string& path, EditorDirtyDocumentPolicy policy);
    void SelectModelMaterial(const MaterialInstanceModelSnapshot& model, size_t index);
    void OpenBrowserAsset(const std::string& path);
    void RefreshResourceEditor();
    void RefreshDocumentTabs();
    void OpenResourceAsset(const std::string& path, EditorAssetKind kind);
    void RequestCloseResource(const std::string& path);
    void CloseResource(const std::string& path);
    void ActivateWorkspacePanel(const std::string& panel);
    void RequestResourceReload();
    void RequestActiveResourceMetadata();
    void RequestModelPreview(const std::string& path);
    void RequestAssetPreview(const std::string& path, bool material,
        const std::string& retargetPath = {}, uint64_t retargetRevision = 0);
    void RefreshRetargetAnimation(const EditorResourceEditorTab& tab);
    void RefreshAnimationControls(const std::string& prefix, bool enabled);
    void RefreshMaterialPreview();
    bool IsMaterialEditorVisible() const;
    uint64_t CurrentMaterialPreviewGeneration() const;
    uint64_t CurrentPreviewCameraGeneration() const;
    void ReturnToLevel();
    bool HasCurrentModelPreview() const;
    uint64_t CurrentModelPreviewGeneration() const;
    bool ProcessModelPreviewInput(Rml::Event& event);
    bool ProcessTexturePreviewInput(Rml::Event& event);
    void ResetTextureView();
    std::string textureViewSource;
    float textureZoom = 1.0f;
    Rml::Vector2f texturePan{0, 0};
    Rml::Vector2f texturePointer{0, 0};
    int textureDragButton = -1;
    void RequestPreviewCamera(ModelPreviewNavigation mode, float x = 0, float y = 0);
    // 毛发视口：只有 tressfx 且未声明 simulation.skinning 的资产允许预览。
    // 左键拖动旋转预览对象（不是相机），复位按钮排队一次 ResetPositions。
    bool IsActiveHairPreview() const;
    void RequestHairPreviewRotation(float deltaX, float deltaY);
    void RequestHairReset();
    int previewDragButton = -1;
    float previewPointerX = 0;
    float previewPointerY = 0;
    uint64_t previewDragGeneration = 0;
    std::string requestedPreviewPath;
    std::string requestedRetargetPath;
    uint64_t requestedRetargetRevision = 0, initializedRetargetGeneration = 0;
    std::string reloadingRetargetPath;
    uint64_t reloadingRetargetRevision = 0;
    std::string levelWorldPath;
    uint64_t requestedPreviewGeneration = 0;
    bool requestedPreviewMaterial = false;
    bool requestedPreviewBlueprint = false;
    uint64_t appliedMaterialPreviewGeneration = 0;
    // 毛发视口状态：catalog/snapshot 判定出的 tressfx 路径、skinning 守卫、
    // 以及预览世界内的累计旋转。都不持久化，切 tab/返回关卡即失效。
    std::string hairPreviewPath;
    bool hairPreviewBlocked = false;
    TressfxHairRotation hairRotation;
    // 毛发视口 Simulation 段：作者值来自 TressFX 草稿快照，拖动先走运行期覆盖，
    // Save to Asset 走标准 tressfx.modify + Save Content（revision 取自同一快照）。
    const EditorAssetDocumentResultPayload* TressfxDocument(const std::string& path) const;
    const EditorAssetDocumentResultPayload* ResourceDocument(const std::string& path) const;
    void SubmitResourceDocumentCommand(EditorCommandType type, const std::string& path);
    void RequestTressfxDocument(const std::string& path);
    std::unordered_set<std::string> requestedTressfxDocuments;
    void RequestHairSimulationDocument(const std::string& path);
    void RefreshHairSimulationSection(bool isHair, bool hairBlocked);
    void SubmitHairSimulationLive(std::size_t controlIndex, const std::array<double, 3>& values);
    void SubmitHairSimulationSave();
    std::string hairSimulationPath;
    nlohmann::json hairSimulationValues = nlohmann::json::object();
    uint64_t hairSimulationBoundRevision = 0;
    uint64_t hairSimulationSaveRevision = 0;
    bool hairSimulationRequested = false;
    bool hairSimulationPendingSave = false;
    int hairSimulationSaveWaitTicks = 0;
    std::string hairSimulationStatus;

    IEditorCommandSink* commandSink = nullptr;
    IMaterialInstanceSceneSelectionSink* sceneSelectionSink = nullptr;
    IEditorRuntimeActionSink* runtimeActionSink = nullptr;
    IEditorJsonAssetRequestSink* jsonAssetRequestSink = nullptr;
    Rml::ElementDocument* document = nullptr;
    MaterialInstanceEditorSnapshot snapshot;
    UiViewModelSnapshot runtimeView;
    bool runtimeControlsCurrent = false;
    bool motionMatchingRowsMounted = false;
    bool motionMatchingVisible = false;
    bool motionMatchingQueryExpanded = false;
    std::vector<std::string> motionMatchingQueryChannels;
    double motionMatchingRefreshTime = -1;
    std::string motionMatchingRefreshKey;
    bool textureOptionsDirty = true;
    std::vector<std::string> textureOptionSelections;
    UiWidgets::WidgetDocument widgetDocument;
    EditorDockingController docking;
    EditorContentBrowserController contentBrowser;
    EditorJsonInspectorController jsonInspector;
    EditorJsonInspectorController resourceJsonInspector;
    EditorResourceEditorController resourceEditor;
    RetargetAssetEditorPanel retargetEditor;
    EditorLogPanel logPanel;
    // Performance 页只消费 MaterialInstanceEditorSnapshot.performance 的冻结副本。
    EditorPerformancePanel performancePanel;
    bool viewingJson = false;
    bool resourceEditorVisible = false;
    std::string requestedJsonPath;
    // 顶部菜单的展开状态是本地面板状态：它不进入资产文档、停靠布局文件或运行时快照。
    std::string openMenu;
    // 右键上下文菜单：目标在打开那一刻解析成稳定地址，条目点击不再读面板的当前选择。
    // 位置是右键坐标（文档坐标），只用于放置根节点上的弹层。
    ContextMenuKind contextMenuKind = ContextMenuKind::None;
    std::string contextMenuTarget;
    float contextMenuX = 0.0f;
    float contextMenuY = 0.0f;
    bool aboutVisible = false;
    bool saveDialogVisible = false;
    bool saveAwaitingSnapshot = false;
    std::string saveFilterPath;
    std::vector<EditorModifiedDocumentSnapshot> saveRows;
    std::unordered_set<std::string> saveChecked;
    std::unordered_set<std::string> saveWaiting;
    std::unordered_set<std::string> saveKnownPaths;
    uint64_t saveStartedSequence = 0;
    bool saveHadFailure = false;
    std::string saveMessage;
    // 视口变换工具与坐标系只驱动高亮，不提供 gizmo；对象变换从 Details 发命令，
    // 切换它们不提交 UiAction 或资产命令（见 ViewportToolbar 相关注释与文档）。
    std::string viewportTransformTool = "select";
    std::string viewportTransformSpace = "world";
    bool showActorVisualization = true;
    std::string resourceTransformTool = "select";
    std::string resourceTransformSpace = "world";
    // File→Open Level 的条目地址按索引寻址，与 markup 同代重建，避免把资产路径写进 DOM 属性。
    std::vector<std::string> menuLevelPaths;
    std::shared_ptr<const EditorJsonAssetCatalogSnapshot> menuCatalog;
    uint64_t menuCatalogRevision = 0;
    bool menuCatalogReady = false;
    // 角落坐标轴指示器的已挂载形态签名（量化后的角度与标签偏移 + 退化标志）。
    // 相机静止时签名不变，因此静止帧不写 DOM —— 与本文件其它"值缓存决定是否写"的做法一致。
    std::string axisIndicatorSignature;
    // 折叠的分组键（缺省即展开）；切换文档不清空，和模型树展开一样是面板本地状态。
    std::unordered_set<std::string> collapsedSections;
    // 大纲的展开与折叠同样是面板本地会话状态：折叠的分组键 + 展开的 actor 稳定身份。
    // 两者合并进 EditorSceneActorTreeExpansion，因为它就是行集合构建的入参。
    EditorSceneActorTreeExpansion outlinerExpansion;
    // 大纲搜索与类型过滤只影响可见性；ASCII 大小写折叠复用 Content Browser 的规则。
    std::string outlinerQuery;
    std::string outlinerTypeFilter = "all";
    // 全局名称排序是面板偏好，不随 World 换代重置。
    SceneActorSortOrder outlinerSortOrder = SceneActorSortOrder::NameAscending;
    // 行集合的已构建副本：只有内容签名或筛选/排序/展开状态变化时才重建，
    // 静止帧不重新排序、不重新分配行标签。
    std::vector<EditorSceneActorRow> outlinerRows;
    SceneActorSortOrder outlinerBuiltSortOrder = SceneActorSortOrder::NameAscending;
    EditorSceneActorTreeExpansion outlinerBuiltExpansion;
    std::string outlinerBuiltQuery;
    std::string outlinerBuiltTypeFilter = "all";
    uint64_t outlinerRowSignature = 0;
    uint64_t outlinerRowsRevision = 0;
    // 已挂载的窗口与"已挂载行的 (选择, 可见性)"签名：用来判断是否需要重挂 DOM
    // （而不是逐帧重写 markup）。outlinerMounted 让 Detach 之后重新 Attach 的第一帧
    // 一定重挂一次。
    EditorSceneActorWindow outlinerMountedWindow;
    bool outlinerMounted = false;
    uint64_t outlinerMountedRevision = 0;
    std::string outlinerMountedVisuals;
    // 行高由已挂载行的 margin box 实测；首帧保持 RCSS 兜底值。
    float outlinerRowHeight = SceneActorRowHeightFallback;
    uint64_t treeWorldGeneration = 0;
    std::unordered_map<std::string, EditorParameterValue> pendingValues;
    std::vector<PendingEdit> pendingEdits;
    std::string pendingAssetPath;
    uint64_t pendingRevision = 0;
    std::string detailsIdentity;
    std::string actorDetailsIdentity;
    bool activateMaterialDocument = false;
    std::string localStatus;
    std::string pendingClosePath;
    std::string closingMaterialPath;
    std::string panelAfterMaterialClose;
    bool saveThenClose = false;
    bool saveCloseSubmitted = false;
    bool refreshingControls = false;
};

} // namespace VL::EditorUi
