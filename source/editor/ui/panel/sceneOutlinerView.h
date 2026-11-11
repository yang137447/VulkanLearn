#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

#include "editor/ui/editorSceneActorTree.h"
#include "editor/ui/materialInstanceEditorView.h"
#include "ui/uiAction.h"
#include "ui/widgets/widgetDocument.h"

namespace Rml
{
class ElementDocument;
}

namespace VL::EditorUi
{
namespace Panel
{

// 大纲状态仍由面板持有；本模块只借用它们的引用，不复制任何缓存。
struct OutlinerViewContext
{
    Rml::ElementDocument** document = nullptr;
    UiWidgets::WidgetDocument* widgetDocument = nullptr;
    const MaterialInstanceEditorSnapshot* snapshot = nullptr;
    EditorSceneActorTreeExpansion* expansion = nullptr;
    std::string* query = nullptr;
    std::string* typeFilter = nullptr;
    SceneActorSortOrder* sortOrder = nullptr;
    std::vector<EditorSceneActorRow>* rows = nullptr;
    SceneActorSortOrder* builtSortOrder = nullptr;
    EditorSceneActorTreeExpansion* builtExpansion = nullptr;
    std::string* builtQuery = nullptr;
    std::string* builtTypeFilter = nullptr;
    uint64_t* rowSignature = nullptr;
    uint64_t* rowsRevision = nullptr;
    EditorSceneActorWindow* mountedWindow = nullptr;
    bool* mounted = nullptr;
    uint64_t* mountedRevision = nullptr;
    std::string* mountedVisuals = nullptr;
    float* rowHeight = nullptr;
    uint64_t* treeWorldGeneration = nullptr;
    bool showActorVisualization = false;
    std::function<void()> closeContextMenu;
};

void RefreshOutlinerView(const OutlinerViewContext& context);
void RefreshOutlinerVisuals(const OutlinerViewContext& context);
void RefreshActorVisualization(const OutlinerViewContext& context);
void RebuildOutlinerRows(const OutlinerViewContext& context,
    const std::vector<EditorSceneActorSnapshot>& actors);
bool OutlinerRowsNeedRebuild(const OutlinerViewContext& context,
    const std::vector<EditorSceneActorSnapshot>& actors);
EditorSceneActorWindow OutlinerWindow(const OutlinerViewContext& context);
void MeasureOutlinerRowHeight(const OutlinerViewContext& context);
std::string OutlinerMountedVisuals(const OutlinerViewContext& context);

const std::vector<EditorSceneActorSnapshot>& OutlinerActors(
    const MaterialInstanceEditorSnapshot& snapshot);
const EditorSceneActorSnapshot* FindSelectedActor(const MaterialInstanceEditorSnapshot& snapshot);
const EditorSceneActorSnapshot* FindPreviewWindActor(const MaterialInstanceEditorSnapshot& snapshot);
const EditorSceneActorSnapshot* FindContextMenuActor(const std::string& target,
    const MaterialInstanceEditorSnapshot& snapshot);

// 右键菜单目标在打开时解析成稳定地址；动作执行时重新查目标，查不到就拒绝执行。
void ToggleContextActorVisibility(const std::string& target,
    const MaterialInstanceEditorSnapshot& snapshot,
    const std::function<void(UiAction)>& requestRuntimeAction,
    std::string& localStatus);
void OpenContextActorMaterial(const std::string& target, size_t slotIndex,
    const MaterialInstanceEditorSnapshot& snapshot,
    const std::function<void(const MaterialInstanceModelSnapshot&, size_t)>& selectModelMaterial,
    std::string& localStatus);
void OpenContextBrowserAsset(const std::string& target,
    const MaterialInstanceEditorSnapshot& snapshot,
    const std::function<void(const std::string&)>& openBrowserAsset,
    std::string& localStatus);

} // namespace Panel
} // namespace VL::EditorUi
