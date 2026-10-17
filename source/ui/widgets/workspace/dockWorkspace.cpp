#include "ui/widgets/workspace/dockWorkspace.h"

#include <algorithm>
#include <cmath>
#include <set>
#include <utility>

namespace VL::UiWidgets
{
namespace
{
constexpr float SplitterSize = 6.0f;

bool IsBasePanel(const std::string& panel)
{
    const auto& known = DockWorkspace::Panels();
    return std::find(known.begin(), known.end(), panel) != known.end();
}

bool VisitNode(const DockLayoutState& state, uint32_t id, std::set<uint32_t>& seen,
    std::set<std::string>& panels, std::string& error)
{
    const auto found = state.nodes.find(id);
    if (found == state.nodes.end() || !seen.insert(id).second)
    {
        error = "Missing, shared or cyclic workspace node.";
        return false;
    }
    const auto& node = found->second;
    if (node.id != id || id == 0 || id > 1000000)
    {
        error = "Workspace node identity mismatch.";
        return false;
    }
    if (node.IsSplit())
    {
        if (node.second == 0 || !node.tabs.empty() || !node.active.empty() ||
            !std::isfinite(node.ratio) || node.ratio <= 0 || node.ratio >= 1 ||
            (node.axis != DockAxis::Horizontal && node.axis != DockAxis::Vertical))
        {
            error = "Invalid workspace split.";
            return false;
        }
        return VisitNode(state, node.first, seen, panels, error) &&
            VisitNode(state, node.second, seen, panels, error);
    }
    if (node.second != 0 || node.tabs.empty() ||
        std::find(node.tabs.begin(), node.tabs.end(), node.active) == node.tabs.end())
    {
        error = "Invalid workspace tab stack.";
        return false;
    }
    for (const auto& panel : node.tabs)
    {
        if ((!IsBasePanel(panel) && !DockWorkspace::IsResourceDocumentPanel(panel)) ||
            !panels.insert(panel).second)
        {
            error = "Unknown or duplicated workspace panel.";
            return false;
        }
    }
    return true;
}

float FirstExtent(float total, float ratio, float firstMin, float secondMin)
{
    const float available = std::max(0.0f, total - SplitterSize);
    // 窗口小于整棵树的最小尺寸时按约束比例缩小，不能产生负矩形或把面板推到窗口外。
    if (available < firstMin + secondMin)
        return available * firstMin / (firstMin + secondMin);
    return std::clamp(available * ratio, firstMin, available - secondMin);
}

// 折叠按子树传播：两个子节点都被折叠的分割节点自己不占几何，否则会出现空洞。
bool IsSubtreeCollapsed(const DockLayoutState& state, uint32_t id,
    const std::vector<uint32_t>& collapsedStacks)
{
    const auto found = state.nodes.find(id);
    if (found == state.nodes.end()) return false;
    if (!found->second.IsSplit())
        return std::find(collapsedStacks.begin(), collapsedStacks.end(), id) != collapsedStacks.end();
    return IsSubtreeCollapsed(state, found->second.first, collapsedStacks) &&
        IsSubtreeCollapsed(state, found->second.second, collapsedStacks);
}
} // namespace

bool DockRect::Contains(float px, float py) const
{
    return px >= x && py >= y && px < x + width && py < y + height;
}

const std::vector<std::string>& DockWorkspace::Panels()
{
    // 资产编辑器（"resource" 与 "resource:<hash>"）**不在常驻面板集合里**：
    // 它按需创建 —— 没有打开资产时不该占一个标签位。UE 同样如此。
    static const std::vector<std::string> panels{
        "viewport", "outliner", "details", "content", "renderer", "log"};
    return panels;
}

bool DockWorkspace::IsResourceDocumentPanel(const std::string& panel)
{
    return panel.size() > 9 && panel.compare(0, 9, "resource:") == 0;
}

bool DockWorkspace::IsAssetEditorPanel(const std::string& panel)
{
    // 占位名与全部文档面板同属一族：它们在布局里都是按需出现、可有可无的。
    return panel == "resource" || IsResourceDocumentPanel(panel);
}

DockWorkspace::DockWorkspace() { Reset(); }

uint32_t DockWorkspace::AddStack(std::vector<std::string> tabs)
{
    const uint32_t id = nextId++;
    DockNode node;
    node.id = id;
    node.tabs = std::move(tabs);
    // 空标签栈是不合法的（布局校验要求每个叶子至少有一个标签），而且对空 vector 取 front()
    // 是未定义行为 —— 这里曾经如此。显式留空 active，让非法输入变成可读的失败而不是崩溃；
    // 调用方仍必须在写盘或校验前保证栈非空。
    node.active = node.tabs.empty() ? std::string{} : node.tabs.front();
    state.nodes.emplace(id, std::move(node));
    return id;
}

uint32_t DockWorkspace::AddSplit(uint32_t first, uint32_t second, DockAxis axis, float ratio)
{
    const uint32_t id = nextId++;
    DockNode node;
    node.id = id;
    node.first = first;
    node.second = second;
    node.axis = axis;
    node.ratio = ratio;
    state.nodes.emplace(id, std::move(node));
    return id;
}

void DockWorkspace::Reset()
{
    state = {};
    nextId = 1;
    const uint32_t viewport = AddStack({"viewport"});
    const uint32_t outliner = AddStack({"outliner"});
    const uint32_t details = AddStack({"details"});
    const uint32_t content = AddStack({"content", "renderer", "log"});
    const uint32_t left = AddSplit(viewport, content, DockAxis::Vertical, 0.62f);
    const uint32_t right = AddSplit(outliner, details, DockAxis::Vertical, 0.32f);
    state.root = AddSplit(left, right, DockAxis::Horizontal, 0.69f);
}

bool DockWorkspace::Validate(const DockLayoutState& candidate, std::string& error)
{
    if (candidate.nodes.empty() || candidate.nodes.size() > 32 || candidate.floating.size() > 32)
    {
        error = "Workspace node count is invalid.";
        return false;
    }
    std::set<uint32_t> seen;
    std::set<std::string> panels;
    if (!VisitNode(candidate, candidate.root, seen, panels, error)) return false;
    for (const auto& item : candidate.floating)
    {
        const auto& rect = item.rect;
        if (!std::isfinite(rect.x) || !std::isfinite(rect.y) ||
            !std::isfinite(rect.width) || !std::isfinite(rect.height) ||
            rect.x < 0 || rect.y < 0 || rect.width < 0.1f || rect.height < 0.1f ||
            rect.x + rect.width > 1.00001f || rect.y + rect.height > 1.00001f ||
            candidate.nodes.find(item.node) == candidate.nodes.end() ||
            candidate.nodes.at(item.node).IsSplit())
        {
            error = "Invalid floating workspace bounds.";
            return false;
        }
        if (!VisitNode(candidate, item.node, seen, panels, error)) return false;
    }
    // 基础面板（常驻工具面板）必须恰好齐备。
    // **不再要求存在资产编辑器面板**：它是按需创建的 —— 没有打开资产时不该有它的位置。
    // UE 里同样如此：没有资产被打开时，标签栏里不存在资产编辑器。
    size_t basePanelCount = 0;
    for (const auto& panel : panels)
    {
        if (!DockWorkspace::IsAssetEditorPanel(panel)) ++basePanelCount;
    }
    if (seen.size() != candidate.nodes.size() ||
        basePanelCount != Panels().size())
    {
        error = "Workspace has unreachable nodes or missing panels.";
        return false;
    }
    error.clear();
    return true;
}

bool DockWorkspace::ReplaceResourcePanels(const std::vector<std::string>& panels)
{
    std::set<std::string> desired;
    for (const auto& panel : panels)
        if (!IsResourceDocumentPanel(panel) || !desired.insert(panel).second) return false;

    std::vector<std::string> current;
    for (const auto& item : state.nodes)
    {
        if (item.second.IsSplit()) continue;
        for (const auto& tab : item.second.tabs)
            if (IsResourceDocumentPanel(tab)) current.push_back(tab);
    }

    for (const auto& panel : current)
    {
        if (desired.find(panel) == desired.end()) RemovePanel(panel);
    }
    // 新文档紧邻关卡标签，不为每个资产再创建分栏；已有的手动停靠位置保留。
    if (FindStack("resource") != 0) RemovePanel("resource");
    const uint32_t stackId = FindStack("viewport");
    if (stackId == 0) return false;
    auto& stack = state.nodes.at(stackId);
    for (const auto& panel : panels)
        if (FindStack(panel) == 0) stack.tabs.push_back(panel);
    return true;
}

bool DockWorkspace::Restore(DockLayoutState candidate, std::string& error)
{
    if (!Validate(candidate, error)) return false;
    state = std::move(candidate);
    nextId = state.nodes.rbegin()->first + 1;
    return true;
}

uint32_t DockWorkspace::FindStack(const std::string& panel) const
{
    for (const auto& item : state.nodes)
        if (std::find(item.second.tabs.begin(), item.second.tabs.end(), panel) != item.second.tabs.end())
            return item.first;
    return 0;
}

bool DockWorkspace::Activate(const std::string& panel)
{
    const uint32_t id = FindStack(panel);
    if (id == 0) return false;
    state.nodes.at(id).active = panel;
    for (auto it = state.floating.begin(); it != state.floating.end(); ++it)
    {
        if (it->node == id)
        {
            const FloatingDock raised = *it;
            state.floating.erase(it);
            state.floating.push_back(raised);
            break;
        }
    }
    return true;
}

void DockWorkspace::ReplaceReference(uint32_t from, uint32_t to)
{
    if (state.root == from) state.root = to;
    for (auto& item : state.nodes)
    {
        if (item.second.first == from) item.second.first = to;
        if (item.second.second == from) item.second.second = to;
    }
}

void DockWorkspace::RemoveEmptyStack(uint32_t stack)
{
    for (auto it = state.floating.begin(); it != state.floating.end(); ++it)
    {
        if (it->node == stack)
        {
            state.floating.erase(it);
            state.nodes.erase(stack);
            return;
        }
    }
    for (const auto& item : state.nodes)
    {
        const auto& parent = item.second;
        if (parent.first == stack || parent.second == stack)
        {
            const uint32_t parentId = parent.id;
            const uint32_t sibling = parent.first == stack ? parent.second : parent.first;
            ReplaceReference(parentId, sibling);
            state.nodes.erase(parentId);
            state.nodes.erase(stack);
            return;
        }
    }
}

void DockWorkspace::RemovePanel(const std::string& panel)
{
    const uint32_t source = FindStack(panel);
    auto& node = state.nodes.at(source);
    node.tabs.erase(std::find(node.tabs.begin(), node.tabs.end(), panel));
    if (node.tabs.empty()) RemoveEmptyStack(source);
    else if (node.active == panel) node.active = node.tabs.front();
}

bool DockWorkspace::Dock(const std::string& panel, uint32_t target, DockEdge edge)
{
    const uint32_t source = FindStack(panel);
    const auto targetNode = state.nodes.find(target);
    if (source == 0 || targetNode == state.nodes.end() || targetNode->second.IsSplit()) return false;
    if (source == target && (edge == DockEdge::Center || targetNode->second.tabs.size() == 1))
        return Activate(panel);
    if (source == state.root && state.nodes.at(source).tabs.size() == 1) return false;
    RemovePanel(panel);
    if (edge == DockEdge::Center)
    {
        auto& node = state.nodes.at(target);
        node.tabs.push_back(panel);
        node.active = panel;
        return true;
    }
    // 悬浮组也可以接收边缘分割：先回归主工作区，再拆分；悬浮根始终是标签组。
    for (auto it = state.floating.begin(); it != state.floating.end(); ++it)
    {
        if (it->node == target)
        {
            state.floating.erase(it);
            const uint32_t root = state.root;
            state.root = AddSplit(root, target, DockAxis::Horizontal, 0.7f);
            break;
        }
    }
    const uint32_t added = AddStack({panel});
    const bool before = edge == DockEdge::Left || edge == DockEdge::Top;
    const uint32_t split = AddSplit(before ? added : target, before ? target : added,
        edge == DockEdge::Left || edge == DockEdge::Right ? DockAxis::Horizontal : DockAxis::Vertical, 0.5f);
    // 先替换旧引用，避免新 split 自己引用自己。
    if (state.root == target) state.root = split;
    for (auto& item : state.nodes)
    {
        if (item.first == split) continue;
        if (item.second.first == target) item.second.first = split;
        if (item.second.second == target) item.second.second = split;
    }
    return true;
}

DockRect DockWorkspace::ClampFloating(DockRect rect)
{
    rect.width = std::clamp(rect.width, 0.1f, 1.0f);
    rect.height = std::clamp(rect.height, 0.1f, 1.0f);
    rect.x = std::clamp(rect.x, 0.0f, 1.0f - rect.width);
    rect.y = std::clamp(rect.y, 0.0f, 1.0f - rect.height);
    return rect;
}

bool DockWorkspace::Float(const std::string& panel, DockRect rect)
{
    const uint32_t source = FindStack(panel);
    if (source == 0) return false;
    // 主树最后一个标签不能被移走，否则没有合法的再停靠目标。
    if (source == state.root && state.nodes.at(source).tabs.size() == 1) return false;
    RemovePanel(panel);
    const uint32_t added = AddStack({panel});
    state.floating.push_back({added, ClampFloating(rect)});
    return true;
}

bool DockWorkspace::MoveFloating(uint32_t stack, DockRect rect)
{
    for (auto& item : state.floating)
    {
        if (item.node == stack)
        {
            item.rect = ClampFloating(rect);
            return true;
        }
    }
    return false;
}

DockRect DockWorkspace::MinimumSize(uint32_t id) const
{
    const auto& node = state.nodes.at(id);
    if (!node.IsSplit())
    {
        const bool viewport = std::find(node.tabs.begin(), node.tabs.end(), "viewport") != node.tabs.end();
        const bool content = std::find(node.tabs.begin(), node.tabs.end(), "content") != node.tabs.end();
        // 浏览器至少容纳标签栏、筛选栏和一行默认小卡片；缩小窗口不能只剩半张缩略图。
        return {0, 0, viewport ? 260.0f : 220.0f, content ? 246.0f : (viewport ? 160.0f : 100.0f)};
    }
    const auto first = MinimumSize(node.first);
    const auto second = MinimumSize(node.second);
    if (node.axis == DockAxis::Horizontal)
        return {0, 0, first.width + second.width + SplitterSize, std::max(first.height, second.height)};
    return {0, 0, std::max(first.width, second.width), first.height + second.height + SplitterSize};
}

bool DockWorkspace::ResizeSplit(uint32_t id, float firstPixels, const DockRect& parent)
{
    const auto found = state.nodes.find(id);
    if (found == state.nodes.end() || !found->second.IsSplit() || !std::isfinite(firstPixels)) return false;
    auto& node = found->second;
    const bool horizontal = node.axis == DockAxis::Horizontal;
    const float available = (horizontal ? parent.width : parent.height) - SplitterSize;
    const auto first = MinimumSize(node.first);
    const auto second = MinimumSize(node.second);
    const float firstMin = horizontal ? first.width : first.height;
    const float secondMin = horizontal ? second.width : second.height;
    if (available < firstMin + secondMin) return false;
    node.ratio = std::clamp(firstPixels, firstMin, available - secondMin) / available;
    return true;
}

void DockWorkspace::LayoutNode(uint32_t id, DockRect rect, bool floating,
    const std::vector<uint32_t>& collapsedStacks, DockGeometry& output) const
{
    if (IsSubtreeCollapsed(state, id, collapsedStacks)) return;
    const auto& node = state.nodes.at(id);
    if (!node.IsSplit())
    {
        output.stacks.push_back({id, rect, floating});
        return;
    }
    // 一侧整体折叠时另一侧继承整块父矩形，也不再产生无法拖动的分隔条。
    if (IsSubtreeCollapsed(state, node.first, collapsedStacks))
    {
        LayoutNode(node.second, rect, floating, collapsedStacks, output);
        return;
    }
    if (IsSubtreeCollapsed(state, node.second, collapsedStacks))
    {
        LayoutNode(node.first, rect, floating, collapsedStacks, output);
        return;
    }
    const bool horizontal = node.axis == DockAxis::Horizontal;
    const auto firstMin = MinimumSize(node.first);
    const auto secondMin = MinimumSize(node.second);
    const float extent = FirstExtent(horizontal ? rect.width : rect.height, node.ratio,
        horizontal ? firstMin.width : firstMin.height, horizontal ? secondMin.width : secondMin.height);
    DockRect first = rect, second = rect, splitter = rect;
    if (horizontal)
    {
        first.width = extent;
        second.x += extent + SplitterSize;
        second.width = std::max(0.0f, rect.width - extent - SplitterSize);
        splitter.x += extent;
        splitter.width = SplitterSize;
    }
    else
    {
        first.height = extent;
        second.y += extent + SplitterSize;
        second.height = std::max(0.0f, rect.height - extent - SplitterSize);
        splitter.y += extent;
        splitter.height = SplitterSize;
    }
    output.splitters.push_back({id, node.axis, splitter, rect});
    LayoutNode(node.first, first, floating, collapsedStacks, output);
    LayoutNode(node.second, second, floating, collapsedStacks, output);
}

DockGeometry DockWorkspace::Layout(DockRect bounds, const std::vector<uint32_t>& collapsedStacks) const
{
    DockGeometry output;
    LayoutNode(state.root, bounds, false, collapsedStacks, output);
    for (const auto& item : state.floating)
    {
        if (IsSubtreeCollapsed(state, item.node, collapsedStacks)) continue;
        const auto minimum = MinimumSize(item.node);
        DockRect rect{bounds.x + item.rect.x * bounds.width, bounds.y + item.rect.y * bounds.height,
            std::min(bounds.width, std::max(minimum.width, item.rect.width * bounds.width)),
            std::min(bounds.height, std::max(minimum.height, item.rect.height * bounds.height))};
        rect.x = std::clamp(rect.x, bounds.x, bounds.x + bounds.width - rect.width);
        rect.y = std::clamp(rect.y, bounds.y, bounds.y + bounds.height - rect.height);
        output.stacks.push_back({item.node, rect, true});
    }
    return output;
}

} // namespace VL::UiWidgets
