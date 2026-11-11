#pragma once

#include <functional>
#include <string>
#include <vector>

#include <RmlUi/Core/EventListener.h>

#include "editor/ui/materialInstanceEditorView.h"
#include "editor/ui/panel/actorMarkup.h"
#include "ui/uiRenderSnapshot.h"
#include "ui/widgets/widgetDocument.h"

namespace Rml
{
class ElementDocument;
}

namespace VL::UiWidgets
{
class BrushLibraryPanel;
}

namespace VL::EditorUi
{
class EditorContentBrowserController;
class EditorDockingController;
class EditorJsonInspectorController;
class EditorLogPanel;
class EditorPerformancePanel;
class RetargetAssetEditorPanel;
class TerrainAssetEditorController;

namespace Panel
{

// 面板生命周期需要跨多个编辑器控制器，本上下文只借用面板成员的引用，
// 不复制任何状态；控制器与回调都指向面板自己的权威实例。
struct EditorPanelBootstrapContext
{
    Rml::ElementDocument** document = nullptr;
    Rml::EventListener* listener = nullptr;
    EditorDockingController* docking = nullptr;
    UiWidgets::WidgetDocument* widgetDocument = nullptr;
    UiWidgets::BrushLibraryPanel* brushLibrary = nullptr;
    RetargetAssetEditorPanel* retargetEditor = nullptr;
    EditorContentBrowserController* contentBrowser = nullptr;
    EditorJsonInspectorController* jsonInspector = nullptr;
    EditorJsonInspectorController* resourceJsonInspector = nullptr;
    TerrainAssetEditorController* terrainAssetEditor = nullptr;
    IEditorCommandSink* commandSink = nullptr;
    EditorLogPanel* logPanel = nullptr;
    EditorPerformancePanel* performancePanel = nullptr;
    std::string* detailsIdentity = nullptr;
    std::string* actorDetailsIdentity = nullptr;
    std::string* terrainSummarySignature = nullptr;
    std::string* blueprintAssetIdentity = nullptr;
    const UiViewModelSnapshot* runtimeView = nullptr;
    std::function<void()> refreshWorkspace;
    std::function<void()> refreshLists;
    std::function<void()> refreshDetails;
    std::function<void(const UiViewModelSnapshot&)> updateRuntimeView;

    // 仅 Detach 使用：取消进行中的手势、清理会话状态。
    std::function<void()> cancelBlueprintViewportDrag;
    std::function<void()> cancelSceneGizmoDrag;
    std::function<void()> cancelTerrainBrushStroke;
    std::function<void()> clearTerrainBrushHit;
    std::function<void()> resetTextureView;
    std::function<void()> resetTerrainLayerWindow;
    bool* landscapeToolActive = nullptr;
    std::string* actorDragPath = nullptr;
    std::string* textureViewSource = nullptr;
    int* previewDragButton = nullptr;
    bool* runtimeControlsCurrent = nullptr;
    bool* motionMatchingRowsMounted = nullptr;
    bool* motionMatchingVisible = nullptr;
    bool* motionMatchingQueryExpanded = nullptr;
    std::vector<std::string>* motionMatchingQueryChannels = nullptr;
    double* motionMatchingRefreshTime = nullptr;
    std::string* motionMatchingRefreshKey = nullptr;
    bool* textureOptionsDirty = nullptr;
    std::vector<std::string>* textureOptionSelections = nullptr;
    int* brushLibraryCatalogDomain = nullptr;
    std::string* axisIndicatorSignature = nullptr;
    std::string* terrainLayerLibrarySignature = nullptr;
    std::string* terrainLayerSourceKey = nullptr;
    bool* terrainLayerWindowDirty = nullptr;
    bool* terrainLayerScrollResetPending = nullptr;
    float* terrainLayerLastScrollTop = nullptr;
    bool* outlinerMounted = nullptr;
    bool* sceneGizmoMarkupMounted = nullptr;
    ContextMenuKind* contextMenuKind = nullptr;
    std::string* contextMenuTarget = nullptr;
};

bool ValidateEditorPanelDocument(Rml::ElementDocument& candidate, std::string& error);
bool AttachEditorPanel(EditorPanelBootstrapContext& context, Rml::ElementDocument& candidate,
    std::string& error);
void DetachEditorPanel(EditorPanelBootstrapContext& context);

void SetPanelMarkup(UiWidgets::WidgetDocument& widgetDocument, const std::string& id,
    std::string markup);
void SetPanelControlValue(UiWidgets::WidgetDocument& widgetDocument, const std::string& id,
    const std::string& value);
void SetPanelDisabled(UiWidgets::WidgetDocument& widgetDocument, const std::string& id,
    bool disabled);

} // namespace Panel
} // namespace VL::EditorUi
