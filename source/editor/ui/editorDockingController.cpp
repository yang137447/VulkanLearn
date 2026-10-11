#include "editor/ui/editorDockingController.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <fstream>
#include <sstream>
#include <stdexcept>
#include <system_error>
#include <unordered_map>
#include <unordered_set>
#include <utility>

#include <nlohmann/json.hpp>
#include <RmlUi/Core/Context.h>
#include <RmlUi/Core/ElementDocument.h>
#include <RmlUi/Core/Event.h>

#include "ui/widgets/text/textWidgets.h"

namespace VL::EditorUi
{
namespace
{
using namespace UiWidgets;
using Json = nlohmann::json;
using UiWidgets::Text;

std::string BodyId(const std::string& panel)
{
    if (panel == "viewport") return "editor-viewport-surface";
    if (panel == "outliner") return "editor-navigation";
    if (panel == "details") return "editor-inspector";
    if (panel == "content") return "content-browser-page";
    if (panel == "resource" || DockWorkspace::IsResourceDocumentPanel(panel))
        return "resource-editor";
    return "renderer-page";
}

std::string TabId(const std::string& panel)
{
    if (panel == "viewport") return "workspace-viewport-tab";
    if (panel == "outliner") return "workspace-outliner-tab";
    if (panel == "details") return "workspace-details-tab";
    if (panel == "content") return "workspace-content-tab";
    if (panel == "resource") return "workspace-resource-tab";
    if (DockWorkspace::IsResourceDocumentPanel(panel))
        return "workspace-resource-tab-" + panel.substr(9);
    return "workspace-renderer-tab";
}

std::string BaseTabMarkup(const std::string& panel)
{
    if (panel == "viewport") return "Viewport";
    if (panel == "outliner") return "Scene Outliner";
    if (panel == "details") return "Details";
    if (panel == "content") return "Content Browser";
    if (panel == "renderer") return "Renderer";
    return "<img id=\"workspace-resource-icon\" src=\"icons/file-box.png\"/>"
        "<span id=\"workspace-resource-label\">Asset Editor</span>";
}

std::string ResourceTabMarkup(const EditorDockResourceTab& tab)
{
    return "<img src=\"" + Text(tab.icon) + "\"/><span>" + Text(tab.label) + "</span>";
}

void NormalizeResourcePanels(DockLayoutState& state)
{
    for (auto& item : state.nodes)
    {
        auto& node = item.second;
        if (node.IsSplit()) continue;
        bool hasResource = false;
        std::vector<std::string> tabs;
        for (const auto& panel : node.tabs)
        {
            if (DockWorkspace::IsResourceDocumentPanel(panel))
            {
                if (!hasResource)
                {
                    tabs.push_back("resource");
                    hasResource = true;
                }
            }
            else
            {
                if (panel == "resource")
                {
                    if (!hasResource)
                    {
                        tabs.push_back(panel);
                        hasResource = true;
                    }
                }
                else
                {
                    tabs.push_back(panel);
                }
            }
        }
        if (hasResource && DockWorkspace::IsResourceDocumentPanel(node.active))
            node.active = "resource";
        node.tabs = std::move(tabs);
    }
}

std::string Pixels(float value)
{
    return std::to_string(value) + "px";
}

Json EncodeNode(const DockLayoutState& state, uint32_t id)
{
    const auto& node = state.nodes.at(id);
    if (!node.IsSplit())
        return Json{{"tabs", node.tabs}, {"active", node.active}};
    return Json{{"axis", node.axis == DockAxis::Horizontal ? "horizontal" : "vertical"},
        {"ratio", node.ratio}, {"first", EncodeNode(state, node.first)},
        {"second", EncodeNode(state, node.second)}};
}

uint32_t DecodeNode(const Json& data, DockLayoutState& state, uint32_t& nextId, unsigned depth)
{
    if (!data.is_object() || depth > 12 || nextId > 32)
        throw std::runtime_error("Invalid workspace node/depth.");
    DockNode node;
    node.id = nextId++;
    if (data.contains("tabs"))
    {
        if (data.size() != 2 || !data.at("tabs").is_array() ||
            data.at("tabs").size() > 32)
            throw std::runtime_error("Invalid workspace tab list.");
        node.tabs = data.at("tabs").get<std::vector<std::string>>();
        node.active = data.at("active").get<std::string>();
    }
    else
    {
        if (data.size() != 4) throw std::runtime_error("Invalid workspace split fields.");
        const std::string axis = data.at("axis").get<std::string>();
        if (axis != "horizontal" && axis != "vertical") throw std::runtime_error("Invalid workspace axis.");
        node.axis = axis == "horizontal" ? DockAxis::Horizontal : DockAxis::Vertical;
        node.ratio = data.at("ratio").get<float>();
        node.first = DecodeNode(data.at("first"), state, nextId, depth + 1);
        node.second = DecodeNode(data.at("second"), state, nextId, depth + 1);
    }
    state.nodes.emplace(node.id, node);
    return node.id;
}

DockRect PreviewRect(DockRect rect, DockEdge edge)
{
    if (edge == DockEdge::Left) rect.width *= 0.5f;
    if (edge == DockEdge::Right) { rect.x += rect.width * 0.5f; rect.width *= 0.5f; }
    if (edge == DockEdge::Top) rect.height *= 0.5f;
    if (edge == DockEdge::Bottom) { rect.y += rect.height * 0.5f; rect.height *= 0.5f; }
    return rect;
}

// 隐藏集合只接受已知面板名：未知名字说明文件来自其他版本，必须整体拒绝而不是静默丢弃。
bool IsKnownDockPanel(const std::string& panel)
{
    if (DockWorkspace::IsResourceDocumentPanel(panel)) return true;
    const auto& known = DockWorkspace::Panels();
    return std::find(known.begin(), known.end(), panel) != known.end();
}

// 读入的隐藏集合必须至少留下一个可见标签，否则编辑器没有可用的恢复入口。
// 没有任何标签的候选布局交给 DockWorkspace::Validate 报出更准确的错误。
bool HidesEveryPanel(const DockLayoutState& state, const std::vector<std::string>& hidden)
{
    bool sawTab = false;
    for (const auto& item : state.nodes)
    {
        for (const auto& panel : item.second.tabs)
        {
            sawTab = true;
            if (std::find(hidden.begin(), hidden.end(), panel) == hidden.end()) return false;
        }
    }
    return sawTab;
}
} // namespace

bool ReadWorkspaceLayout(const std::filesystem::path& path, DockLayoutState& output,
    std::string& error, std::vector<std::string>* hidden)
{
    try
    {
        if (std::filesystem::file_size(path) > 65536) throw std::runtime_error("Workspace file is too large.");
        std::ifstream stream(path);
        if (!stream) throw std::runtime_error("Workspace file cannot be read.");
        const Json data = Json::parse(stream);
        // "hidden" 是可选字段：旧文件没有它时表示没有任何面板被隐藏。
        if (!data.is_object() || (data.size() != 3 && data.size() != 4) || data.at("version") != 1 ||
            !data.at("floating").is_array() ||
            data.at("floating").size() > 32)
            throw std::runtime_error("Unsupported workspace format.");
        std::vector<std::string> candidateHidden;
        if (data.contains("hidden"))
        {
            const auto& list = data.at("hidden");
            if (!list.is_array() || list.size() > 32)
                throw std::runtime_error("Invalid workspace hidden panel list.");
            for (const auto& item : list)
            {
                if (!item.is_string()) throw std::runtime_error("Invalid workspace hidden panel name.");
                const std::string panel = item.get<std::string>();
                if (!IsKnownDockPanel(panel) ||
                    std::find(candidateHidden.begin(), candidateHidden.end(), panel) != candidateHidden.end())
                    throw std::runtime_error("Unknown or duplicated hidden workspace panel.");
                candidateHidden.push_back(panel);
            }
        }
        DockLayoutState candidate;
        uint32_t nextId = 1;
        candidate.root = DecodeNode(data.at("root"), candidate, nextId, 0);
        for (const auto& item : data.at("floating"))
        {
            if (!item.is_object() || item.size() != 2 || !item.at("rect").is_array() || item.at("rect").size() != 4)
                throw std::runtime_error("Invalid floating workspace format.");
            FloatingDock floating;
            floating.node = DecodeNode(item.at("stack"), candidate, nextId, 0);
            const auto& rect = item.at("rect");
            floating.rect = {rect[0].get<float>(), rect[1].get<float>(), rect[2].get<float>(), rect[3].get<float>()};
            candidate.floating.push_back(floating);
        }
        // v1 旧布局没有资源编辑器页；把它并入 Viewport 标签组，保持旧布局可恢复。
        bool hasResource = false;
        uint32_t viewportStack = 0;
        for (const auto& item : candidate.nodes)
        {
            if (item.second.IsSplit()) continue;
            if (std::find(item.second.tabs.begin(), item.second.tabs.end(), "resource") !=
                item.second.tabs.end())
                hasResource = true;
            if (std::find(item.second.tabs.begin(), item.second.tabs.end(), "viewport") !=
                item.second.tabs.end())
                viewportStack = item.first;
        }
        if (!hasResource && viewportStack != 0)
            candidate.nodes.at(viewportStack).tabs.push_back("resource");
        NormalizeResourcePanels(candidate);
        if (HidesEveryPanel(candidate, candidateHidden))
            throw std::runtime_error("Workspace hides every panel.");
        if (!DockWorkspace::Validate(candidate, error)) return false;
        output = std::move(candidate);
        if (hidden != nullptr) *hidden = std::move(candidateHidden);
        error.clear();
        return true;
    }
    catch (const std::exception& exception)
    {
        error = exception.what();
        return false;
    }
}

bool WriteWorkspaceLayout(const std::filesystem::path& path, const DockLayoutState& state,
    std::string& error, const std::vector<std::string>* hidden)
{
    DockLayoutState persisted = state;
    NormalizeResourcePanels(persisted);
    if (!DockWorkspace::Validate(persisted, error)) return false;
    try
    {
        Json floating = Json::array();
        for (const auto& item : persisted.floating)
            floating.push_back({{"stack", EncodeNode(persisted, item.node)},
                {"rect", {item.rect.x, item.rect.y, item.rect.width, item.rect.height}}});
        Json data{{"version", 1}, {"root", EncodeNode(persisted, persisted.root)}, {"floating", floating}};
        // 只在确实有隐藏面板时写字段，旧读取方仍能解析没有该键的文件。
        if (hidden != nullptr && !hidden->empty()) data["hidden"] = *hidden;
        if (!path.parent_path().empty()) std::filesystem::create_directories(path.parent_path());
        const auto temporary = std::filesystem::path(path.wstring() + L".tmp");
        const auto backup = std::filesystem::path(path.wstring() + L".bak");
        {
            std::ofstream stream(temporary, std::ios::trunc);
            if (!stream) throw std::runtime_error("Workspace temporary file cannot be written.");
            stream << data.dump(2) << '\n';
            stream.flush();
            if (!stream) throw std::runtime_error("Workspace write failed.");
        }
        // Windows rename 不覆盖已有文件；保留旧文件到发布成功，失败时可以回滚。
        std::error_code cleanupError;
        std::filesystem::remove(backup, cleanupError);
        const bool existed = std::filesystem::exists(path);
        if (existed) std::filesystem::rename(path, backup);
        try { std::filesystem::rename(temporary, path); }
        catch (...)
        {
            if (existed) std::filesystem::rename(backup, path);
            throw;
        }
        std::filesystem::remove(backup, cleanupError);
        error.clear();
        return true;
    }
    catch (const std::exception& exception)
    {
        error = exception.what();
        return false;
    }
}

EditorDockingController::EditorDockingController()
{
    const char* userDirectory = std::getenv("LOCALAPPDATA");
    if (userDirectory == nullptr) userDirectory = std::getenv("APPDATA");
    if (userDirectory != nullptr)
        persistencePath = std::filesystem::path(userDirectory) / "VulkanLearn" / "Editor" / "workspace-v1.json";
}

void EditorDockingController::SetPersistencePath(std::filesystem::path path)
{
    persistencePath = std::move(path);
    loaded = false;
}

bool EditorDockingController::IsPanelVisible(const std::string& panel) const
{
    if (!IsKnownDockPanel(panel)) return false;
    return std::find(hiddenPanels.begin(), hiddenPanels.end(), panel) == hiddenPanels.end();
}

size_t EditorDockingController::VisiblePanelCount() const
{
    size_t count = 0;
    for (const auto& item : workspace.State().nodes)
        for (const auto& panel : item.second.tabs)
            if (IsPanelVisible(panel)) ++count;
    return count;
}

std::vector<uint32_t> EditorDockingController::CollapsedStacks() const
{
    std::vector<uint32_t> collapsed;
    for (const auto& item : workspace.State().nodes)
    {
        if (item.second.IsSplit() || item.second.tabs.empty()) continue;
        bool anyVisible = false;
        for (const auto& panel : item.second.tabs)
            if (IsPanelVisible(panel)) anyVisible = true;
        if (!anyVisible) collapsed.push_back(item.first);
    }
    return collapsed;
}

// 隐藏当前激活标签时把激活权交给同组仍可见的标签；整组隐藏时保留原激活记录，
// 重新显示后标签栏和正文仍落在同一个面板上。
void EditorDockingController::HandOffActivation(uint32_t stack, const std::string& hidden)
{
    const auto found = workspace.State().nodes.find(stack);
    if (found == workspace.State().nodes.end() || found->second.active != hidden) return;
    std::string next;
    for (const auto& panel : found->second.tabs)
    {
        if (panel == hidden || !IsPanelVisible(panel)) continue;
        next = panel;
        break;
    }
    if (!next.empty()) workspace.Activate(next);
}

// 资源文档标签关闭后它的隐藏记录也失效；文件里不留再也无法显示的名字。
void EditorDockingController::PruneHiddenPanels()
{
    std::vector<std::string> present;
    for (const auto& item : workspace.State().nodes)
        for (const auto& panel : item.second.tabs)
            present.push_back(panel);
    std::vector<std::string> kept;
    for (const auto& panel : hiddenPanels)
        if (std::find(present.begin(), present.end(), panel) != present.end()) kept.push_back(panel);
    hiddenPanels = std::move(kept);
}

bool EditorDockingController::SetPanelVisible(const std::string& panel, bool visible)
{
    if (!IsKnownDockPanel(panel))
    {
        notice = "Unknown dock panel: " + panel;
        return false;
    }
    if (visible == IsPanelVisible(panel)) return true;
    if (!visible)
    {
        // 全部面板都隐藏会让编辑器没有恢复入口，这里直接拒绝而不是留下死路。
        if (VisiblePanelCount() <= 1)
        {
            notice = "At least one panel must stay visible.";
            return false;
        }
        hiddenPanels.insert(std::lower_bound(hiddenPanels.begin(), hiddenPanels.end(), panel), panel);
        const uint32_t stack = workspace.FindStack(panel);
        if (stack != 0) HandOffActivation(stack, panel);
    }
    else
    {
        hiddenPanels.erase(std::find(hiddenPanels.begin(), hiddenPanels.end(), panel));
        // 显示即激活：Window 菜单的勾选和真正可见的标签页必须指向同一个面板。
        workspace.Activate(panel);
    }
    savePending = true;
    if (document != nullptr)
    {
        // 只改属性与 class，不搬移 DOM，因此可以在事件分发过程中立即生效。
        ApplyGeometry();
        ApplyTabs();
    }
    return true;
}

bool EditorDockingController::LoadLayoutFromDisk()
{
    if (persistencePath.empty()) return false;
    std::error_code existsError;
    if (!std::filesystem::exists(persistencePath, existsError)) return false;
    DockLayoutState candidateState;
    std::vector<std::string> candidateHidden;
    std::string restoreError;
    if (!ReadWorkspaceLayout(persistencePath, candidateState, restoreError, &candidateHidden))
    {
        notice = "Workspace restore failed: " + restoreError;
        return false;
    }
    if (!workspace.Restore(std::move(candidateState), restoreError))
    {
        notice = "Workspace restore failed: " + restoreError;
        return false;
    }
    hiddenPanels = std::move(candidateHidden);
    PruneHiddenPanels();
    return true;
}

bool EditorDockingController::RestoreSavedLayout()
{
    notice.clear();
    if (!LoadLayoutFromDisk())
    {
        if (notice.empty()) notice = "No saved workspace layout is available.";
        return false;
    }
    // 壳元素重建延后到下一次 GT refresh，事件分发期间不销毁正在派发事件的 DOM。
    structureDirty = true;
    savePending = false;
    return true;
}

bool EditorDockingController::Attach(Rml::ElementDocument& candidate, std::string& error)
{
    for (const char* id : {"editor-content", "dock-parking", "dock-dynamic-tabs", "dock-shell",
        "dock-preview", "dock-drag-shield", "editor-viewport-image", "viewport-toolbar"})
    {
        if (candidate.GetElementById(id) == nullptr)
        {
            error = std::string("Dock workspace is missing element: ") + id;
            return false;
        }
    }
    for (const auto& panel : DockWorkspace::Panels())
    {
        if (candidate.GetElementById(BodyId(panel)) == nullptr || candidate.GetElementById(TabId(panel)) == nullptr)
        {
            error = "Dock workspace is missing a panel body or tab.";
            return false;
        }
    }
    document = &candidate;
    widgets.Attach(candidate);
    if (!loaded)
    {
        loaded = true;
        LoadLayoutFromDisk();
    }
    structureDirty = true;
    Sync();
    return true;
}

void EditorDockingController::Detach()
{
    CancelDrag();
    if (savePending) Save();
    restoreFocus = nullptr;
    document = nullptr;
    widgets.Detach();
    geometry = {};
    structureDirty = true;
}

DockRect EditorDockingController::Bounds() const
{
    if (document == nullptr || document->GetContext() == nullptr) return {};
    const auto size = document->GetContext()->GetDimensions();
    return {0, 0, static_cast<float>(size.x), std::max(0.0f, static_cast<float>(size.y) - 66.0f)};
}

void EditorDockingController::MoveElement(Rml::Element& element, Rml::Element& parent)
{
    if (element.GetParentNode() == &parent) return;
    auto owned = element.GetParentNode()->RemoveChild(&element);
    parent.AppendChild(std::move(owned));
}

void EditorDockingController::RebuildShell()
{
    restoreFocus = document->GetContext()->GetFocusElement();
    bool businessFocus = false;
    for (const auto& panel : DockWorkspace::Panels())
    {
        const auto* body = document->GetElementById(BodyId(panel));
        for (const auto* current = restoreFocus; current != nullptr; current = current->GetParentNode())
            if (current == body) businessFocus = true;
    }
    // 壳元素会销毁，只有保留下来的业务子树指针才能跨越重排。
    if (!businessFocus) restoreFocus = nullptr;
    auto* parking = document->GetElementById("dock-parking");
    for (const auto& panel : DockWorkspace::Panels())
    {
        MoveElement(*document->GetElementById(BodyId(panel)), *parking);
        MoveElement(*document->GetElementById(TabId(panel)), *parking);
    }
    for (const auto& tab : resourceTabs)
    {
        auto* element = document->GetElementById(TabId(tab.panel));
        if (element != nullptr) MoveElement(*element, *parking);
    }
    std::string resourceMarkup;
    for (const auto& tab : resourceTabs)
    {
        resourceMarkup += "<button id=\"" + TabId(tab.panel) +
            "\" data-workspace-panel=\"" + Text(tab.panel) +
            "\" title=\"" + Text(tab.path) + "\" class=\"workspace-resource-tab\">";
        resourceMarkup += "<img src=\"" + Text(tab.icon) +
            "\"/><span>" + Text(tab.label) + "</span></button>";
    }
    document->GetElementById("dock-dynamic-tabs")->SetInnerRML(resourceMarkup);
    std::string markup;
    for (const auto& item : workspace.State().nodes)
    {
        const auto& node = item.second;
        if (node.IsSplit())
            markup += "<div id=\"dock-split-" + std::to_string(node.id) +
                "\" class=\"dock-splitter\" data-dock-split=\"" + std::to_string(node.id) + "\"></div>";
        else
            markup += "<div id=\"dock-stack-" + std::to_string(node.id) +
                "\" class=\"dock-stack\"><div id=\"dock-header-" + std::to_string(node.id) +
                "\" class=\"workspace-tabs\"></div><div id=\"dock-body-" + std::to_string(node.id) +
                "\" class=\"dock-body\"></div></div>";
    }
    document->GetElementById("dock-shell")->SetInnerRML(markup);
    widgets.Attach(*document);
    for (const auto& item : workspace.State().nodes)
    {
        const auto& node = item.second;
        if (node.IsSplit()) continue;
        std::unordered_set<std::string> movedBodies;
        for (const auto& panel : node.tabs)
        {
            auto* tab = document->GetElementById(TabId(panel));
            MoveElement(*tab,
                *document->GetElementById("dock-header-" + std::to_string(node.id)));
            // RmlUi 在 display:none 容器中创建的文本子节点被重新挂到可见标题栏后，
            // 部分版本不会重新触发文字布局；重写 inner RML 让标题和图标在首次绘制时稳定出现。
            if (DockWorkspace::IsResourceDocumentPanel(panel))
            {
                for (const auto& resourceTab : resourceTabs)
                    if (resourceTab.panel == panel)
                    {
                        tab->SetInnerRML(ResourceTabMarkup(resourceTab));
                        break;
                    }
            }
            else
            {
                tab->SetInnerRML(BaseTabMarkup(panel));
            }
            const std::string bodyId = BodyId(panel);
            if (movedBodies.insert(bodyId).second)
                MoveElement(*document->GetElementById(bodyId),
                    *document->GetElementById("dock-body-" + std::to_string(node.id)));
        }
    }
    structureDirty = false;
}

void EditorDockingController::ApplyGeometry()
{
    const std::vector<uint32_t> collapsed = CollapsedStacks();
    geometry = workspace.Layout(Bounds(), collapsed);
    // 整组隐藏的标签组不进入几何，也不渲染成空灰框；重新显示时由下一次 Sync 放回原位。
    for (const auto& item : workspace.State().nodes)
    {
        if (item.second.IsSplit()) continue;
        const bool folded = std::find(collapsed.begin(), collapsed.end(), item.first) != collapsed.end();
        widgets.SetProperty("dock-stack-" + std::to_string(item.first), "display", folded ? "none" : "flex");
    }
    int floatingZ = 5;
    for (const auto& stack : geometry.stacks)
    {
        auto* element = document->GetElementById("dock-stack-" + std::to_string(stack.node));
        Place(*element, stack.rect);
        element->SetClass("floating", stack.floating);
        widgets.SetProperty(element->GetId(), "z-index", stack.floating ? std::to_string(floatingZ++) : "1");
    }
    for (const auto& splitter : geometry.splitters)
    {
        auto* element = document->GetElementById("dock-split-" + std::to_string(splitter.node));
        Place(*element, splitter.rect);
        element->SetClass("horizontal", splitter.axis == DockAxis::Horizontal);
        element->SetClass("vertical", splitter.axis == DockAxis::Vertical);
    }
}

void EditorDockingController::ApplyTabs()
{
    std::unordered_map<std::string, bool> bodyActive;
    for (const auto& item : workspace.State().nodes)
    {
        if (item.second.IsSplit()) continue;
        for (const auto& panel : item.second.tabs)
        {
            const bool visible = IsPanelVisible(panel);
            const bool active = visible && item.second.active == panel;
            const std::string bodyId = BodyId(panel);
            bodyActive[bodyId] = bodyActive[bodyId] || active;
            widgets.SetClass(TabId(panel), "selected", active);
            // 隐藏面板保留在布局树中的位置，只从标签栏和正文里抑制；正文 display 由仍然
            // 激活且可见的标签决定，隐藏标签不会把面板正文留在屏幕上。
            widgets.SetProperty(TabId(panel), "display", visible ? "flex" : "none");
        }
    }
    for (const auto& item : bodyActive)
        widgets.SetProperty(item.first, "display", item.second ? "flex" : "none");
}

void EditorDockingController::Place(Rml::Element& element, DockRect rect)
{
    widgets.SetProperty(element.GetId(), "left", Pixels(rect.x));
    widgets.SetProperty(element.GetId(), "top", Pixels(rect.y));
    widgets.SetProperty(element.GetId(), "width", Pixels(rect.width));
    widgets.SetProperty(element.GetId(), "height", Pixels(rect.height));
}

void EditorDockingController::Sync()
{
    if (document == nullptr) return;
    if (!IsDragging()) suppressClick = false;
    // 输入事件只修改模型；下一次 GT refresh 才搬移 DOM，避免销毁正在派发事件的祖先。
    if (structureDirty) RebuildShell();
    ApplyGeometry();
    ApplyTabs();
    if (restoreFocus != nullptr)
    {
        // RemoveChild 会临时转移 RmlUi 焦点；只恢复仍然处于活动业务面板内的原控件。
        for (const auto& panel : DockWorkspace::Panels())
        {
            const uint32_t stack = workspace.FindStack(panel);
            if (stack == 0) continue;
            if (workspace.State().nodes.at(stack).active != panel) continue;
            const auto* body = document->GetElementById(BodyId(panel));
            for (const auto* ancestor = restoreFocus; ancestor != nullptr; ancestor = ancestor->GetParentNode())
                if (ancestor == body) { restoreFocus->Focus(); break; }
        }
        restoreFocus = nullptr;
    }
    if (savePending && !IsDragging()) Save();
}

void EditorDockingController::Save()
{
    savePending = false;
    if (persistencePath.empty()) return;
    std::string error;
    if (!WriteWorkspaceLayout(persistencePath, workspace.State(), error, &hiddenPanels))
        notice = "Workspace save failed: " + error;
    else notice.clear();
}

void EditorDockingController::SetResourceTabs(
    const std::vector<EditorDockResourceTab>& tabs, const std::string& activePath)
{
    bool changed = tabs.size() != resourceTabs.size();
    if (!changed)
    {
        for (size_t index = 0; index < tabs.size(); ++index)
        {
            const auto& lhs = tabs[index];
            const auto& rhs = resourceTabs[index];
            if (lhs.panel != rhs.panel || lhs.path != rhs.path || lhs.label != rhs.label ||
                lhs.icon != rhs.icon)
            {
                changed = true;
                break;
            }
        }
    }
    resourceTabs = tabs;
    if (changed)
    {
        std::vector<std::string> panels;
        panels.reserve(resourceTabs.size());
        for (const auto& tab : resourceTabs) panels.push_back(tab.panel);
        if (!workspace.ReplaceResourcePanels(panels))
        {
            notice = "Resource tabs could not be placed in the workspace.";
            return;
        }
        PruneHiddenPanels();
        structureDirty = true;
        savePending = true;
    }

    for (const auto& tab : resourceTabs)
    {
        if (tab.path == activePath)
        {
            // 隐藏的资源标签不能被激活，否则同组的可见标签会失去正文。
            if (IsPanelVisible(tab.panel)) workspace.Activate(tab.panel);
            break;
        }
    }
    if (document != nullptr)
    {
        if (changed) Sync();
        else ApplyTabs();
    }
}

std::string EditorDockingController::ConsumeActivatedPanel()
{
    std::string result = std::move(activatedPanel);
    activatedPanel.clear();
    return result;
}

void EditorDockingController::Activate(const std::string& panel)
{
    // 隐藏面板不接受激活：同组若还有可见标签，激活权必须留在可见的那一个上。
    if (!IsPanelVisible(panel)) return;
    if (workspace.Activate(panel))
    {
        savePending = true;
        if (document != nullptr) ApplyTabs();
    }
}

std::string EditorDockingController::ResourcePathForPanel(const std::string& panel) const
{
    for (const auto& tab : resourceTabs)
        if (tab.panel == panel) return tab.path;
    return {};
}

void EditorDockingController::Reset()
{
    CancelDrag();
    workspace.Reset();
    // Reset Layout 恢复默认布局，同时把全部面板重新显示出来。
    hiddenPanels.clear();
    structureDirty = true;
    savePending = true;
}

void EditorDockingController::CancelDrag()
{
    pendingPanel.clear();
    dragPanel.clear();
    dragSplit = 0;
    dropTarget = 0;
    if (document != nullptr)
    {
        document->GetElementById("dock-preview")->SetProperty("display", "none");
        document->GetElementById("dock-drag-shield")->SetProperty("display", "none");
    }
}

void EditorDockingController::FindDrop(float x, float y)
{
    dropTarget = 0;
    for (auto it = geometry.stacks.rbegin(); it != geometry.stacks.rend(); ++it)
    {
        if (!it->rect.Contains(x, y)) continue;
        if (it->node == workspace.FindStack(dragPanel)) continue;
        dropTarget = it->node;
        const auto& rect = it->rect;
        const float edgeWidth = std::min(60.0f, rect.width * 0.23f);
        const float edgeHeight = std::min(60.0f, rect.height * 0.23f);
        // 标题栏始终是中心/tab-stack 目标；其余区域的四个边缘形成新 split。
        if (y < rect.y + 32) dropEdge = DockEdge::Center;
        else if (x < rect.x + edgeWidth) dropEdge = DockEdge::Left;
        else if (x > rect.x + rect.width - edgeWidth) dropEdge = DockEdge::Right;
        else if (y < rect.y + edgeHeight) dropEdge = DockEdge::Top;
        else if (y > rect.y + rect.height - edgeHeight) dropEdge = DockEdge::Bottom;
        else dropEdge = DockEdge::Center;
        Place(*document->GetElementById("dock-preview"), PreviewRect(rect, dropEdge));
        break;
    }
    document->GetElementById("dock-preview")->SetProperty("display", dropTarget == 0 ? "none" : "block");
}

void EditorDockingController::UpdateDrag(float x, float y)
{
    if (!pendingPanel.empty() && std::hypot(x - startX, y - startY) > 5.0f)
    {
        dragPanel = pendingPanel;
        pendingPanel.clear();
        document->GetElementById("dock-drag-shield")->SetProperty("display", "block");
    }
    if (!dragPanel.empty()) FindDrop(x, y);
    else if (dragSplit != 0)
    {
        const auto& node = workspace.State().nodes.at(dragSplit);
        const float first = node.axis == DockAxis::Horizontal ? x - splitParent.x : y - splitParent.y;
        if (workspace.ResizeSplit(dragSplit, first, splitParent))
        {
            savePending = true;
            ApplyGeometry();
        }
    }
}

void EditorDockingController::FinishDrag(float x, float y)
{
    if (!dragPanel.empty())
    {
        FindDrop(x, y);
        bool changed = false;
        if (dropTarget != 0) changed = workspace.Dock(dragPanel, dropTarget, dropEdge);
        else
        {
            const auto bounds = Bounds();
            if (bounds.width > 0 && bounds.height > 0)
            {
                const DockRect rect{(dragOriginal.x + x - startX) / bounds.width,
                    (dragOriginal.y + y - startY) / bounds.height,
                    dragOriginal.width / bounds.width, dragOriginal.height / bounds.height};
                const uint32_t source = workspace.FindStack(dragPanel);
                if (workspace.State().nodes.at(source).tabs.size() == 1 && workspace.MoveFloating(source, rect))
                    changed = true;
                else changed = workspace.Float(dragPanel, rect);
            }
        }
        if (changed) { structureDirty = true; savePending = true; }
        suppressClick = true;
    }
    CancelDrag();
}

bool EditorDockingController::ProcessEvent(Rml::Event& event, bool modal)
{
    if (document == nullptr) return false;
    if (modal)
    {
        CancelDrag();
        return false;
    }
    auto* element = event.GetTargetElement();
    const std::string type = event.GetType();
    const auto point = event.GetUnprojectedMouseScreenPos();
    const float x = point.x, y = point.y - 41.0f;
    if (type == "mousemove" && (!pendingPanel.empty() || IsDragging()))
    {
        UpdateDrag(x, y);
        return IsDragging();
    }
    if (type == "mouseup" && (!pendingPanel.empty() || IsDragging()))
    {
        const bool captured = IsDragging();
        FinishDrag(x, y);
        return captured;
    }
    if (type == "click" && suppressClick)
    {
        suppressClick = false;
        return true;
    }
    while (element != nullptr && element != document &&
        !element->HasAttribute("data-workspace-panel") && !element->HasAttribute("data-dock-split") &&
        element->GetAttribute<std::string>("data-action", "") != "workspace-reset")
        element = element->GetParentNode();
    if (element == nullptr || element == document) return false;
    const std::string panel = element->GetAttribute<std::string>("data-workspace-panel", "");
    if (type == "click")
    {
        if (!panel.empty())
        {
            Activate(panel);
            if (DockWorkspace::IsResourceDocumentPanel(panel))
                activatedPanel = panel;
        }
        else if (element->GetAttribute<std::string>("data-action", "") == "workspace-reset") Reset();
        else return false;
        return true;
    }
    if (type == "mousedown" && event.GetParameter<int>("button", 0) == 0)
    {
        startX = x;
        startY = y;
        suppressClick = false;
        if (!panel.empty())
        {
            pendingPanel = panel;
            const uint32_t source = workspace.FindStack(panel);
            for (const auto& stack : geometry.stacks)
                if (stack.node == source) dragOriginal = stack.rect;
        }
        else
        {
            dragSplit = element->GetAttribute<uint32_t>("data-dock-split", 0);
            for (const auto& splitter : geometry.splitters)
                if (splitter.node == dragSplit) splitParent = splitter.parentRect;
            if (dragSplit != 0) document->GetElementById("dock-drag-shield")->SetProperty("display", "block");
        }
        return true;
    }
    return false;
}

UiViewportRect EditorDockingController::GetSceneViewportRect() const
{
    if (document == nullptr || !document->IsVisible()) return {};
    const uint32_t stack = workspace.FindStack("viewport");
    if (workspace.State().nodes.at(stack).active != "viewport") return {};
    auto* image = document->GetElementById("editor-viewport-image");
    if (image == nullptr || !image->IsVisible(true)) return {};
    const auto offset = image->GetAbsoluteOffset(Rml::BoxArea::Content);
    const auto size = image->GetBox().GetSize(Rml::BoxArea::Content);
    const auto dimensions = document->GetContext()->GetDimensions();
    const int left = std::clamp(static_cast<int>(std::ceil(offset.x)), 0, dimensions.x);
    const int top = std::clamp(static_cast<int>(std::ceil(offset.y)), 0, dimensions.y);
    const int right = std::clamp(static_cast<int>(std::floor(offset.x + size.x)), left, dimensions.x);
    const int bottom = std::clamp(static_cast<int>(std::floor(offset.y + size.y)), top, dimensions.y);
    if (right == left || bottom == top) return {};
    return {left, top, static_cast<uint32_t>(right - left), static_cast<uint32_t>(bottom - top)};
}

bool EditorDockingController::IsSceneViewportElement(const Rml::Element* element) const
{
    if (document == nullptr) return false;
    const auto* toolbar = document->GetElementById("viewport-toolbar");
    const auto* surface = document->GetElementById("editor-viewport-surface");
    for (const auto* current = element; current != nullptr && current != document;
        current = current->GetParentNode())
    {
        // 视口工具条叠在场景图像上，但它的点击属于编辑器：不能同时驱动相机或场景拾取。
        if (current == toolbar) return false;
        if (current == surface) return surface->IsVisible(true);
    }
    return false;
}

} // namespace VL::EditorUi
