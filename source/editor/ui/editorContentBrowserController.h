#pragma once

#include <map>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <unordered_map>
#include <vector>

#include "editor/assets/editorJsonAssetTypes.h"
#include "ui/widgets/widgetDocument.h"

namespace Rml
{
class Element;
class ElementDocument;
class Event;
}

namespace VL::EditorUi
{
struct MaterialInstanceEditorSnapshot;

// 从只读目录快照派生浏览状态；文档由 panel 借用，打开路径交回 panel 的既有命令。
// 不扫描文件、不修改资产/World、不接触 Vulkan；Attach/Detach 不丢失本地浏览状态。
class EditorContentBrowserController
{
public:
    // 单调计数记录真正发生的工作，不用耗时断言；visibleItems 只统计窗口内资产/目录节点。
    struct Stats
    {
        size_t catalogRebuilds = 0;
        size_t entryRebuilds = 0;
        size_t entriesMarkupBuilds = 0;
        size_t listDomRebuilds = 0;
        size_t sourcesMarkupBuilds = 0;
        size_t breadcrumbMarkupBuilds = 0;
        size_t matchedItems = 0;
        size_t visibleItems = 0;
    };

    bool Attach(Rml::ElementDocument& document, std::string& error);
    void Detach();
    void Build(const MaterialInstanceEditorSnapshot& snapshot);
    bool ProcessEvent(Rml::Event& event, std::optional<std::string>& openAssetPath);
    const Stats& GetStats() const { return stats; }

private:
    struct Entry
    {
        std::string path;
        std::string name;
        std::string foldedName;
        std::string logicalPath;
        std::string foldedPath;
        EditorAssetKind kind = EditorAssetKind::MaterialInstance;
        bool folder = false;
    };
    struct Folder
    {
        Entry entry;
        std::vector<const Entry*> children;
    };
    struct EntryOrder
    {
        bool descending = false;
        bool operator()(const Entry* first, const Entry* second) const;
    };
    struct Window
    {
        size_t begin = 0;
        size_t end = 0;
        size_t columns = 1;
        float rowHeight = 30.0f;
        float top = 0.0f;
        float bottom = 0.0f;
    };
    struct VisibleNode
    {
        Rml::Element* element = nullptr;
        const Entry* entry = nullptr;
        bool dirty = false;
    };

    bool ReadCatalog(const MaterialInstanceEditorSnapshot& snapshot);
    bool ReadDirtyOverlay(const MaterialInstanceEditorSnapshot& snapshot, bool catalogChanged);
    void ReconcileNavigation();
    void ExpandAncestors(const std::string& path);
    void Navigate(const std::string& path);
    void NavigationChanged();
    void FilterChanged();
    void BuildEntries();
    Window VisibleWindow() const;
    void BindVisibleNodes(bool newDom);
    void SyncVisibleState();
    std::string SourcesMarkup() const;
    std::string BreadcrumbMarkup();
    std::string EntriesMarkup(const Window& window) const;
    void SyncControls();

    Rml::ElementDocument* document = nullptr;
    UiWidgets::WidgetDocument widgets;
    bool refreshingControls = false;
    std::string folder;
    std::string query;
    std::string typeFilter = "all";
    std::string selectedPath;
    bool selectedFolder = false;
    bool recursive = true;
    bool grid = true;
    bool descending = false;
    bool sourcesVisible = true;
    std::string tileSize = "small";
    std::vector<std::string> history{""};
    size_t historyIndex = 0;
    std::set<std::string> expanded{""};
    std::map<std::string, Folder> folders;
    std::unordered_map<std::string, Entry> catalog;
    std::vector<const Entry*> entries;
    // 强持有不可变快照，防止地址复用；旧 MI fixture 按路径序列比较，dirty 不触发重建。
    std::shared_ptr<const EditorJsonAssetCatalogSnapshot> catalogSnapshot;
    uint64_t catalogRevision = 0;
    bool catalogInitialized = false;
    std::vector<std::string> fallbackPaths;
    std::vector<std::pair<std::string, bool>> dirtyOverlay;
    std::set<std::string> dirtyPaths;
    // 事件绑定的是当前 DOM 的原始完整地址，不按虚拟窗口索引回收/改写节点。
    std::vector<VisibleNode> visibleNodes;
    std::set<std::string> currentBreadcrumbs;
    Window visibleWindow;
    std::string visibleMarkup;
    bool entriesDirty = true;
    bool sourcesDirty = true;
    bool breadcrumbsDirty = true;
    bool controlsDirty = true;
    bool presentationDirty = true;
    bool selectionDirty = true;
    bool resetScroll = true;
    bool clickArmed = false;
    std::string clickedPath;
    size_t clickedDomGeneration = 0;
    float clickedScrollTop = 0.0f;
    Stats stats;
};
} // namespace VL::EditorUi
