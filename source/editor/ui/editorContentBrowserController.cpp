#include "editor/ui/editorContentBrowserController.h"

#include <algorithm>
#include <cmath>
#include <utility>

#include <RmlUi/Core/ElementDocument.h>
#include <RmlUi/Core/Elements/ElementFormControl.h>
#include <RmlUi/Core/Event.h>

#include "editor/ui/editorTextMatch.h"
#include "editor/ui/materialInstanceEditorView.h"
#include "ui/widgets/text/textWidgets.h"

namespace VL::EditorUi
{
namespace
{
constexpr size_t HistoryLimit = 128;
constexpr size_t OverscanRows = 2;
constexpr size_t MaxVisibleItems = 192;
const char* const RequiredIds[] = {
    "asset-search", "asset-list", "asset-sources", "asset-breadcrumbs", "asset-count",
    "asset-empty", "browser-selection", "browser-back", "browser-forward", "browser-up",
    "browser-sources-toggle", "browser-grid", "browser-list", "browser-recursive",
    "browser-sort", "browser-size", "browser-sources-panel", "browser-type", "browser-refresh"
};

std::string FoldCase(std::string text)
{
    // 折叠规则由 editorTextMatch.h 拥有，大纲搜索复用同一份实现。
    return FoldAsciiCase(std::move(text));
}

std::string LogicalPath(std::string path)
{
    std::replace(path.begin(), path.end(), '\\', '/');
    return path;
}

std::string Parent(const std::string& path)
{
    const auto separator = path.rfind('/');
    return separator == std::string::npos ? std::string() : path.substr(0, separator);
}

std::string Name(const std::string& path)
{
    const auto separator = path.rfind('/');
    return separator == std::string::npos ? path : path.substr(separator + 1);
}

std::string Stem(const std::string& path)
{
    const std::string name = Name(LogicalPath(path));
    const auto extension = name.rfind('.');
    return extension == std::string::npos ? name : name.substr(0, extension);
}

bool IsWithin(const std::string& path, const std::string& folder)
{
    return folder.empty() || (path.size() > folder.size() &&
        path.compare(0, folder.size(), folder) == 0 && path[folder.size()] == '/');
}

bool IsDescendantOf(const Rml::Element* element, const Rml::Element* parent)
{
    for (const auto* current = element; current != nullptr; current = current->GetParentNode())
        if (current == parent) return true;
    return false;
}

const char* KindFilter(EditorAssetKind kind)
{
    switch (kind)
    {
    case EditorAssetKind::MaterialInstance: return "material";
    case EditorAssetKind::Mesh: return "mesh";
    case EditorAssetKind::Scene: return "scene";
    case EditorAssetKind::Texture: return "texture";
    case EditorAssetKind::Terrain: return "terrain";
    }
    return "";
}

bool ValidFilter(const std::string& value)
{
    for (const char* filter : {"all", "material", "mesh", "scene", "texture", "terrain"})
        if (value == filter) return true;
    return false;
}

std::string Address(const char* action, const std::string& path)
{
    return " data-action=\"" + std::string(action) + "\" data-path=\"" + UiWidgets::Text(path) + "\"";
}
} // namespace

bool EditorContentBrowserController::EntryOrder::operator()(const Entry* first, const Entry* second) const
{
    if (first->folder != second->folder) return first->folder;
    int order = first->foldedName.compare(second->foldedName);
    if (order == 0) order = first->path.compare(second->path);
    return descending ? order > 0 : order < 0;
}

bool EditorContentBrowserController::Attach(Rml::ElementDocument& candidate, std::string& error)
{
    for (const char* id : RequiredIds)
    {
        if (candidate.GetElementById(id) == nullptr)
        {
            error = std::string("Content Browser is missing element: ") + id;
            return false;
        }
    }
    document = &candidate;
    widgets.Attach(candidate);
    visibleNodes.clear();
    visibleMarkup.clear();
    NavigationChanged();
    presentationDirty = selectionDirty = true;
    clickArmed = false;
    currentBreadcrumbs.clear();
    error.clear();
    return true;
}

void EditorContentBrowserController::Detach()
{
    widgets.Detach();
    document = nullptr;
    visibleNodes.clear();
    stats.visibleItems = 0;
    clickArmed = false;
    currentBreadcrumbs.clear();
}

bool EditorContentBrowserController::ReadCatalog(const MaterialInstanceEditorSnapshot& snapshot)
{
    bool unchanged = catalogInitialized && catalogSnapshot == snapshot.assetCatalog;
    if (snapshot.assetCatalog)
        unchanged = unchanged && catalogRevision == snapshot.assetCatalog->revision;
    else
    {
        unchanged = unchanged && fallbackPaths.size() == snapshot.assets.size();
        for (size_t index = 0; unchanged && index < fallbackPaths.size(); ++index)
            unchanged = fallbackPaths[index] == snapshot.assets[index].assetPath;
    }
    if (unchanged) return false;

    // 完整 catalog 是唯一资产集合；MI 快照不允许把目录外或已删除的资产混回浏览器。
    entries.clear();
    catalog.clear();
    folders.clear();
    catalogSnapshot = snapshot.assetCatalog;
    catalogRevision = catalogSnapshot ? catalogSnapshot->revision : 0;
    catalogInitialized = true;
    fallbackPaths.clear();
    if (catalogSnapshot)
    {
        catalog.reserve(catalogSnapshot->assets.size());
        for (const auto& asset : catalogSnapshot->assets)
        {
            Entry entry;
            entry.path = asset.assetPath;
            entry.name = asset.name.empty() ? Stem(entry.path) : asset.name;
            entry.foldedName = FoldCase(entry.name);
            entry.logicalPath = LogicalPath(entry.path);
            entry.foldedPath = FoldCase(entry.logicalPath);
            entry.kind = asset.kind;
            catalog.emplace(asset.assetPath, std::move(entry));
        }
    }
    else
    {
        catalog.reserve(snapshot.assets.size());
        for (const auto& asset : snapshot.assets)
        {
            fallbackPaths.push_back(asset.assetPath);
            Entry entry;
            entry.path = asset.assetPath;
            entry.name = Stem(entry.path);
            entry.foldedName = FoldCase(entry.name);
            entry.logicalPath = LogicalPath(entry.path);
            entry.foldedPath = FoldCase(entry.logicalPath);
            catalog.emplace(asset.assetPath, std::move(entry));
        }
    }
    folders.emplace("", Folder{});
    for (const auto& asset : catalog)
    {
        std::string ancestor = Parent(asset.second.logicalPath);
        while (!ancestor.empty())
        {
            const bool inserted = folders.emplace(ancestor, Folder{}).second;
            if (!inserted) break;
            ancestor = Parent(ancestor);
        }
    }
    for (auto& item : folders)
    {
        auto& entry = item.second.entry;
        entry.path = entry.logicalPath = item.first;
        entry.name = Name(item.first);
        entry.foldedName = FoldCase(entry.name);
        entry.foldedPath = FoldCase(entry.logicalPath);
        entry.folder = true;
        if (!item.first.empty()) folders.at(Parent(item.first)).children.push_back(&entry);
    }
    for (auto& item : folders)
        std::sort(item.second.children.begin(), item.second.children.end(), EntryOrder{});
    ++stats.catalogRebuilds;
    NavigationChanged();
    selectionDirty = true;
    return true;
}

bool EditorContentBrowserController::ReadDirtyOverlay(
    const MaterialInstanceEditorSnapshot& snapshot, bool catalogChanged)
{
    bool unchanged = !catalogChanged && dirtyOverlay.size() == snapshot.assets.size();
    for (size_t index = 0; unchanged && index < dirtyOverlay.size(); ++index)
        unchanged = dirtyOverlay[index].first == snapshot.assets[index].assetPath &&
            dirtyOverlay[index].second == snapshot.assets[index].dirty;
    if (unchanged) return false;
    dirtyOverlay.clear();
    dirtyOverlay.reserve(snapshot.assets.size());
    dirtyPaths.clear();
    for (const auto& asset : snapshot.assets)
    {
        dirtyOverlay.emplace_back(asset.assetPath, asset.dirty);
        const auto found = catalog.find(asset.assetPath);
        if (asset.dirty && found != catalog.end() &&
            found->second.kind == EditorAssetKind::MaterialInstance)
            dirtyPaths.insert(asset.assetPath);
    }
    return true;
}

void EditorContentBrowserController::ReconcileNavigation()
{
    const std::string oldFolder = folder;
    while (folders.count(folder) == 0) folder = Parent(folder);
    // 服务移除目录时回到最近存在的祖先，历史只保留仍能导航的地址。
    history[historyIndex] = folder;
    std::vector<std::string> retained;
    size_t retainedIndex = 0;
    for (size_t index = 0; index < history.size(); ++index)
    {
        if (folders.count(history[index]) == 0) continue;
        if (retained.empty() || retained.back() != history[index]) retained.push_back(history[index]);
        if (index <= historyIndex) retainedIndex = retained.size() - 1;
    }
    history = std::move(retained);
    historyIndex = retainedIndex;
    if (oldFolder != folder) ExpandAncestors(folder);
    if ((!selectedFolder && catalog.count(selectedPath) == 0) ||
        (selectedFolder && folders.count(selectedPath) == 0))
    {
        selectedPath.clear();
        selectedFolder = false;
    }
    for (auto it = expanded.begin(); it != expanded.end();)
    {
        if (folders.count(*it) == 0) it = expanded.erase(it);
        else ++it;
    }
}

void EditorContentBrowserController::ExpandAncestors(const std::string& path)
{
    std::string ancestor = path;
    for (;;)
    {
        expanded.insert(ancestor);
        if (ancestor.empty()) break;
        ancestor = Parent(ancestor);
    }
}

void EditorContentBrowserController::Navigate(const std::string& path)
{
    if (folders.count(path) == 0 || folder == path) return;
    folder = path;
    ExpandAncestors(folder);
    history.resize(historyIndex + 1);
    history.push_back(folder);
    if (history.size() > HistoryLimit) history.erase(history.begin());
    historyIndex = history.size() - 1;
    NavigationChanged();
}

void EditorContentBrowserController::NavigationChanged()
{
    sourcesDirty = breadcrumbsDirty = controlsDirty = true;
    FilterChanged();
}

void EditorContentBrowserController::FilterChanged()
{
    entriesDirty = resetScroll = true;
}

void EditorContentBrowserController::BuildEntries()
{
    entries.clear();
    const std::string foldedQuery = FoldCase(query);
    if (!recursive)
    {
        for (const auto* child : folders.at(folder).children)
        {
            if (!foldedQuery.empty() && child->foldedPath.find(foldedQuery) == std::string::npos) continue;
            entries.push_back(child);
        }
    }
    for (const auto& asset : catalog)
    {
        const auto& entry = asset.second;
        const std::string& logical = entry.logicalPath;
        if (typeFilter != "all" && typeFilter != KindFilter(entry.kind)) continue;
        if (!IsWithin(logical, folder) || (!recursive && Parent(logical) != folder)) continue;
        if (!foldedQuery.empty() && entry.foldedPath.find(foldedQuery) == std::string::npos &&
            entry.foldedName.find(foldedQuery) == std::string::npos) continue;
        // 展示名仅用于排版；事件输出始终保留快照提供的完整资产地址。
        entries.push_back(&entry);
    }
    std::sort(entries.begin(), entries.end(), EntryOrder{descending});
    ++stats.entryRebuilds;
    stats.matchedItems = entries.size();
    size_t folderCount = 0;
    for (const auto* entry : entries) if (entry->folder) ++folderCount;
    const size_t assetCount = entries.size() - folderCount;
    std::string count = std::to_string(assetCount) + (assetCount == 1 ? " asset" : " assets");
    if (folderCount != 0)
        count = std::to_string(folderCount) + (folderCount == 1 ? " folder, " : " folders, ") + count;
    widgets.SetMarkup("asset-count", count);
}

std::string EditorContentBrowserController::SourcesMarkup() const
{
    std::string markup;
    std::vector<std::pair<std::string, size_t>> pending{{"", 0}};
    while (!pending.empty())
    {
        const auto row = std::move(pending.back());
        pending.pop_back();
        const auto& children = folders.at(row.first).children;
        const bool isExpanded = expanded.count(row.first) != 0;
        markup += "<div class=\"browser-folder-row";
        if (row.first == folder) markup += " selected";
        markup += "\" style=\"padding-left: " + std::to_string(row.second * 14) + "px;\">";
        if (!children.empty())
            markup += "<button class=\"browser-folder-toggle\"" + Address("browser-folder-expand", row.first) +
                " aria-expanded=\"" + (isExpanded ? "true" : "false") + "\">" +
                (isExpanded ? "-" : "+") + "</button>";
        else markup += "<span class=\"browser-folder-toggle\"></span>";
        markup += "<button class=\"browser-folder-name";
        if (row.first == folder) markup += " selected";
        markup += "\"" + Address("browser-folder", row.first) + " title=\"" + UiWidgets::Text(row.first) +
            "\"><img src=\"icons/folder.png\"/><span>" +
            UiWidgets::Text(row.first.empty() ? "Content" : Name(row.first)) + "</span></button></div>";
        if (isExpanded)
            for (auto it = children.rbegin(); it != children.rend(); ++it)
                pending.emplace_back((*it)->path, row.second + 1);
    }
    return markup;
}

std::string EditorContentBrowserController::BreadcrumbMarkup()
{
    currentBreadcrumbs.clear();
    currentBreadcrumbs.insert("");
    std::string markup = "<button class=\"browser-crumb\"" + Address("browser-breadcrumb", "") + ">Content</button>";
    size_t offset = 0;
    while (!folder.empty())
    {
        const auto separator = folder.find('/', offset);
        const std::string path = folder.substr(0, separator);
        currentBreadcrumbs.insert(path);
        markup += "<span class=\"browser-crumb-separator\">/</span><button class=\"browser-crumb\"" +
            Address("browser-breadcrumb", path) + ">" + UiWidgets::Text(Name(path)) + "</button>";
        if (separator == std::string::npos) break;
        offset = separator + 1;
    }
    return markup;
}

EditorContentBrowserController::Window EditorContentBrowserController::VisibleWindow() const
{
    Window result;
    auto* list = document->GetElementById("asset-list");
    const auto& box = list->GetBox();
    const float paddingX = box.GetEdge(Rml::BoxArea::Padding, Rml::BoxEdge::Left) +
        box.GetEdge(Rml::BoxArea::Padding, Rml::BoxEdge::Right);
    const float paddingY = box.GetEdge(Rml::BoxArea::Padding, Rml::BoxEdge::Top) +
        box.GetEdge(Rml::BoxArea::Padding, Rml::BoxEdge::Bottom);
    const float clientWidth = list->GetClientWidth() - paddingX;
    const float clientHeight = list->GetClientHeight() - paddingY;
    float columnWidth = tileSize == "large" ? 145.0f : (tileSize == "medium" ? 121.0f : 97.0f);
    result.rowHeight = grid ?
        (tileSize == "large" ? 172.0f : (tileSize == "medium" ? 148.0f : 122.0f)) : 30.0f;
    // 首帧/模式切换还没有新布局时使用 RCSS 固定尺寸；下一帧读 margin box 校正。
    if (!presentationDirty && !visibleNodes.empty())
    {
        const auto measured = visibleNodes.front().element->GetBox().GetSize(Rml::BoxArea::Margin);
        if (measured.x > 0.0f && measured.y > 0.0f)
        {
            columnWidth = measured.x;
            result.rowHeight = measured.y;
        }
    }
    if (grid)
        result.columns = clientWidth > 0.0f ?
            std::max(size_t{1}, static_cast<size_t>(clientWidth / columnWidth)) : 4;
    result.columns = std::min(result.columns, MaxVisibleItems);
    const size_t rows = (entries.size() + result.columns - 1) / result.columns;
    if (rows == 0) return result;
    const size_t visibleRows = clientHeight > 0.0f ?
        static_cast<size_t>(std::ceil(clientHeight / result.rowHeight)) + 1 : 3;
    const size_t rowBudget = std::max(size_t{1}, MaxVisibleItems / result.columns);
    const size_t windowRows = std::min(visibleRows + 2 * OverscanRows, rowBudget);
    const float scroll = std::max(0.0f, list->GetScrollTop() -
        box.GetEdge(Rml::BoxArea::Padding, Rml::BoxEdge::Top));
    const size_t scrollRow = std::min(static_cast<size_t>(scroll / result.rowHeight), rows - 1);
    size_t firstRow = scrollRow > OverscanRows ? scrollRow - OverscanRows : 0;
    // 尾页向前补齐窗口，微小滚动不反复增删尾页节点。
    firstRow = std::min(firstRow, rows > windowRows ? rows - windowRows : 0);
    const size_t endRow = std::min(rows, firstRow + windowRows);
    result.begin = firstRow * result.columns;
    result.end = std::min(entries.size(), endRow * result.columns);
    result.top = static_cast<float>(firstRow) * result.rowHeight;
    result.bottom = static_cast<float>(rows - endRow) * result.rowHeight;
    return result;
}

std::string EditorContentBrowserController::EntriesMarkup(const Window& window) const
{
    std::string markup;
    if (window.top > 0.0f)
        markup += "<div class=\"browser-spacer\" style=\"height: " +
            std::to_string(window.top) + "px;\"></div>";
    for (size_t index = window.begin; index < window.end; ++index)
    {
        const auto& entry = *entries[index];
        const char* icon = entry.folder ? "icons/folder-tile.png" : EditorAssetKindIcon(entry.kind);
        markup += "<button class=\"" + std::string(grid ? "asset-tile" : "asset-row");
        if (!entry.folder) markup += " asset-kind-" + std::string(KindFilter(entry.kind));
        markup += "\"" + Address("browser-item", entry.path) + " title=\"" + UiWidgets::Text(entry.path) +
            "\"><img class=\"asset-preview\" src=\"" + icon + "\"/><span class=\"asset-name\">" +
            UiWidgets::Text(entry.name) + "</span><span class=\"asset-kind\">" +
            (entry.folder ? "Folder" : EditorAssetKindName(entry.kind)) + "</span>";
        if (!entry.folder) markup += "<span class=\"asset-dirty\" style=\"display: none;\">*</span>";
        markup += "</button>";
    }
    if (window.bottom > 0.0f)
        markup += "<div class=\"browser-spacer\" style=\"height: " +
            std::to_string(window.bottom) + "px;\"></div>";
    return markup;
}

void EditorContentBrowserController::BindVisibleNodes(bool newDom)
{
    auto previous = std::move(visibleNodes);
    visibleNodes = {};
    auto* list = document->GetElementById("asset-list");
    size_t entryIndex = visibleWindow.begin;
    for (int index = 0; index < list->GetNumChildren(); ++index)
    {
        auto* element = list->GetChild(index);
        if (!element->HasAttribute("data-action")) continue;
        bool dirty = false;
        if (!newDom)
            for (const auto& node : previous)
                if (node.element == element) dirty = node.dirty;
        visibleNodes.push_back({element, entries[entryIndex++], dirty});
    }
    stats.visibleItems = visibleNodes.size();
}

void EditorContentBrowserController::SyncVisibleState()
{
    for (auto& node : visibleNodes)
    {
        const auto& entry = *node.entry;
        const bool selected = selectedPath == entry.path && selectedFolder == entry.folder;
        if (node.element->IsClassSet("selected") != selected) node.element->SetClass("selected", selected);
        const bool dirty = !entry.folder && dirtyPaths.count(entry.path) != 0;
        // marker 是固定节点；仅 dirty 翻转写属性，保留 RmlUi 的跨帧双击目标。
        if (dirty != node.dirty)
        {
            node.element->QuerySelector(".asset-dirty")->SetProperty("display", dirty ? "block" : "none");
            node.dirty = dirty;
        }
    }
}

void EditorContentBrowserController::SyncControls()
{
    auto* checkbox = document->GetElementById("browser-recursive");
    if (recursive != checkbox->HasAttribute("checked"))
    {
        if (recursive) checkbox->SetAttribute("checked", "");
        else checkbox->RemoveAttribute("checked");
    }
    if (!controlsDirty) return;
    widgets.SetControlValue("asset-search", query);
    widgets.SetControlValue("browser-sort", descending ? "name-desc" : "name-asc");
    widgets.SetControlValue("browser-size", tileSize);
    widgets.SetControlValue("browser-type", typeFilter);
    widgets.SetDisabled("browser-back", historyIndex == 0);
    widgets.SetDisabled("browser-forward", historyIndex + 1 == history.size());
    widgets.SetDisabled("browser-up", folder.empty());
    widgets.SetProperty("browser-sources-panel", "display", sourcesVisible ? "flex" : "none");
    document->GetElementById("browser-sources-toggle")->SetClass("selected", sourcesVisible);
    auto* toggle = document->GetElementById("browser-sources-toggle");
    const std::string ariaExpanded = sourcesVisible ? "true" : "false";
    if (toggle->GetAttribute<std::string>("aria-expanded", "") != ariaExpanded)
        toggle->SetAttribute("aria-expanded", ariaExpanded);
    document->GetElementById("browser-grid")->SetClass("selected", grid);
    document->GetElementById("browser-list")->SetClass("selected", !grid);
    auto* list = document->GetElementById("asset-list");
    list->SetClass("browser-grid-view", grid);
    list->SetClass("browser-list-view", !grid);
    for (const char* size : {"small", "medium", "large"})
        list->SetClass(std::string("browser-size-") + size, tileSize == size);
    controlsDirty = false;
}

void EditorContentBrowserController::Build(const MaterialInstanceEditorSnapshot& snapshot)
{
    if (document == nullptr || !document->GetElementById("asset-list")->IsVisible(true)) return;
    UiWidgets::WidgetRefreshScope refresh(refreshingControls);
    const bool catalogChanged = ReadCatalog(snapshot);
    if (catalogChanged) ReconcileNavigation();
    const bool dirtyChanged = ReadDirtyOverlay(snapshot, catalogChanged);
    SyncControls();
    const bool rebuildEntries = entriesDirty;
    if (entriesDirty)
    {
        BuildEntries();
        entriesDirty = false;
    }
    if (sourcesDirty)
    {
        widgets.SetMarkup("asset-sources", SourcesMarkup());
        ++stats.sourcesMarkupBuilds;
        sourcesDirty = false;
    }
    if (breadcrumbsDirty)
    {
        widgets.SetMarkup("asset-breadcrumbs", BreadcrumbMarkup());
        ++stats.breadcrumbMarkupBuilds;
        breadcrumbsDirty = false;
    }
    auto* list = document->GetElementById("asset-list");
    if (resetScroll)
    {
        if (list->GetScrollTop() != 0.0f) list->SetScrollTop(0.0f);
        resetScroll = false;
    }
    const Window window = VisibleWindow();
    bool domChanged = false;
    if (rebuildEntries || presentationDirty || window.begin != visibleWindow.begin ||
        window.end != visibleWindow.end || window.columns != visibleWindow.columns ||
        window.top != visibleWindow.top || window.bottom != visibleWindow.bottom)
    {
        std::string markup = EntriesMarkup(window);
        ++stats.entriesMarkupBuilds;
        if (markup != visibleMarkup || catalogChanged)
        {
            // catalog 换代后 Entry 地址已失效；相同页 markup 也必须重绑，但无需替换 DOM。
            domChanged = markup != visibleMarkup;
            if (domChanged)
            {
                widgets.SetMarkup("asset-list", markup);
                ++stats.listDomRebuilds;
                visibleMarkup = std::move(markup);
            }
            visibleWindow = window;
            BindVisibleNodes(domChanged);
        }
    }
    visibleWindow = window;
    presentationDirty = false;
    if (domChanged || catalogChanged || dirtyChanged || selectionDirty) SyncVisibleState();
    widgets.SetMarkup("browser-selection", UiWidgets::Text(selectedPath.empty() ? "No selection" : selectedPath));
    selectionDirty = false;
    widgets.SetMarkup("asset-empty", (snapshot.browserLoading || snapshot.assetCatalogLoading) ? "Loading..." :
        (query.empty() && typeFilter == "all" ?
            (snapshot.assetCatalog ? "No assets found." : "No material instances found.") : "No matching assets."));
    widgets.SetProperty("asset-empty", "display", entries.empty() ? "block" : "none");
}

bool EditorContentBrowserController::ProcessEvent(Rml::Event& event, std::optional<std::string>& openAssetPath)
{
    openAssetPath.reset();
    if (document == nullptr) return false;
    auto* element = event.GetTargetElement();
    if (!IsDescendantOf(element, document)) return false;
    const std::string type = event.GetType();
    if (type != "click" && type != "change" && type != "dblclick") return false;
    while (element != nullptr && element != document && !element->HasAttribute("data-action"))
        element = element->GetParentNode();
    if (element == nullptr || element == document) return false;
    const std::string action = element->GetAttribute<std::string>("data-action", "");
    const bool item = action == "browser-item";
    const bool tree = action == "browser-folder" || action == "browser-folder-expand";
    const bool crumb = action == "browser-breadcrumb";
    bool control = false;
    for (const char* id : RequiredIds)
        if (action == id && element == document->GetElementById(id)) control = true;
    if (!item && !tree && !crumb && !control) return false;
    // 刷新由 panel 的 request sink 接手，不消费此点击，也不读文件。
    if (action == "browser-refresh") return false;
    if (refreshingControls || element->HasAttribute("disabled")) return true;
    const std::string path = element->GetAttribute<std::string>("data-path", "");
    // 回调只改变值语义状态；DOM 替换延后到 Build，派发中的目标/祖先始终存活。
    if (item)
    {
        const Entry* entry = nullptr;
        for (const auto& node : visibleNodes)
            if (node.element == element && node.entry->path == path) entry = node.entry;
        if (entry == nullptr || !IsDescendantOf(element, document->GetElementById("asset-list"))) return true;
        if (type == "click" || type == "dblclick")
        {
            const float scrollTop = document->GetElementById("asset-list")->GetScrollTop();
            // RmlUi 6.2 的 last_click_element 是原始地址，节点释放后可能复用；
            // 双击还必须属于首击的同一完整路径、DOM 代次和滚动位置。
            if (type == "dblclick" && clickArmed &&
                (clickedPath != path || clickedDomGeneration != stats.listDomRebuilds ||
                    clickedScrollTop != scrollTop))
                return true;
            selectedPath = path;
            selectedFolder = entry->folder;
            selectionDirty = true;
            if (type == "dblclick")
            {
                clickArmed = false;
                if (selectedFolder) Navigate(path);
                else openAssetPath = path;
            }
            else
            {
                clickArmed = true;
                clickedPath = path;
                clickedDomGeneration = stats.listDomRebuilds;
                clickedScrollTop = scrollTop;
            }
        }
    }
    else if (tree)
    {
        if (type != "click" || folders.count(path) == 0 ||
            !IsDescendantOf(element, document->GetElementById("asset-sources"))) return true;
        if (action == "browser-folder") Navigate(path);
        else
        {
            if (expanded.count(path) != 0) expanded.erase(path);
            else expanded.insert(path);
            sourcesDirty = true;
        }
    }
    else if (crumb)
    {
        if (type == "click" && currentBreadcrumbs.count(path) != 0 &&
            IsDescendantOf(element, document->GetElementById("asset-breadcrumbs"))) Navigate(path);
    }
    else if (type == "change")
    {
        const auto* form = dynamic_cast<Rml::ElementFormControl*>(element);
        const std::string value = event.GetParameter<std::string>("value", form == nullptr ? "" : form->GetValue());
        if (action == "asset-search" && query != value)
        {
            query = value;
            FilterChanged();
        }
        else if (action == "browser-recursive")
        {
            const bool oldValue = recursive;
            recursive = event.GetParameter<bool>("checked", element->HasAttribute("checked"));
            if (oldValue != recursive) FilterChanged();
        }
        else if (action == "browser-sort" && (value == "name-asc" || value == "name-desc"))
        {
            const bool oldValue = descending;
            descending = value == "name-desc";
            if (oldValue != descending) FilterChanged();
        }
        else if (action == "browser-type" && ValidFilter(value) && value != typeFilter)
        {
            typeFilter = value;
            FilterChanged();
        }
        else if (action == "browser-size" && (value == "small" || value == "medium" || value == "large"))
        {
            presentationDirty = presentationDirty || tileSize != value;
            tileSize = value;
        }
        controlsDirty = true;
    }
    else if (type == "click")
    {
        if (action == "browser-back" && historyIndex > 0)
        {
            folder = history[--historyIndex];
            ExpandAncestors(folder);
            NavigationChanged();
        }
        else if (action == "browser-forward" && historyIndex + 1 < history.size())
        {
            folder = history[++historyIndex];
            ExpandAncestors(folder);
            NavigationChanged();
        }
        else if (action == "browser-up" && !folder.empty()) Navigate(Parent(folder));
        else if (action == "browser-sources-toggle") sourcesVisible = !sourcesVisible;
        else if (action == "browser-grid" || action == "browser-list")
        {
            const bool newGrid = action == "browser-grid";
            presentationDirty = presentationDirty || newGrid != grid;
            grid = newGrid;
        }
        // panel 会 StopPropagation，RmlUi 的 checkbox 默认动作不会执行；模型先翻转，Build 再同步属性。
        else if (action == "browser-recursive")
        {
            recursive = !recursive;
            FilterChanged();
        }
        controlsDirty = true;
    }
    return true;
}
} // namespace VL::EditorUi
