#include "editor/ui/editorLogPanel.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <ctime>
#include <iomanip>
#include <sstream>
#include <RmlUi/Core/ElementDocument.h>
#include <RmlUi/Core/Elements/ElementFormControl.h>
#include <RmlUi/Core/Event.h>

#include "ui/widgets/text/textWidgets.h"

namespace VL::EditorUi
{
namespace
{
std::string Fold(std::string value)
{
    for (auto& character : value)
        if (character >= 'A' && character <= 'Z') character += 'a' - 'A';
    return value;
}

std::string OneLine(std::string value)
{
    for (auto& character : value)
        if (character == '\n' || character == '\r' || character == '\t') character = ' ';
    return value;
}

std::string Timestamp(int64_t milliseconds)
{
    const std::time_t seconds = static_cast<std::time_t>(milliseconds / 1000);
    std::tm local{};
#if defined(_WIN32)
    localtime_s(&local, &seconds);
#else
    localtime_r(&seconds, &local);
#endif
    std::ostringstream result;
    result << std::put_time(&local, "%H:%M:%S") << '.' <<
        std::setfill('0') << std::setw(3) << milliseconds % 1000;
    return result.str();
}

double Now()
{
    return std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count();
}

std::string Value(const nlohmann::json& object, const char* key)
{
    if (!object.is_object() || !object.contains(key)) return "-";
    const auto& value = object.at(key);
    if (value.is_string()) return value.get<std::string>();
    if (value.is_number_float())
    {
        std::ostringstream text;
        text << std::fixed << std::setprecision(3) << value.get<double>();
        return text.str();
    }
    return value.dump();
}

std::string Metrics(const nlohmann::json& object)
{
    std::string result;
    if (object.contains("callbacks") && object.at("callbacks").is_array())
        for (const auto& callback : object.at("callbacks"))
        {
            if (!result.empty()) result += " | ";
            result += Value(callback, "phase") + ": " + Value(callback, "calls") +
                " calls, last " + Value(callback, "lastMs") + " ms, total " +
                Value(callback, "totalMs") + " ms, max " + Value(callback, "maxMs") + " ms";
        }
    return result;
}

std::string ScriptRow(const nlohmann::json& object, const std::string& owner)
{
    std::string label = owner + Value(object, "id") + " [" + Value(object, "type") + "] " +
        Value(object, "lifecycle") + " enabled=" + Value(object, "enabled");
    const std::string exception = object.value("lastException", std::string{});
    if (!exception.empty()) label += " | " + exception;
    const std::string text = label + " | " + Metrics(object);
    return "<div class=\"log-script-row" + std::string(object.value("failed", false) ? " log-error" : "") +
        "\" title=\"" + UiWidgets::Text(text) + "\">" + UiWidgets::Text(OneLine(text)) + "</div>";
}
} // namespace

bool EditorLogPanel::Attach(Rml::ElementDocument& candidate, std::string& error)
{
    for (const char* id : {"log-page", "log-rows", "log-scroll", "log-count", "log-severity",
        "log-source", "log-search", "log-autoscroll", "log-output", "log-scripts",
        "log-scripts-summary", "log-scripts-rows", "log-scripts-page"})
        if (!candidate.GetElementById(id))
        {
            error = std::string("Log panel is missing element: ") + id;
            return false;
        }
    document = &candidate;
    widgets.Attach(candidate);
    dirty = true;
    mounted = false;
    nextPerformanceRefresh = 0;
    return true;
}

void EditorLogPanel::Detach()
{
    document = nullptr;
    widgets.Detach();
    mounted = false;
}

void EditorLogPanel::Build(std::shared_ptr<const LogSnapshot> value, bool isVisible)
{
    snapshot = std::move(value);
    visible = isVisible;
    if (!document || !visible) return;
    widgets.SetClass("log-output-tab", "selected", !scriptsView);
    widgets.SetClass("log-scripts-tab", "selected", scriptsView);
    widgets.SetProperty("log-output", "display", scriptsView ? "none" : "flex");
    widgets.SetProperty("log-scripts", "display", scriptsView ? "flex" : "none");
    if (scriptsView) return;
    const bool changed = dirty || snapshot.get() != filteredIdentity ||
        (snapshot && snapshot->revision != filteredRevision);
    if (changed) RefreshFilters();
    RefreshRows((changed || pendingTail) && autoScroll);
}

void EditorLogPanel::RefreshFilters()
{
    filtered.clear();
    const auto foldedQuery = Fold(query);
    const auto foldedSource = Fold(source);
    if (snapshot)
        for (size_t index = 0; index < snapshot->entries.size(); ++index)
        {
            const auto& entry = snapshot->entries[index];
            if (entry.sequence <= clearThrough) continue;
            if (severity != "all" && Fold(LogSeverityName(entry.severity)) != severity) continue;
            if (!source.empty() && Fold(entry.source).find(foldedSource) == std::string::npos) continue;
            if (!query.empty() && Fold(entry.message + " " + entry.source + " " + entry.location.file).
                find(foldedQuery) == std::string::npos) continue;
            filtered.push_back(index);
        }
    filteredIdentity = snapshot.get();
    filteredRevision = snapshot ? snapshot->revision : 0;
    dirty = false;
    mounted = false;
    widgets.SetMarkup("log-count", std::to_string(filtered.size()) + " / " +
        std::to_string(snapshot ? snapshot->entries.size() : 0) + " entries" +
        (snapshot && snapshot->discardedEntries ? " | " +
            std::to_string(snapshot->discardedEntries) + " older entries discarded" : ""));
}

std::string EditorLogPanel::EntryMarkup(const LogEntry& entry) const
{
    std::string location;
    if (!entry.location.file.empty())
        location = entry.location.file + "(" + std::to_string(entry.location.line) + "," +
            std::to_string(entry.location.column) + "): ";
    const auto label = location + entry.message;
    return "<div class=\"log-row log-" + Fold(LogSeverityName(entry.severity)) +
        "\" data-sequence=\"" + std::to_string(entry.sequence) + "\" title=\"" +
        UiWidgets::Text(label) + "\"><span class=\"log-time\">" + Timestamp(entry.timestampMilliseconds) +
        "</span><span class=\"log-level\">" + LogSeverityName(entry.severity) +
        "</span><span class=\"log-origin\">" + UiWidgets::Text(entry.source) +
        "</span><span class=\"log-message\">" + UiWidgets::Text(OneLine(label)) + "</span></div>";
}

void EditorLogPanel::RefreshRows(bool forceTail)
{
    auto* scroll = document->GetElementById("log-scroll");
    const float height = std::max(RowHeight, scroll->GetClientHeight());
    if (height != lastViewportHeight && autoScroll) forceTail = true;
    lastViewportHeight = height;
    float top = std::max(0.0f, scroll->GetScrollTop());
    if (forceTail) top = std::max(0.0f, filtered.size() * RowHeight - height);
    size_t first = static_cast<size_t>(top / RowHeight);
    first = first > 2 ? first - 2 : 0;
    const size_t count = std::min(MaxMountedRows, static_cast<size_t>(std::ceil(height / RowHeight)) + 4);
    first = std::min(first, filtered.size() > count ? filtered.size() - count : 0);
    const size_t end = std::min(filtered.size(), first + count);
    if (!mounted || first != mountedFirst || end != mountedEnd)
    {
        std::string markup;
        if (first) markup += "<div class=\"log-spacer\" style=\"height:" +
            std::to_string(first * RowHeight) + "px;\"></div>";
        for (size_t index = first; index < end; ++index)
            markup += EntryMarkup(snapshot->entries[filtered[index]]);
        if (end < filtered.size()) markup += "<div class=\"log-spacer\" style=\"height:" +
            std::to_string((filtered.size() - end) * RowHeight) + "px;\"></div>";
        widgets.SetMarkup("log-rows", std::move(markup));
        mounted = true;
        mountedFirst = first;
        mountedEnd = end;
        ++rowsBuildCount;
    }
    // RmlUi 在下一次 layout 后才能接受新的底部偏移；列表自身的 min-height 保证 extent 已知。
    widgets.SetProperty("log-rows", "min-height", std::to_string(filtered.size() * RowHeight) + "px");
    if (forceTail)
    {
        scroll->SetScrollTop(top);
        pendingTail = std::abs(scroll->GetScrollTop() - top) > 0.5f;
    }
    else pendingTail = false;
}

void EditorLogPanel::BuildPerformance(const nlohmann::json& performance, bool isVisible)
{
    if (!document || !isVisible || !scriptsView || Now() < nextPerformanceRefresh) return;
    nextPerformanceRefresh = Now() + 0.25;
    const auto time = performance.value("time", nlohmann::json::object());
    const auto pending = performance.value("pending", nlohmann::json::object());
    const auto assembly = performance.value("assembly", nlohmann::json::object());
    const auto gc = performance.value("gc", nlohmann::json::object());
    std::string summary = "Time: dt " + Value(time, "deltaTime") + " s | unscaled " +
        Value(time, "unscaledDeltaTime") + " s | scale " + Value(time, "timeScale") +
        " | play " + Value(time, "timeSincePlay") + " s | fixed " + Value(time, "fixedSteps") +
        " | dropped " + Value(time, "droppedSteps") + " (" + Value(time, "droppedTime") +
        " s) | physics " + Value(performance, "physicsMs") + " ms\nAssembly: generation " +
        Value(assembly, "generation") + " | loaded " + Value(assembly, "loaded") +
        " | retired alive " + Value(assembly, "retiredAlive") + " | contexts " + Value(assembly, "contexts") +
        "\nGC: gen0 " + Value(gc, "generation0") + " | gen1 " + Value(gc, "generation1") +
        " | gen2 " + Value(gc, "generation2") + " | heap " + Value(gc, "heapBytes") +
        " bytes\nPending: timers " + Value(pending, "timers") + " | subscriptions " +
        Value(pending, "subscriptions") + " | events " + Value(pending, "events") +
        " | rays " + Value(pending, "rays") + " | assets " + Value(pending, "assets") +
        " | commands " + Value(pending, "commands") + " | components " + Value(pending, "componentCommands");
    summary += "\nBlueprints: " + Value(performance, "blueprintCount") +
        (performance.value("truncated", false) ? " | snapshot truncated" : "");
    summary += " | details frame " + Value(performance, "detailsFrame") +
        " | age " + Value(performance, "detailsAgeSeconds") + " s";
    widgets.SetMarkup("log-scripts-summary", UiWidgets::Text(summary));
    struct ScriptObjectRow
    {
        const nlohmann::json* object;
        std::string owner;
    };
    std::vector<ScriptObjectRow> rows;
    const std::string search = Fold(scriptsQuery);
    if (performance.contains("blueprints") && performance.at("blueprints").is_array())
        for (const auto& blueprint : performance.at("blueprints"))
        {
            const auto owner = Value(blueprint, "name") + " / ";
            if (search.empty() || Fold(owner + Value(blueprint, "id") + Value(blueprint, "type")).
                find(search) != std::string::npos)
                rows.push_back({&blueprint, owner});
            if (blueprint.contains("components") && blueprint.at("components").is_array())
                for (const auto& component : blueprint.at("components"))
                    if (search.empty() || Fold(owner + Value(component, "id") + Value(component, "type")).
                        find(search) != std::string::npos)
                        rows.push_back({&component, owner + "component / "});
        }
    constexpr size_t PageSize = 64;
    scriptsPageCount = std::max(size_t{1}, (rows.size() + PageSize - 1) / PageSize);
    scriptsPage = std::min(scriptsPage, scriptsPageCount - 1);
    std::string markup;
    const size_t end = std::min(rows.size(), (scriptsPage + 1) * PageSize);
    // 仅格式化本页对象的 callback 明细；大场景不先生成全部对象的 RML 再截断。
    for (size_t index = scriptsPage * PageSize; index < end; ++index)
        markup += ScriptRow(*rows[index].object, rows[index].owner);
    widgets.SetMarkup("log-scripts-rows", std::move(markup));
    widgets.SetMarkup("log-scripts-page", std::to_string(scriptsPage + 1) + " / " +
        std::to_string(scriptsPageCount) + " | " + std::to_string(rows.size()) + " objects");
    widgets.SetDisabled("log-scripts-prev", scriptsPage == 0);
    widgets.SetDisabled("log-scripts-next", scriptsPage + 1 >= scriptsPageCount);
}

bool EditorLogPanel::ProcessEvent(Rml::Event& event)
{
    auto* target = event.GetTargetElement();
    while (target && target != document && !target->HasAttribute("data-action"))
        target = target->GetParentNode();
    if (!target || target == document) return false;
    const auto action = target->GetAttribute<std::string>("data-action", "");
    if (action.rfind("log-", 0) != 0) return false;
    if (event.GetType() == "click")
    {
        if (action == "log-clear")
        {
            clearThrough = snapshot ? snapshot->lastSequence : clearThrough;
            dirty = true;
        }
        else if (action == "log-output" || action == "log-scripts")
        {
            scriptsView = action == "log-scripts";
            nextPerformanceRefresh = 0;
        }
        else if (action == "log-scripts-prev" && scriptsPage > 0) { --scriptsPage; nextPerformanceRefresh = 0; }
        else if (action == "log-scripts-next" && scriptsPage + 1 < scriptsPageCount) { ++scriptsPage; nextPerformanceRefresh = 0; }
    }
    if (event.GetType() == "change")
    {
        const auto* control = dynamic_cast<Rml::ElementFormControl*>(target);
        const std::string value = control ? control->GetValue() : "";
        if (action == "log-severity") { severity = value; dirty = true; }
        else if (action == "log-source") { source = value; dirty = true; }
        else if (action == "log-search") { query = value; dirty = true; }
        else if (action == "log-scripts-search") { scriptsQuery = value; scriptsPage = 0; nextPerformanceRefresh = 0; }
        else if (action == "log-autoscroll")
        {
            autoScroll = target->HasAttribute("checked");
            dirty = true;
        }
    }
    return true;
}
} // namespace VL::EditorUi
