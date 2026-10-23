#include <gtest/gtest.h>

#include <cmath>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include <RmlUi/Core/Context.h>
#include <RmlUi/Core/Core.h>
#include <RmlUi/Core/ElementDocument.h>
#include <RmlUi/Core/EventListener.h>
#include <RmlUi/Core/RenderInterface.h>
#include <RmlUi/Core/SystemInterface.h>

#include "core/performanceSnapshot.h"
#include "editor/ui/editorDockingController.h"
#include "editor/ui/editorPerformancePanel.h"

namespace
{
using namespace VL;
using namespace VL::EditorUi;

class PerformanceTestSystem final : public Rml::SystemInterface
{
public:
    bool LogMessage(Rml::Log::Type type, const Rml::String& message) override
    {
        if (type <= Rml::Log::LT_WARNING) errors.push_back(message);
        return true;
    }
    std::vector<std::string> errors;
};

class PerformanceTestRenderer final : public Rml::RenderInterface
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

class EditorPerformancePanelTest : public testing::Test
{
protected:
    void SetUp() override
    {
        Rml::SetSystemInterface(&system);
        Rml::SetRenderInterface(&renderer);
        ASSERT_TRUE(Rml::Initialise());
        ASSERT_TRUE(Rml::LoadFontFace("C:/Windows/Fonts/segoeui.ttf"));
        context = Rml::CreateContext("performance-test", {1280, 720});
        ASSERT_NE(context, nullptr);
        document = context->LoadDocument(
            std::string(VL_EDITOR_ASSET_ROOT) + "/editor-workspace.rml");
        ASSERT_NE(document, nullptr);
        docking.SetPersistencePath({});
        std::string error;
        ASSERT_TRUE(docking.Attach(*document, error)) << error;
        docking.Activate("performance");
        ASSERT_TRUE(panel.Attach(*document, error)) << error;
        document->Show();
        context->Update();
    }

    void TearDown() override
    {
        panel.Detach();
        docking.Detach();
        if (context) Rml::RemoveContext(context->GetName());
        Rml::Shutdown();
        Rml::SetRenderInterface(nullptr);
        Rml::SetSystemInterface(nullptr);
    }

    void Refresh(double nowSeconds)
    {
        docking.Sync();
        panel.Build(snapshot, docking.IsPanelActive("performance"), nowSeconds);
        context->Update();
    }

    std::string Text(const std::string& id) const
    {
        auto* element = document->GetElementById(id);
        return element == nullptr ? std::string("<missing>") : element->GetInnerRML();
    }

    Rml::ElementList Rows() const
    {
        Rml::ElementList rows;
        auto* host = document->GetElementById("performance-passes");
        if (host != nullptr) host->QuerySelectorAll(rows, ".performance-row");
        return rows;
    }

