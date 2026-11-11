#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "editor/ui/editorSceneActorTree.h"
#include "editor/ui/materialInstanceEditorView.h"
#include "script/scriptTypes.h"
#include "terrain/terrainAssetTypes.h"
#include "ui/widgets/views/viewWidgets.h"

namespace VL::EditorUi
{
class EditorDockingController;

namespace Panel
{

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

// 文档命令的可用性统一作用在所有发出该命令的控件上，避免出现两套互相矛盾的启用状态。
// 菜单和中央材质页使用同一份文档命令可用性。
inline constexpr const char* DocumentCommandIds[] = {
    "menu-file-save", "menu-file-revert", "menu-file-reload",
    "menu-edit-reset", "menu-edit-validate", "material-save"};

std::string Number(float value);
std::string TerrainNumber(float value);
std::string TerrainLayerRange(float minimum, float maximum);
std::string TerrainLayerColor(const TerrainLayerDesc& layer);
std::string TerrainLayerSourceLabel(uint32_t sourceSet);
bool TerrainLayerMatchesSearch(const TerrainLayerDesc& layer, const std::string& search);
std::string TerrainLayerCardMarkup(const TerrainLayerDesc& layer, const std::string& color,
    const std::string& spherePath, size_t index, bool selected, bool listView);
int TerrainLayerGridColumns(float viewportWidth);

// 窗口计算必须与 RCSS 的固定卡片盒一致：144px 卡宽 + 6px 右间距 = 150px 单元格，
// 116px 卡高 + 6px 下间距 = 122px 行高；列表行高为 34px 卡 + 4px 间距 = 38px。
inline constexpr float TerrainLayerGridCellWidth = 150.0f;
inline constexpr float TerrainLayerGridRowHeight = 122.0f;
inline constexpr float TerrainLayerListRowHeight = 38.0f;
inline constexpr float TerrainLayerFallbackViewportWidth = 320.0f;
inline constexpr float TerrainLayerFallbackViewportHeight = 180.0f;
inline constexpr size_t TerrainLayerVirtualizeThreshold = 12;

std::string FileLabel(const std::string& path);

std::string ActorPropertyRowMarkup(const EditorSceneActorEditableField& field);
std::string ActorCountLabel(size_t count);

// 树行只携带展示数据与动作地址；动作索引是 sceneActors 的下标，过滤只改变可见性。
UiWidgets::TreeRowData OutlinerActorRow(size_t actorIndex, const EditorSceneActorSnapshot& actor,
    bool selected, bool expanded, bool hasChildren);
UiWidgets::TreeRowData OutlinerSlotRow(size_t actorIndex, size_t slotIndex,
    const std::string& label, bool selected);

// 只把窗口内的行挂进 DOM，上下各一个 spacer 保留未挂载行的高度。
std::string OutlinerRowsMarkup(const std::vector<EditorSceneActorRow>& rows,
    const std::vector<EditorSceneActorSnapshot>& actors,
    const std::string& selectedActorIdentity,
    std::optional<uint32_t> selectedMaterialSlotIndex,
    const EditorSceneActorWindow& window);

const char* DocumentStatus(EditorDocumentStatus status);
const char* PreviewStatus(EditorPreviewStatus status);
const char* PanelLabel(const std::string& panel);
std::string ResourcePanelId(const std::string& path);
std::string MaterialPanelId(const std::string& path);
std::string RenderStateOptions(const std::string& name);

// Window 菜单沿用停靠标签的显示名；面板集合本身仍由 DockWorkspace::Panels() 定义。
std::string BuildWindowMenuMarkup(const EditorDockingController& docking);

struct ContextMenuMarkupInput
{
    ContextMenuKind kind = ContextMenuKind::None;
    const EditorSceneActorSnapshot* actor = nullptr;
    const std::string* target = nullptr;
    const std::vector<EditorModifiedDocumentSnapshot>* modifiedDocuments = nullptr;
    bool scriptsAvailable = false;
    ScriptPlayState scriptState = ScriptPlayState::Editing;
    int debugViewMode = 0;
};

// 条目在打开时生成一次：菜单存活期间快照可能换代，因此动作执行时一律按稳定地址
// 重新查目标，查不到就给出诊断，而不是退回面板的当前选择。
std::string ContextMenuItemsMarkup(const ContextMenuMarkupInput& input);

// 只读目录（缺目录时退回旧 MI 资产列表）里查路径：既是"资产还是目录"的判据，
// 也是菜单标题的名字来源。
bool FindCatalogAssetKind(const MaterialInstanceEditorSnapshot& snapshot, const std::string& path,
    EditorAssetKind& kind, std::string& name);

} // namespace Panel
} // namespace VL::EditorUi
