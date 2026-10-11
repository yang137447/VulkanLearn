#pragma once

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
#include "editor/ui/editorSceneActorTree.h"
#include "ui/uiAction.h"
#include "ui/uiRenderSnapshot.h"
#include "ui/widgets/widgetDocument.h"

namespace Rml
{
class Element;
class ElementDocument;
}

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
    void ShowMaterialDetails() { viewingJson = false; resourceEditorVisible = false; requestedJsonPath.clear(); }
    void Build(const MaterialInstanceEditorSnapshot& snapshot);
    void UpdateRuntimeView(const UiViewModelSnapshot& view);
    void RequestSave();
    void SetWorkspaceLayoutPath(std::filesystem::path path) { docking.SetPersistencePath(std::move(path)); }
    UiViewportRect GetSceneViewportRect() const { return docking.GetSceneViewportRect(); }
    bool IsSceneViewportElement(const Rml::Element* element) const { return docking.IsSceneViewportElement(element); }
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
    bool IsResourceEditorVisible() const
    {
        return resourceEditorVisible || docking.IsPanelActive("resource");
    }
    bool IsWorkspaceDragging() const { return docking.IsDragging(); }
    size_t WorkspacePropertyWriteCount() const { return docking.PropertyWriteCount(); }
    size_t PanelPropertyWriteCount() const { return widgetDocument.PropertyWriteCount(); }
    void CancelWorkspaceDrag() { docking.CancelDrag(); }
    bool HasModal() const { return !pendingClosePath.empty() || aboutVisible; }
    void ProcessEvent(Rml::Event& event) override;

private:
    // 上下文菜单的目标类别。目标只用稳定地址表达（actor 稳定身份 / 资产完整路径）：
    // 菜单条目要到下一次点击才执行，而快照下标只在当帧有效，用下标会指到另一个对象。
    enum class ContextMenuKind
    {
        None,
        Actor,
        BrowserAsset,
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
    void RefreshActorValues(const EditorSceneActorSnapshot* actor);
    std::string ActorSectionMarkup(const EditorSceneActorSnapshot& actor);
    std::string PrimaryDirectionalLightControls(const EditorSceneActorSnapshot& actor) const;
    const std::vector<EditorSceneActorSnapshot>& OutlinerActors() const;
    const EditorSceneActorSnapshot* SelectedActor() const;
    void SelectOutlinerActor(const EditorSceneActorSnapshot& actor);
    void ToggleOutlinerGroup(size_t groupIndex);
    void RefreshWorkspace();
    void RefreshMenus();
    void RefreshLevelMenu();
    void RefreshViewportToolbar();
    void RefreshViewportAxisIndicator();
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
    void HandleChange(Rml::Element& element, const std::string& action);
    void CloseDocument(const std::string& path, EditorDirtyDocumentPolicy policy);
    void SelectModelMaterial(const MaterialInstanceModelSnapshot& model, size_t index);
    void OpenBrowserAsset(const std::string& path);
    void RefreshResourceEditor();
    void OpenResourceAsset(const std::string& path, EditorAssetKind kind);
    void CloseActiveResource();
    void RequestResourceReload();

    IEditorCommandSink* commandSink = nullptr;
    IMaterialInstanceSceneSelectionSink* sceneSelectionSink = nullptr;
    IEditorRuntimeActionSink* runtimeActionSink = nullptr;
    IEditorJsonAssetRequestSink* jsonAssetRequestSink = nullptr;
    Rml::ElementDocument* document = nullptr;
    MaterialInstanceEditorSnapshot snapshot;
    UiViewModelSnapshot runtimeView;
    UiWidgets::WidgetDocument widgetDocument;
    EditorDockingController docking;
    EditorContentBrowserController contentBrowser;
    EditorJsonInspectorController jsonInspector;
    EditorJsonInspectorController resourceJsonInspector;
    EditorResourceEditorController resourceEditor;
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
    // 视口工具条的变换工具与坐标系只驱动高亮；文件里没有 gizmo，也没有 transform 编辑后端，
    // 切换它们不提交 UiAction 或资产命令（见 ViewportToolbar 相关注释与文档）。
    std::string viewportTransformTool = "select";
    std::string viewportTransformSpace = "world";
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
    // 组内排序是面板偏好（与 Content Browser 的排序一样），不随 World 换代重置。
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
    std::string localStatus;
    std::string pendingClosePath;
    bool saveThenClose = false;
    bool saveCloseSubmitted = false;
    bool refreshingControls = false;
};

} // namespace VL::EditorUi
