#pragma once

#include <filesystem>
#include <string>
#include <unordered_map>
#include <vector>

#include "ui/uiRenderSnapshot.h"
#include "ui/widgets/workspace/dockWorkspace.h"
#include "ui/widgets/widgetDocument.h"

namespace Rml
{
class Element;
class ElementDocument;
class Event;
}

namespace VL::EditorUi
{

struct EditorDockResourceTab
{
    std::string panel;
    std::string path;
    std::string label;
    std::string icon;
    int documentIndex = -1;
};

bool ReadWorkspaceLayout(const std::filesystem::path& path,
    UiWidgets::DockLayoutState& output, std::string& error,
    std::vector<std::string>* hidden = nullptr);
bool WriteWorkspaceLayout(const std::filesystem::path& path,
    const UiWidgets::DockLayoutState& state, std::string& error,
    const std::vector<std::string>* hidden = nullptr);

// 文档由 panel 借用；仅停靠壳可以重建，业务 DOM 和 tabs 始终 RemoveChild/AppendChild 搬移。
class EditorDockingController
{
public:
    EditorDockingController();
    void SetPersistencePath(std::filesystem::path path);
    bool Attach(Rml::ElementDocument& document, std::string& error);
    void Detach();
    void Sync();
    bool ProcessEvent(Rml::Event& event, bool modal);
    void CancelDrag();
    void SetResourceTabs(const std::vector<EditorDockResourceTab>& tabs);
    std::string ConsumeActivatedPanel();
    bool IsDragging() const { return !dragPanel.empty() || dragSplit != 0; }
    bool IsPanelActive(const std::string& panel) const
    {
        const uint32_t stack = workspace.FindStack(panel);
        return stack != 0 && IsPanelVisible(panel) &&
            workspace.State().nodes.at(stack).active == panel;
    }
    // 可见性由停靠视图拥有：DockWorkspace 只保留布局树，隐藏面板仍占树中的位置。
    bool IsPanelVisible(const std::string& panel) const;
    bool SetPanelVisible(const std::string& panel, bool visible);
    bool RestoreSavedLayout();
    const std::vector<std::string>& HiddenPanels() const { return hiddenPanels; }
    UiViewportRect GetSceneViewportRect() const;
    bool IsSceneViewportElement(const Rml::Element* element) const;
    const UiWidgets::DockWorkspace& Workspace() const { return workspace; }
    const std::string& Notice() const { return notice; }
    size_t PropertyWriteCount() const { return widgets.PropertyWriteCount(); }
    void Activate(const std::string& panel);
    std::string ResourcePathForPanel(const std::string& panel) const;
    void Reset();

private:
    void RebuildShell();
    void ApplyGeometry();
    void ApplyTabs();
    void Save();
    void UpdateDrag(float x, float y);
    void FinishDrag(float x, float y);
    void FindDrop(float x, float y);
    UiWidgets::DockRect Bounds() const;
    void MoveElement(Rml::Element& element, Rml::Element& parent);
    void Place(Rml::Element& element, UiWidgets::DockRect rect);
    bool LoadLayoutFromDisk();
    void HandOffActivation(uint32_t stack, const std::string& hidden);
    void PruneHiddenPanels();
    size_t VisiblePanelCount() const;
    std::vector<uint32_t> CollapsedStacks() const;

    Rml::ElementDocument* document = nullptr;
    Rml::Element* restoreFocus = nullptr;
    UiWidgets::DockWorkspace workspace;
    UiWidgets::DockGeometry geometry;
    UiWidgets::WidgetDocument widgets;
    std::filesystem::path persistencePath;
    std::string notice;
    // 隐藏集合按名字排序保存，写入文件时顺序稳定；它是视图状态，不进 DockWorkspace。
    std::vector<std::string> hiddenPanels;
    bool loaded = false;
    bool structureDirty = true;
    bool savePending = false;
    bool suppressClick = false;
    std::string activatedPanel;
    std::string pendingPanel;
    std::string dragPanel;
    uint32_t dragSplit = 0;
    uint32_t dropTarget = 0;
    UiWidgets::DockEdge dropEdge = UiWidgets::DockEdge::Center;
    UiWidgets::DockRect dragOriginal;
    UiWidgets::DockRect splitParent;
    float startX = 0;
    float startY = 0;
    std::vector<EditorDockResourceTab> resourceTabs;
};

} // namespace VL::EditorUi
