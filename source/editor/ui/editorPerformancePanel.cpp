#include "editor/ui/editorPerformancePanel.h"

#include <algorithm>
#include <cmath>
#include <string>

#include <RmlUi/Core/ElementDocument.h>

#include "ui/widgets/text/textWidgets.h"

namespace VL::EditorUi
{
namespace
{

std::string MillisecondsText(const std::optional<double>& value)
{
    return value.has_value() ? FormatPerformanceNumber(*value) + " ms" : "--";
}

std::string PercentText(const std::optional<double>& percent)
{
    return percent.has_value() ? FormatPerformanceNumber(*percent, 1) + "%" : "n/a";
}

// 条形宽度用 RCSS class 表达，不写内联 style；5% 一档对视觉比例足够，
// 精确百分比仍在同一行的文本里。class 必须存在于 editor-workspace.rcss。
std::string BarClass(const std::optional<double>& percent)
{
    int bucket = 0;
    if (percent.has_value())
    {
        bucket = static_cast<int>(std::lround(*percent / 5.0) * 5.0);
        bucket = std::clamp(bucket, 0, 100);
    }
    return "performance-bar-" + std::to_string(bucket);
}

} // namespace

bool EditorPerformancePanel::Attach(Rml::ElementDocument& candidate, std::string& error)
{
    for (const char* id : {"performance-page", "performance-fps", "performance-cpu-ms",
        "performance-gpu-ms", "performance-status", "performance-passes"})
    {
        if (candidate.GetElementById(id) == nullptr)
        {
            error = std::string("Performance panel is missing element: ") + id;
            return false;
        }
    }
    document = &candidate;
    widgets.Attach(candidate);
    mounted = false;
    wasVisible = false;
    hasSnapshot = false;
    return true;
}

void EditorPerformancePanel::Detach()
{
    document = nullptr;
    widgets.Detach();
    mounted = false;
    wasVisible = false;
    hasSnapshot = false;
}

void EditorPerformancePanel::Build(
    const PerformanceSnapshot& value,
    bool isVisible,
    double nowSeconds)
{
    const bool visibilityChanged = isVisible != wasVisible;
    visible = isVisible;
    wasVisible = isVisible;
    // 隐藏页不做数字格式化或 DOM 工作；重新显示时立即补一次。
    if (document == nullptr || !visible)
    {
        return;
    }

    const bool due = !mounted || visibilityChanged ||
        !(nowSeconds > lastRefreshSeconds) ||
        (nowSeconds - lastRefreshSeconds) >= RefreshIntervalSeconds;
    if (!due)
    {
        return;
    }

    // 只在真正刷新时复制 pass 数组，避免每帧一次 vector<string> 深拷贝。
    snapshot = value;
    hasSnapshot = true;
    lastRefreshSeconds = nowSeconds;
    mounted = true;
    Refresh();
}

void EditorPerformancePanel::Refresh()
{
    widgets.SetMarkup("performance-fps", snapshot.fps.has_value()
        ? FormatPerformanceNumber(*snapshot.fps) : "--");
    widgets.SetMarkup("performance-cpu-ms", MillisecondsText(snapshot.cpuFrameMs));
    widgets.SetMarkup("performance-gpu-ms", MillisecondsText(snapshot.gpuTotalMs));

    std::string status;
    if (!snapshot.gpuTimingSupported)
    {
        status = "GPU pass timing unsupported on this device; FPS and CPU frame time remain available.";
    }
    else if (!snapshot.collecting || snapshot.passes.empty())
    {
        status = "Waiting for GPU samples...";
    }
    else if (!snapshot.gpuTotalMs.has_value())
    {
        status = "No valid GPU samples in the current window.";
    }
    else
    {
        status = "GPU pass average over " + std::to_string(snapshot.windowFrameCount) +
            " frame" + (snapshot.windowFrameCount == 1 ? "" : "s") + ".";
    }
    widgets.SetMarkup("performance-status", UiWidgets::Text(status));

    std::string rows;
    rows.reserve(snapshot.passes.size() * 160);
    for (const PerformancePassSample& pass : snapshot.passes)
    {
        const std::optional<double> percent = PerformancePassPercent(snapshot, pass);
        rows += "<div class=\"performance-row\">";
        rows += "<span class=\"performance-pass-name\" title=\"" +
            UiWidgets::Text(pass.name) + "\">" + UiWidgets::Text(pass.name) + "</span>";
        rows += "<span class=\"performance-pass-ms\">" +
            (pass.milliseconds.has_value() ? FormatPerformanceNumber(*pass.milliseconds) : "n/a") +
            "</span>";
        rows += "<span class=\"performance-pass-percent\">" + PercentText(percent) + "</span>";
        rows += "<span class=\"performance-pass-bar-cell\"><span class=\"performance-bar-fill " +
            BarClass(percent) + "\"></span></span>";
        rows += "</div>";
    }
    widgets.SetMarkup("performance-passes", rows);
    ++rowsBuildCount;
}

} // namespace VL::EditorUi
