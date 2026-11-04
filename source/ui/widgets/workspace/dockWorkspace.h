#pragma once

#include <cstdint>
#include <map>
#include <string>
#include <vector>

namespace VL::UiWidgets
{

enum class DockAxis { Horizontal, Vertical };
enum class DockEdge { Center, Left, Right, Top, Bottom };

struct DockRect
{
    float x = 0;
    float y = 0;
    float width = 0;
    float height = 0;
    bool Contains(float px, float py) const;
};

struct DockNode
{
    uint32_t id = 0;
    uint32_t first = 0;
    uint32_t second = 0;
    DockAxis axis = DockAxis::Horizontal;
    float ratio = 0.5f;
    std::vector<std::string> tabs;
    std::string active;
    bool IsSplit() const { return first != 0; }
};

struct FloatingDock
{
    uint32_t node = 0;
    DockRect rect; // 归一化工作区坐标，不依赖显示器或 renderer。
};

struct DockLayoutState
{
    uint32_t root = 0;
    std::map<uint32_t, DockNode> nodes;
    std::vector<FloatingDock> floating;
};

struct DockPlacement
{
    uint32_t node = 0;
    DockRect rect;
    bool floating = false;
};

struct DockSplitter
{
    uint32_t node = 0;
    DockAxis axis = DockAxis::Horizontal;
    DockRect rect;
    DockRect parentRect;
};

struct DockGeometry
{
    std::vector<DockPlacement> stacks;
    std::vector<DockSplitter> splitters;
};

// 纯 GT 布局模型，不含 DOM、文件、业务命令或图形 API。宿主负责反序列化后整体校验。
class DockWorkspace
{
public:
    DockWorkspace();
    void Reset();
    const DockLayoutState& State() const { return state; }
    bool Restore(DockLayoutState candidate, std::string& error);
    static bool Validate(const DockLayoutState& candidate, std::string& error);
    static bool IsResourceDocumentPanel(const std::string& panel);
    // 整个资产编辑器面板族：占位名 `resource` 与全部文档面板（`resource:<hash>`）。
    // 它们**不属于常驻面板集合**，按需创建 —— 没有打开资产时不该占标签位。
    // 布局校验用它把"可有可无"与"必须齐备"分开。
    static bool IsAssetEditorPanel(const std::string& panel);
    uint32_t FindStack(const std::string& panel) const;
    bool Activate(const std::string& panel);
    // 只计算关闭后的候选，不执行关闭；确认保存等业务门禁通过后由宿主提交。
    std::string TabAfterClose(const std::string& panel, const std::vector<std::string>& eligible,
        const std::string& fallback) const;
    bool ReplaceResourcePanels(const std::vector<std::string>& panels);
    bool Dock(const std::string& panel, uint32_t target, DockEdge edge);
    bool Float(const std::string& panel, DockRect normalizedRect);
    bool MoveFloating(uint32_t stack, DockRect normalizedRect);
    bool ResizeSplit(uint32_t split, float firstPixels, const DockRect& parent);
    // collapsedStacks 是纯几何输入：这些叶标签组不占矩形，空间让给兄弟子树。
    // 谁被折叠、可见性集合、持久化字段和"最后一个可见面板不可隐藏"规则都由宿主拥有，
    // 布局模型既不认识面板可见性，也不改写自己的标签集合。
    // maximizedStack 是临时呈现输入，最大化不修改树、浮动矩形和分隔比例。
    DockGeometry Layout(DockRect bounds, const std::vector<uint32_t>& collapsedStacks = {},
        uint32_t maximizedStack = 0) const;
    static const std::vector<std::string>& Panels();
    static DockRect ClampFloating(DockRect rect);

private:
    uint32_t AddStack(std::vector<std::string> tabs);
    uint32_t AddSplit(uint32_t first, uint32_t second, DockAxis axis, float ratio);
    void RemovePanel(const std::string& panel);
    void ReplaceReference(uint32_t from, uint32_t to);
    void RemoveEmptyStack(uint32_t stack);
    DockRect MinimumSize(uint32_t node) const;
    void LayoutNode(uint32_t node, DockRect rect, bool floating,
        const std::vector<uint32_t>& collapsedStacks, DockGeometry& output) const;
    DockLayoutState state;
    uint32_t nextId = 1;
};

} // namespace VL::UiWidgets
