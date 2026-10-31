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
#include "editor/ui/tressfxGroupEditor.h"
#include "hair/tressfxGroupPaint.h"
#include "hair/tressfxSurfaceBrush.h"
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
    // 模型预览场景自己的 wind actor；它不是关卡风，也不写回任何资产。
    const EditorSceneActorSnapshot* PreviewWindActor() const;
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
    // Groups 编辑器与 Simulation 一样只改冻结的 TF 草稿快照；保存仍走
    // ModifyTressfxDocument + Save Content，不直接写资产或 World。
    void RefreshHairGroupsSection(bool isHair, bool hairBlocked);
    void SubmitHairGroupsSave();
    void SelectHairGroup(int index);
    // 预览状态只发值语义；generation 不可用时不投递，且同一组状态只发一次。
    void SubmitTressfxGroupPreview();
    // 草稿 binding 同步：只在草稿结构 revision 或预览 generation 变化时发送，
    // 让新建/重命名/删除组无需保存重载即可被 GPU 着色、选中和笔刷使用。
    void SubmitTressfxGroupBindingPreview();
    std::shared_ptr<const VL::Hair::TressfxGroupBinding> EnsureHairDraftBinding();
    bool HairDraftBindingReady() const;
    // Phase 3 分组笔刷：屏幕只生成 ray；SurfaceBrush/RootCloudSelector 决定命中。
    // ghost 只通过 SetTressfxBrushPreview 值语义命令进入渲染线程。
    const EditorSceneMeshSectionSnapshot* HairBrushSection() const;
    // 草稿组号 -> 已保存资产的 GPU 组号（0=Default，k=第 k 个已保存命名组）。
    // 草稿新增/删除组后不能直接用草稿下标；找不到返回 -1。
    int32_t SavedHairGroupIndex(const EditorSceneMeshSectionSnapshot* section,
        int draftIndex) const;
    bool HairBrushReady() const;
    // 同一个预览 generation + binding 只建立一次共享会话；hover/笔画不再复制
    // 40k guide 映射。binding 变化（保存后重载/切预览）时自动重建。
    bool EnsureHairPaintSession(const EditorSceneMeshSectionSnapshot* section);
    void ResetHairPaintSession();
    void ToggleHairGroupPaint(bool active);
    // 视口几何输入（viewport/camera/settings）只构造一次；圆形笔刷与套索共用同一套
    // NDC/FOV 约定，避免它们的平面落点不一致。
    bool PrepareHairBrushInputs(VL::Hair::BrushViewport& viewport,
        VL::Hair::BrushCamera& camera, VL::Hair::BrushSettings& settings) const;
    // 只更新 stamp/光标，不做圆形命中；套索与 hover 都需要一个稳定的 brush plane。
    bool SampleHairBrushStamp(float screenX, float screenY);
    void UpdateHairBrushSample(float screenX, float screenY, bool applyToStroke);
    void BeginHairBrushStroke(float screenX, float screenY, bool shift, bool alt);
    // 套索在手势期间自行累积命中，提交时不能再按当前光标补一个圆形 stamp。
    void BeginHairLassoStroke(float screenX, float screenY, bool shift, bool alt);
    void AppendHairLassoPoint(float screenX, float screenY);
    // 拖动期间按当前多边形重建 ghost；每 3 个点更新一次，避免整屏套索下每次鼠标移动
    // 都做全量 guide 选择。松开时仍以最终多边形重新计算一次。
    void UpdateHairLassoPreview();
    void CommitHairLassoStroke();
    void CommitHairBrushStroke(bool sampleCurrent = true);
    void CancelHairBrushStroke(bool exitPaint);
    void SubmitTressfxBrushPreview();
    void UpdateHairBrushOverlay();
    void UndoLastHairBrushCommit();
    std::string HairBrushConflictText(
        const std::vector<VL::Hair::GroupPaintConflict>& conflicts) const;
    std::string hairSimulationPath;
    nlohmann::json hairSimulationValues = nlohmann::json::object();
    // 上次从文档同步的 simulationDefaults；用于区分“另一个区域保存导致 revision 前进”
    // 与真正的外部修改，避免 Groups 保存时丢掉未保存的 Simulation 草稿。
    nlohmann::json hairSimulationObservedDefaults = nullptr;
    uint64_t hairSimulationBoundRevision = 0;
    uint64_t hairSimulationSaveRevision = 0;
    bool hairSimulationRequested = false;
    bool hairSimulationPendingSave = false;
    int hairSimulationSaveWaitTicks = 0;
    std::string hairSimulationStatus;
    // Preview Wind 的会话值。滑块拖动时 World/actor 快照会晚一帧到达；如果每帧
    // 都从快照回写滑块，滑块会被旧值拉回去。这里保存面板的即时值，预览 World
    // generation 变化（重新加载预览）时才重新绑定作者值。
    EditorSceneWorldWindActor hairPreviewWind;
    std::string hairPreviewWindPath;
    uint64_t hairPreviewWindGeneration = 0;
    bool hairPreviewWindBound = false;
    // Groups 的草稿状态完全属于面板：guideCount 只用于 range 上界校验，
    // 选中索引 -1 表示隐式 Default 组 0（不可编辑、不可作为笔刷目标）。
    VL::Editor::TressfxGroupEditor hairGroups;
    std::string hairGroupsPath;
    // 只有成功 Load 的控制器才允许保存；加载失败时保留原始 groups 不动，
    // 绝不能把空控制器写回并删除用户的组。
    bool hairGroupsLoaded = false;
    // 上次成功同步的文档 groups；revision 前进但该节未变时保留本地草稿。
    nlohmann::json hairGroupsObservedDocument = nullptr;
    uint64_t hairGroupsBoundRevision = 0;
    uint32_t hairGroupsBoundGuideCount = 0;
    int hairGroupSelected = -1;
    // Group Colors / Solo 是会话预览状态，不写资产。默认开着颜色，
    // 让 TF 预览能直接看出分组；关卡头发仍由 World 默认 flags=0 保持原外观。
    bool hairGroupColors = true;
    bool hairGroupSolo = false;
    bool hairGroupPreviewSent = false;
    bool hairGroupPaintActive = false;
    float hairBrushRadiusPx = 20.0f;
    VL::Hair::GroupPaintMode hairBrushMode = VL::Hair::GroupPaintMode::Add;
    bool hairBrushReplaceExisting = false;
    bool hairBrushStrokeActive = false;
    VL::Hair::GroupPaintMode hairBrushStrokeMode = VL::Hair::GroupPaintMode::Add;
    VL::Hair::BrushStamp hairBrushStamp;
    std::vector<VL::Hair::RootHit> hairBrushHits;
    std::vector<VL::Hair::GroupPaintConflict> hairBrushConflicts;
    VL::Hair::GroupPaintController hairPaint;
    uint64_t hairPaintSessionGeneration = 0;
    std::shared_ptr<const VL::Hair::TressfxGroupBinding> hairPaintSessionBinding;
    // 笔刷冲突/Replace existing 必须针对当前 Groups 草稿，而不是已保存资产；
    // 草稿结构变化时重建这份 binding（O(40k)，只在编辑时发生）。
    std::shared_ptr<const VL::Hair::TressfxGroupBinding> hairPaintDraftBinding;
    uint64_t hairPaintDraftRevision = 0;
    uint64_t hairGroupsDraftRevision = 0;
    std::string hairGroupsDraftSignature;
    // 草稿 binding 已投递到的预览身份。Ready 只表示本 revision/generation
    // 的队列命令已发出；实际 buffer 更新仍在 RT。容量不足由运行时拒绝并打印。
    bool hairGroupBindingPreviewSent = false;
    // 入队当帧仍视为“同步中”；下一次 Refresh 没有新 revision 才允许 Paint。
    bool hairGroupBindingPreviewQueued = false;
    uint64_t hairGroupBindingPreviewSentRevision = 0;
    uint64_t hairGroupBindingPreviewSentGeneration = 0;
    std::string hairGroupBindingPreviewSentObject;
    std::string hairGroupBindingPreviewError;
    // 只保存本笔刷会话的提交前整份 Groups 草稿；不是编辑器全局 undo。
    std::vector<VL::Editor::TressfxGroupEditor> hairPaintUndo;
    std::string hairBrushStatus;
    float hairBrushCursorX = 0.0f;
    float hairBrushCursorY = 0.0f;
    // Lasso 是 Paint 区内的独立工具；手势点只保留屏幕坐标，平面投影在选择器里完成。
    bool hairLassoActive = false;
    bool hairLassoDragging = false;
    std::vector<Eigen::Vector2f> hairLassoScreenPoints;
    bool hairBrushPreviewActive = false;
    // 离开预览时 section 已不可用，必须用最后一次对象名显式清 ghost。
    std::string hairBrushLastObjectName;
    uint64_t hairBrushLastGeneration = 0;
    // 世界圆环的独立去重：ghost 权重不变时，鼠标移动仍必须更新圆环。
    bool hairBrushRingSent = false;
    bool hairBrushRingSentVisible = false;
    Eigen::Vector3f hairBrushRingSentCenter = Eigen::Vector3f::Zero();
    Eigen::Vector3f hairBrushRingSentNormal = Eigen::Vector3f::UnitY();
    float hairBrushRingSentRadius = 0.0f;
    uint64_t hairGroupPreviewSentGeneration = 0;
    int32_t hairGroupPreviewSentSelected = -2;
    uint32_t hairGroupPreviewSentFlags = 0;
    bool hairGroupCreateActive = false;
    std::string hairGroupCreateName;
    std::string hairGroupCreateStart;
    std::string hairGroupCreateEnd;
    std::string hairGroupCreateError;
    bool hairGroupAddRangeActive = false;
    std::string hairGroupRangeStart;
    std::string hairGroupRangeEnd;
    std::string hairGroupRangeError;
    std::string hairGroupStatus;
    bool hairGroupsPendingSave = false;
    uint64_t hairGroupsSaveRevision = 0;
    int hairGroupsSaveWaitTicks = 0;
    bool hairGroupsPendingReset = false;
    nlohmann::json hairGroupsPendingValue = nlohmann::json();

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
