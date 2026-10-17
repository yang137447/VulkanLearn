#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <vector>
#include <nlohmann/json.hpp>

#include "core/logStore.h"
#include "ui/widgets/widgetDocument.h"

namespace Rml { class ElementDocument; class Event; }

namespace VL::EditorUi
{
// 面板只保留冻结展示值与筛选/滚动状态；Clear 是 sequence 水位，不回调日志 producer。
class EditorLogPanel
{
public:
    static constexpr size_t MaxMountedRows = 192;
    static constexpr float RowHeight = 24;
    bool Attach(Rml::ElementDocument& document, std::string& error);
    void Detach();
    void Build(std::shared_ptr<const LogSnapshot> snapshot, bool visible);
    void BuildPerformance(const nlohmann::json& performance, bool visible);
    bool ProcessEvent(Rml::Event& event);
    size_t RowsBuildCount() const { return rowsBuildCount; }
    bool IsScriptsView() const { return scriptsView; }
private:
    void RefreshFilters();
    void RefreshRows(bool forceTail);
    std::string EntryMarkup(const LogEntry& entry) const;
    Rml::ElementDocument* document = nullptr;
    UiWidgets::WidgetDocument widgets;
    std::shared_ptr<const LogSnapshot> snapshot;
    uint64_t filteredRevision = 0;
    const LogSnapshot* filteredIdentity = nullptr;
    uint64_t clearThrough = 0;
    std::vector<size_t> filtered;
    std::string query;
    std::string source;
    std::string severity = "all";
    bool dirty = true;
    bool mounted = false;
    bool autoScroll = true;
    bool pendingTail = false;
    bool visible = false;
    bool scriptsView = false;
    size_t mountedFirst = 0;
    size_t mountedEnd = 0;
    float lastViewportHeight = 0;
    size_t rowsBuildCount = 0;
    size_t scriptsPage = 0;
    size_t scriptsPageCount = 1;
    std::string scriptsQuery;
    double nextPerformanceRefresh = 0;
};
} // namespace VL::EditorUi
