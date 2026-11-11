#pragma once

#include <cstddef>
#include <string>

#include "core/performanceSnapshot.h"
#include "ui/widgets/widgetDocument.h"

namespace Rml
{
class ElementDocument;
}

namespace VL::EditorUi
{

// Performance 页的展示适配器：只消费冻结的 PerformanceSnapshot，
// 不包含 World、资产服务、RenderSystem 或 Vulkan 依赖。
// 刷新节奏与每帧 UI 更新解耦：宿主每帧调用 Build，面板内部按 5 Hz 决定是否重写 DOM。
class EditorPerformancePanel
{
public:
    // 4..8 Hz 之间的折中；0.2 s = 5 Hz。数字来自冻结快照，不逐帧 rebind。
    static constexpr double RefreshIntervalSeconds = 0.2;

    bool Attach(Rml::ElementDocument& document, std::string& error);
    void Detach();
    // nowSeconds 是单调时钟秒；测试可显式推进它来验证节流与刷新。
    void Build(const PerformanceSnapshot& snapshot, bool visible, double nowSeconds);
    // 已执行过的 DOM 刷新次数（可见性变化和首次构建会立即刷新）。
    size_t RowsBuildCount() const { return rowsBuildCount; }
    bool HasSnapshot() const { return hasSnapshot; }

private:
    void Refresh();

    Rml::ElementDocument* document = nullptr;
    UiWidgets::WidgetDocument widgets;
    PerformanceSnapshot snapshot;
    bool hasSnapshot = false;
    bool visible = false;
    bool wasVisible = false;
    bool mounted = false;
    double lastRefreshSeconds = 0.0;
    size_t rowsBuildCount = 0;
};

} // namespace VL::EditorUi
