#include <gtest/gtest.h>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <memory>
#include <thread>
#include <vector>
#include <RmlUi/Core/Context.h>
#include <RmlUi/Core/Core.h>
#include <RmlUi/Core/ElementDocument.h>
#include <RmlUi/Core/Elements/ElementFormControl.h>
#include <RmlUi/Core/Event.h>
#include <RmlUi/Core/EventListener.h>
#include <RmlUi/Core/RenderInterface.h>
#include <RmlUi/Core/SystemInterface.h>

#include "core/logStore.h"
#include "editor/ui/editorDockingController.h"
#include "editor/ui/editorLogPanel.h"
#include "engine/diagnosticsSubsystem.h"
#include "ui/rmlUiInterfaces.h"

namespace
{
using namespace VL;
using namespace VL::EditorUi;
using namespace VL::UiWidgets;

TEST(LogStoreTest, FreezeReusesTheUnchangedSnapshotAndOldSnapshotsRemainImmutable)
{
    LogStore store;
    const auto empty = store.Freeze();
    EXPECT_EQ(empty, store.Freeze());
    store.Append(LogSeverity::Info, "Engine", "first");
    const auto first = store.Freeze();
    EXPECT_NE(first, empty);
    EXPECT_TRUE(empty->entries.empty());
    store.Append(LogSeverity::Error, "CSharp", "second", {"Gameplay/Test.cs", 12, 7});
    const auto second = store.Freeze();
    ASSERT_EQ(first->entries.size(), 1u);
    ASSERT_EQ(second->entries.size(), 2u);
    EXPECT_EQ(second->entries[1].location.file, "Gameplay/Test.cs");
    EXPECT_EQ(second->entries[1].location.line, 12u);
    EXPECT_EQ(second->entries[1].location.column, 7u);
    EXPECT_EQ(second, store.Freeze());
}

TEST(LogStoreTest, ClearNeverReusesASequenceAndCannotMutateAPublishedSnapshot)
{
    LogStore store;
    const auto before = store.Append(LogSeverity::Warning, "Script", "warning");
    const auto snapshot = store.Freeze();
    store.Clear();
    EXPECT_TRUE(store.Freeze()->entries.empty());
    EXPECT_EQ(store.Freeze()->lastSequence, before);
    EXPECT_GT(store.Append(LogSeverity::Error, "Script", "after clear"), before);
    EXPECT_EQ(snapshot->entries.size(), 1u);
}

void AppendThread(LogStore& store, size_t count)
{
    for (size_t index = 0; index < count; ++index)
        store.Append(LogSeverity::Info, "Worker", std::to_string(index));
}

TEST(LogStoreTest, ConcurrentProducersHaveOneOrderedBoundedSequence)
{
    LogStore store;
    std::vector<std::thread> workers;
    for (size_t index = 0; index < 4; ++index) workers.emplace_back(AppendThread, std::ref(store), 1500);
    // 读侧在 producer 活动时拍照，旧副本不借用 deque 或字符串内存。
    const auto during = store.Freeze();
    for (auto& worker : workers) worker.join();
    const auto result = store.Freeze();
    ASSERT_EQ(result->entries.size(), LogStore::MaxEntries);
    EXPECT_EQ(result->lastSequence, 6000u);
    EXPECT_EQ(result->discardedEntries, 6000u - LogStore::MaxEntries);
    for (size_t index = 1; index < result->entries.size(); ++index)
        EXPECT_EQ(result->entries[index].sequence, result->entries[index - 1].sequence + 1);
    EXPECT_LE(during->entries.size(), LogStore::MaxEntries);
}

TEST(LogStoreTest, TextAndTotalRetentionBudgetsBoundLargeMessages)
{
    LogStore store;
    for (size_t index = 0; index < 1000; ++index)
        store.Append(LogSeverity::Info, std::string(300, 's'), std::string(12000, 'm'),
            {std::string(2000, 'f'), 1, 1});
    const auto result = store.Freeze();
    size_t bytes = 0;
    for (const auto& entry : result->entries)
    {
        EXPECT_LE(entry.source.size(), LogStore::MaxSourceBytes);
        EXPECT_LE(entry.message.size(), LogStore::MaxMessageBytes);
        EXPECT_LE(entry.location.file.size(), LogStore::MaxFileBytes);
        bytes += entry.source.size() + entry.message.size() + entry.location.file.size();
    }
    EXPECT_LE(bytes, LogStore::MaxBytes);
    EXPECT_GT(result->discardedEntries, 0u);
}

TEST(LogStoreTest, TruncationPreservesMultibyteUtf8)
{
    LogStore store;
    store.Append(LogSeverity::Info, "Script", std::string(LogStore::MaxMessageBytes - 4, 'x') +
        "\xe4\xb8\xad\xe6\x96\x87");
    const auto message = store.Freeze()->entries.front().message;
    EXPECT_LE(message.size(), LogStore::MaxMessageBytes);
    EXPECT_EQ(message.substr(message.size() - 3), "...");
    EXPECT_NO_THROW(nlohmann::json(message).dump());
}

TEST(LogStoreTest, DiagnosticsAndRmlUiKeepTheirSourceAndSeverity)
{
    const auto start = ProcessLog().Freeze()->lastSequence;
    DiagnosticsSubsystem diagnostics;
    diagnostics.ReportWarning("central warning", "TestSource");
    RmlUiSystemInterface system;
    system.LogMessage(Rml::Log::LT_ERROR, "central Rml error");
    system.LogMessage(Rml::Log::LT_DEBUG, "central Rml debug");
    const auto snapshot = ProcessLog().Freeze();
    std::vector<LogEntry> recent;
    for (const auto& entry : snapshot->entries) if (entry.sequence > start) recent.push_back(entry);
    ASSERT_EQ(recent.size(), 3u);
    EXPECT_EQ(recent[0].severity, LogSeverity::Warning);
    EXPECT_EQ(recent[0].source, "TestSource");
    EXPECT_EQ(recent[1].source, "RmlUi");
    EXPECT_EQ(recent[1].severity, LogSeverity::Error);
    EXPECT_EQ(recent[2].severity, LogSeverity::Debug);
}

class LogTestSystem final : public Rml::SystemInterface
{
public:
    bool LogMessage(Rml::Log::Type type, const Rml::String& message) override
    {
        if (type <= Rml::Log::LT_WARNING) errors.push_back(message);
        return true;
    }
    std::vector<std::string> errors;
};

class LogTestRenderer final : public Rml::RenderInterface
{
public:
    Rml::CompiledGeometryHandle CompileGeometry(Rml::Span<const Rml::Vertex>, Rml::Span<const int>) override { return ++next; }
    void RenderGeometry(Rml::CompiledGeometryHandle, Rml::Vector2f, Rml::TextureHandle) override {}
    void ReleaseGeometry(Rml::CompiledGeometryHandle) override {}
    Rml::TextureHandle LoadTexture(Rml::Vector2i& dimensions, const Rml::String&) override { dimensions = {32, 32}; return 1; }
    Rml::TextureHandle GenerateTexture(Rml::Span<const Rml::byte>, Rml::Vector2i) override { return 1; }
    void ReleaseTexture(Rml::TextureHandle) override {}
    void EnableScissorRegion(bool) override {}
    void SetScissorRegion(Rml::Rectanglei) override {}
    size_t next = 0;
};

class EditorLogPanelTest : public testing::Test, public Rml::EventListener
{
protected:
    void SetUp() override
    {
        Rml::SetSystemInterface(&system);
        Rml::SetRenderInterface(&renderer);
        ASSERT_TRUE(Rml::Initialise());
        ASSERT_TRUE(Rml::LoadFontFace("C:/Windows/Fonts/segoeui.ttf"));
        context = Rml::CreateContext("log-test", {1280, 720});
        ASSERT_NE(context, nullptr);
        document = context->LoadDocument(std::string(VL_EDITOR_ASSET_ROOT) + "/editor-workspace.rml");
        ASSERT_NE(document, nullptr);
        docking.SetPersistencePath({});
        std::string error;
        ASSERT_TRUE(docking.Attach(*document, error)) << error;
        docking.Activate("log");
        ASSERT_TRUE(panel.Attach(*document, error)) << error;
        document->AddEventListener("click", this);
        document->AddEventListener("change", this);
        document->Show();
        Refresh();
        ASSERT_TRUE(system.errors.empty());
    }
    void TearDown() override
    {
        panel.Detach();
        docking.Detach();
        if (context) Rml::RemoveContext(context->GetName());
        Rml::Shutdown();
        Rml::SetSystemInterface(nullptr);
        Rml::SetRenderInterface(nullptr);
    }
    void ProcessEvent(Rml::Event& event) override { panel.ProcessEvent(event); }
    void Refresh()
    {
        docking.Sync();
        panel.Build(store.Freeze(), docking.IsPanelActive("log"));
        context->Update();
    }
    void Set(const std::string& id, const std::string& value)
    {
        auto* control = dynamic_cast<Rml::ElementFormControl*>(document->GetElementById(id));
        ASSERT_NE(control, nullptr);
        control->SetValue(value);
        control->DispatchEvent("change", {});
        Refresh();
    }
    void Click(const std::string& id)
    {
        auto* element = document->GetElementById(id);
        ASSERT_NE(element, nullptr);
        const auto position = element->GetAbsoluteOffset(Rml::BoxArea::Border) +
            element->GetBox().GetSize(Rml::BoxArea::Border) * 0.5f;
        context->ProcessMouseMove(static_cast<int>(position.x), static_cast<int>(position.y), 0);
        context->ProcessMouseButtonDown(0, 0);
        context->ProcessMouseButtonUp(0, 0);
        Refresh();
    }
    Rml::ElementList Rows()
    {
        Rml::ElementList result;
        document->GetElementById("log-rows")->QuerySelectorAll(result, ".log-row");
        return result;
    }
    LogTestSystem system;
    LogTestRenderer renderer;
    Rml::Context* context = nullptr;
    Rml::ElementDocument* document = nullptr;
    EditorDockingController docking;
    EditorLogPanel panel;
    LogStore store;
};

TEST_F(EditorLogPanelTest, EntriesEscapeMarkupAndDisplayCompilerLocations)
{
    store.Append(LogSeverity::Error, "CSharp", "<Bad>&\"value\"", {"Gameplay/Broken.cs", 8, 2});
    Refresh();
    auto* rows = document->GetElementById("log-rows");
    EXPECT_EQ(rows->QuerySelector("Bad"), nullptr);
    EXPECT_NE(rows->GetInnerRML().find("Broken.cs(8,2)"), std::string::npos);
    ASSERT_EQ(Rows().size(), 1u);
    EXPECT_TRUE(Rows().front()->IsClassSet("log-error"));
}

TEST_F(EditorLogPanelTest, SeveritySourceAndSearchComposeWithoutChangingTheStore)
{
    store.Append(LogSeverity::Warning, "Script", "Needle one");
    store.Append(LogSeverity::Error, "Script", "Needle two");
    store.Append(LogSeverity::Error, "Engine", "Needle three");
    store.Append(LogSeverity::Error, "Script", "other");
    Refresh();
    Set("log-severity", "error");
    Set("log-source", "sCrIpT");
    Set("log-search", "nEeDlE");
    ASSERT_EQ(Rows().size(), 1u);
    EXPECT_NE(Rows().front()->GetInnerRML().find("Needle two"), std::string::npos);
    EXPECT_EQ(store.Freeze()->entries.size(), 4u);
}

TEST_F(EditorLogPanelTest, ClearHidesExistingEntriesAndNewEntriesAppear)
{
    store.Append(LogSeverity::Info, "Engine", "before");
    Refresh();
    Click("log-clear");
    EXPECT_TRUE(Rows().empty());
    EXPECT_EQ(store.Freeze()->entries.size(), 1u);
    store.Append(LogSeverity::Info, "Engine", "after");
    Refresh();
    ASSERT_EQ(Rows().size(), 1u);
    EXPECT_EQ(Rows().front()->GetAttribute<uint64_t>("data-sequence", 0), 2u);
}

TEST_F(EditorLogPanelTest, IdleFramesKeepRowsAndHiddenPanelsDeferNewRows)
{
    store.Append(LogSeverity::Info, "Engine", "idle");
    Refresh();
    Refresh();
    const auto count = panel.RowsBuildCount();
    auto* row = Rows().front();
    for (size_t index = 0; index < 10; ++index) Refresh();
    EXPECT_EQ(panel.RowsBuildCount(), count);
    EXPECT_EQ(Rows().front(), row);
    ASSERT_TRUE(docking.SetPanelVisible("log", false));
    store.Append(LogSeverity::Error, "Script", "hidden");
    Refresh();
    EXPECT_EQ(panel.RowsBuildCount(), count);
    ASSERT_TRUE(docking.SetPanelVisible("log", true));
    Refresh();
    EXPECT_GT(panel.RowsBuildCount(), count);
    EXPECT_EQ(Rows().size(), 2u);
}

TEST_F(EditorLogPanelTest, LargeOutputMountsABoundedWindowAndCanReachBothEnds)
{
    for (size_t index = 0; index < 4096; ++index)
        store.Append(LogSeverity::Info, "Script", "row " + std::to_string(index));
    Refresh();
    Refresh();
    Refresh();
    ASSERT_FALSE(Rows().empty());
    EXPECT_LE(Rows().size(), EditorLogPanel::MaxMountedRows);
    EXPECT_EQ(Rows().back()->GetAttribute<uint64_t>("data-sequence", 0), 4096u);
    Click("log-autoscroll");
    EXPECT_FALSE(document->GetElementById("log-autoscroll")->HasAttribute("checked"));
    auto* scroll = document->GetElementById("log-scroll");
    scroll->SetScrollTop(0);
    Refresh();
    EXPECT_EQ(Rows().front()->GetAttribute<uint64_t>("data-sequence", 0), 1u);
    store.Append(LogSeverity::Info, "Script", "new tail");
    Refresh();
    EXPECT_LT(scroll->GetScrollTop(), EditorLogPanel::RowHeight);
    Click("log-autoscroll");
    Refresh();
    Refresh();
    EXPECT_EQ(Rows().back()->GetAttribute<uint64_t>("data-sequence", 0), 4097u);
}

TEST_F(EditorLogPanelTest, ScriptsViewBoundsObjectRowsAndExposesTimingAndFailures)
{
    nlohmann::json performance{{"time", {{"deltaTime", 0.01}, {"timeScale", 0.5}}},
        {"physicsMs", 0.3}, {"blueprintCount", 300}, {"truncated", true},
        {"blueprints", nlohmann::json::array()}};
    for (size_t index = 0; index < 100; ++index)
        performance["blueprints"].push_back({{"id", "bp-" + std::to_string(index)},
            {"name", "Mover"}, {"type", "MoverBlueprint"}, {"failed", true},
            {"lastException", "<Failure>"}, {"callbacks", {{{"phase", "Update"}, {"calls", 5},
                {"lastMs", 0.2}, {"totalMs", 1.0}, {"maxMs", 0.4}}}}});
    performance["blueprints"][0]["components"] = nlohmann::json::array({
        {{"id", "health"}, {"type", "HealthComponent"}, {"lifecycle", "Playing"},
            {"callbacks", {{{"phase", "FixedUpdate"}, {"calls", 10},
                {"lastMs", 0.1}, {"totalMs", 1.0}, {"maxMs", 0.2}}}}}});
    Click("log-scripts-tab");
    panel.BuildPerformance(performance, true);
    context->Update();
    Rml::ElementList rows;
    document->GetElementById("log-scripts-rows")->QuerySelectorAll(rows, ".log-script-row");
    EXPECT_EQ(rows.size(), 64u);
    EXPECT_EQ(document->QuerySelector("Failure"), nullptr);
    EXPECT_NE(rows.front()->GetInnerRML().find("Update"), std::string::npos);
    EXPECT_NE(rows[1]->GetInnerRML().find("FixedUpdate"), std::string::npos);
    EXPECT_NE(document->GetElementById("log-scripts-summary")->GetInnerRML().find("0.500"), std::string::npos);
    EXPECT_NE(document->GetElementById("log-scripts-summary")->GetInnerRML().find("300"), std::string::npos);
    EXPECT_NE(document->GetElementById("log-scripts-summary")->GetInnerRML().find("snapshot truncated"), std::string::npos);
    Click("log-scripts-next");
    panel.BuildPerformance(performance, true);
    context->Update();
    rows.clear();
    document->GetElementById("log-scripts-rows")->QuerySelectorAll(rows, ".log-script-row");
    ASSERT_EQ(rows.size(), 37u);
    EXPECT_NE(rows.back()->GetInnerRML().find("bp-99"), std::string::npos);
    Set("log-scripts-search", "HealthComponent");
    panel.BuildPerformance(performance, true);
    context->Update();
    rows.clear();
    document->GetElementById("log-scripts-rows")->QuerySelectorAll(rows, ".log-script-row");
    ASSERT_EQ(rows.size(), 1u);
    EXPECT_NE(rows.front()->GetInnerRML().find("health"), std::string::npos);
}

TEST(LogDockMigrationTest, LegacyLayoutGainsLogWithoutChangingRatiosOrActivePanel)
{
    const auto path = std::filesystem::temp_directory_path() /
        ("vl-log-layout-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()) + ".json");
    {
        std::ofstream stream(path);
        stream << R"({"version":1,"root":{"axis":"vertical","ratio":0.7,"first":{"tabs":["viewport","outliner","details"],"active":"outliner"},"second":{"tabs":["content","renderer"],"active":"renderer"}},"floating":[],"hidden":["details"]})";
    }
    DockLayoutState restored;
    std::vector<std::string> hidden;
    std::string error;
    const bool read = ReadWorkspaceLayout(path, restored, error, &hidden);
    std::error_code cleanup;
    std::filesystem::remove(path, cleanup);
    ASSERT_TRUE(read) << error;
    DockWorkspace workspace;
    ASSERT_TRUE(workspace.Restore(restored, error)) << error;
    EXPECT_EQ(workspace.FindStack("log"), workspace.FindStack("content"));
    EXPECT_EQ(workspace.State().nodes.at(workspace.FindStack("content")).active, "renderer");
    EXPECT_FLOAT_EQ(workspace.State().nodes.at(workspace.State().root).ratio, 0.7f);
    EXPECT_EQ(hidden, std::vector<std::string>{"details"});
}
} // namespace