    PerformanceTestSystem system;
    PerformanceTestRenderer renderer;
    Rml::Context* context = nullptr;
    Rml::ElementDocument* document = nullptr;
    EditorDockingController docking;
    EditorPerformancePanel panel;
    PerformanceSnapshot snapshot;
};

TEST_F(EditorPerformancePanelTest, PopulatedSnapshotMapsSummaryRowsPercentagesAndBars)
{
    snapshot = BuildPerformanceSnapshot(
        {{"hairSimulation", 0.18, 120}, {"shadowCascade0", std::nullopt, 0},
            {"deferredLighting", 8.0, 120}},
        120, true, true, 1.0 / 60.0);
    Refresh(0.0);

    // 面板与 JSON 共用同一个 FormatPerformanceNumber：同快照同数字。
    EXPECT_EQ(Text("performance-fps"), "60.000");
    EXPECT_EQ(Text("performance-cpu-ms"), "16.667 ms");
    EXPECT_EQ(Text("performance-gpu-ms"), "8.180 ms");
    EXPECT_NE(Text("performance-status").find("120 frames"), std::string::npos);

    const Rml::ElementList rows = Rows();
    ASSERT_EQ(rows.size(), 3u);
    EXPECT_NE(rows[0]->GetInnerRML().find("hairSimulation"), std::string::npos);
    EXPECT_NE(rows[0]->GetInnerRML().find("0.180"), std::string::npos);
    EXPECT_NE(rows[1]->GetInnerRML().find("shadowCascade0"), std::string::npos);
    EXPECT_NE(rows[1]->GetInnerRML().find("n/a"), std::string::npos);
    EXPECT_NE(rows[2]->GetInnerRML().find("deferredLighting"), std::string::npos);

    // 8.0 / 8.18 = 97.8% → 文本保留一位小数，条形按 5% 档落在 100。
    EXPECT_NE(rows[2]->GetInnerRML().find("97.8%"), std::string::npos);
    auto* fullBar = rows[2]->QuerySelector(".performance-bar-fill");
    ASSERT_NE(fullBar, nullptr);
    EXPECT_TRUE(fullBar->IsClassSet("performance-bar-100"));
    auto* emptyBar = rows[1]->QuerySelector(".performance-bar-fill");
    ASSERT_NE(emptyBar, nullptr);
    EXPECT_TRUE(emptyBar->IsClassSet("performance-bar-0"));
}

TEST_F(EditorPerformancePanelTest, RowsKeepGraphPassOrderInsteadOfCostOrder)
{
    snapshot = BuildPerformanceSnapshot(
        {{"cheapFirst", 0.1, 1}, {"expensiveSecond", 5.0, 1}, {"middleThird", 1.0, 1}},
        1, true, true, 0.02);
    Refresh(0.0);
    const Rml::ElementList rows = Rows();
    ASSERT_EQ(rows.size(), 3u);
    EXPECT_NE(rows[0]->GetInnerRML().find("cheapFirst"), std::string::npos);
    EXPECT_NE(rows[1]->GetInnerRML().find("expensiveSecond"), std::string::npos);
    EXPECT_NE(rows[2]->GetInnerRML().find("middleThird"), std::string::npos);
}

TEST_F(EditorPerformancePanelTest, RefreshIsThrottledAndVisibilityChangeRefreshesImmediately)
{
    snapshot = BuildPerformanceSnapshot({{"a", 1.0, 1}}, 1, true, true, 1.0 / 60.0);
    Refresh(0.0);
    const size_t firstBuilds = panel.RowsBuildCount();
    EXPECT_EQ(Text("performance-fps"), "60.000");

    snapshot = BuildPerformanceSnapshot({{"a", 2.0, 1}}, 1, true, true, 1.0 / 30.0);
    Refresh(0.05);
    EXPECT_EQ(panel.RowsBuildCount(), firstBuilds);
    EXPECT_EQ(Text("performance-fps"), "60.000");

    Refresh(0.20);
    EXPECT_EQ(panel.RowsBuildCount(), firstBuilds + 1);
    EXPECT_EQ(Text("performance-fps"), "30.000");

    // 隐藏期间不刷新，也不销毁 DOM；切回该页时立即补一次。
    docking.Activate("content");
    snapshot = BuildPerformanceSnapshot({{"a", 3.0, 1}}, 1, true, true, 1.0 / 120.0);
    Refresh(0.30);
    EXPECT_EQ(panel.RowsBuildCount(), firstBuilds + 1);
    docking.Activate("performance");
    Refresh(0.31);
    EXPECT_EQ(panel.RowsBuildCount(), firstBuilds + 2);
    EXPECT_EQ(Text("performance-fps"), "120.000");
}

TEST_F(EditorPerformancePanelTest, UnsupportedGpuTimingKeepsCpuNumbersAndExplainsWhy)
{
    snapshot = BuildPerformanceSnapshot({}, 0, false, false, 0.02);
    Refresh(0.0);
    EXPECT_EQ(Text("performance-fps"), "50.000");
    EXPECT_EQ(Text("performance-cpu-ms"), "20.000 ms");
    EXPECT_EQ(Text("performance-gpu-ms"), "--");
    EXPECT_NE(Text("performance-status").find("unsupported"), std::string::npos);
    EXPECT_TRUE(Rows().empty());
}

TEST_F(EditorPerformancePanelTest, NotYetSampledGpuChannelShowsWaitingInsteadOfZero)
{
    snapshot = BuildPerformanceSnapshot({{"geometry", std::nullopt, 0}}, 0, true, false, 0.02);
    Refresh(0.0);
    EXPECT_EQ(Text("performance-gpu-ms"), "--");
    EXPECT_NE(Text("performance-status").find("Waiting for GPU samples"), std::string::npos);
    const Rml::ElementList rows = Rows();
    ASSERT_EQ(rows.size(), 1u);
    EXPECT_NE(rows[0]->GetInnerRML().find("n/a"), std::string::npos);
}

TEST_F(EditorPerformancePanelTest, WindowWithoutAnyValidSampleReportsNaNotZero)
{
    snapshot = BuildPerformanceSnapshot(
        {{"geometry", std::nullopt, 0}, {"shadowCascade0", std::nullopt, 0}},
        2, true, true, 0.02);
    Refresh(0.0);
    EXPECT_EQ(Text("performance-gpu-ms"), "--");
    EXPECT_NE(Text("performance-status").find("No valid GPU samples"), std::string::npos);
    const Rml::ElementList rows = Rows();
    ASSERT_EQ(rows.size(), 2u);
    EXPECT_NE(rows[0]->GetInnerRML().find("n/a"), std::string::npos);
    EXPECT_EQ(rows[0]->GetInnerRML().find("0.000"), std::string::npos);
}

TEST_F(EditorPerformancePanelTest, StartBeforeFirstSampleNeverShowsFabricatedZeros)
{
    // 宿主在首帧就调用 Build，但快照还是 Initialize 的空状态。
    Refresh(0.0);
    EXPECT_EQ(Text("performance-fps"), "--");
    EXPECT_EQ(Text("performance-cpu-ms"), "--");
    EXPECT_EQ(Text("performance-gpu-ms"), "--");
    EXPECT_TRUE(Rows().empty());
    EXPECT_TRUE(system.errors.empty()) << system.errors.front();
}

} // namespace
