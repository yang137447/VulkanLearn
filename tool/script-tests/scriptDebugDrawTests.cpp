#include <gtest/gtest.h>
#include "script/scriptRuntime.h"
#include "script/scriptDebugDraw.h"
#include "world/world.h"
#include "sceneNode.h"
#include "input/inputSubsystem.h"
#include "engine/diagnosticsSubsystem.h"

#ifdef VULKANLEARN_ENABLE_CSHARP_BLUEPRINTS
namespace
{
using namespace VL;
using Json = nlohmann::json;
class ScriptDebugDrawTest : public testing::Test
{
protected:
    void SetUp() override
    {
        runtime.Initialize(true, VL_SCRIPT_OUTPUT_DIRECTORY, std::string(VL_SCRIPT_OUTPUT_DIRECTORY) + "/ScriptFixtures.dll");
        world = std::make_shared<World>(1, "Maps/SC_debug/SC_debug.json");
        world->SetCamera(std::make_shared<Camera>());
    }
    void TearDown() override { runtime.Shutdown(); }
    Json Blueprint(int mode = 0, std::string id = "owner")
    {
        return {{"type", "blueprintInstance"}, {"id", id}, {"name", id}, {"class", "ScriptFixtures.DebugDrawProbe"},
            {"position", {0, 0, 0}}, {"rotation", {0, 0, 0}}, {"scale", {1, 1, 1}},
            {"properties", {{"Mode", mode}}}, {"components", Json::array()}};
    }
    Json Component(std::string id, bool fail = false)
    { return {{"id", id}, {"class", "ScriptFixtures.DebugDrawProbeComponent"}, {"properties", {{"Fail", fail}}}}; }
    void Prepare(Json declarations)
    {
        runtime.PrepareWorld(*world, {{"objects", declarations}}); runtime.SynchronizeWorld(world);
    }
    void Play()
    {
        std::string error; ASSERT_TRUE(runtime.Control(*world, ScriptPlayState::Playing, error)) << error;
    }
    void Tick(float dt = 1.f / 60)
    {
        if (!runtime.IsPaused()) world->GetDebugDraw().BeginFrame();
        runtime.Tick(world, dt, {}, diagnostics);
        world->GetDebugDraw().EndFrame();
    }
    ScriptRuntimeSubsystem runtime;
    std::shared_ptr<World> world;
    DiagnosticsSubsystem diagnostics;
};
TEST_F(ScriptDebugDrawTest, AllPrimitivesReachNativeWorldAndSingleFrameDoesNotSurviveNextGTFrame)
{
    Prepare(Json::array({Blueprint(16)})); Play();
    const auto frozen = world->GetDebugDraw().Freeze();
    EXPECT_EQ(frozen->points.size(), 1u); EXPECT_EQ(frozen->texts.size(), 1u); EXPECT_GT(frozen->lines.size(), 100u);
    world->GetDebugDraw().EndFrame();
    Tick();
    EXPECT_TRUE(world->GetDebugDraw().Freeze()->lines.empty()); EXPECT_GT(frozen->lines.size(), 100u);
}
TEST_F(ScriptDebugDrawTest, BeginPlayDrawsSurviveFirstFrameAndShutdownClearsRetainedWorld)
{
    Prepare(Json::array({Blueprint(16)})); Play(); Tick();
    EXPECT_GT(world->GetDebugDraw().Count(), 0u);
    runtime.Shutdown();
    EXPECT_EQ(world->GetDebugDraw().Count(), 0u);
}
TEST_F(ScriptDebugDrawTest, KeyReplacementAndHandleRemovalStayWithinPublisher)
{
    auto blueprint = Blueprint(1); blueprint["scriptComponents"] = Json::array({Component("first")});
    Prepare(Json::array({blueprint})); Play(); Tick();
    auto snapshot = world->GetDebugDraw().Freeze();
    ASSERT_EQ(snapshot->points.size(), 2u);
    EXPECT_FLOAT_EQ(snapshot->points[0].position.x(), 1);
    EXPECT_FLOAT_EQ(snapshot->points[1].position.x(), 20);
}
TEST_F(ScriptDebugDrawTest, RemoveAndCategoryClearLeaveSiblingAndUnrelatedCategory)
{
    for (int mode : {2, 3})
    {
        auto blueprint = Blueprint(mode); blueprint["scriptComponents"] = Json::array({Component("first")});
        Prepare(Json::array({blueprint})); Play(); Tick();
        auto snapshot = world->GetDebugDraw().Freeze();
        ASSERT_EQ(snapshot->points.size(), 1u); EXPECT_FLOAT_EQ(snapshot->points[0].position.x(), 20);
        EXPECT_EQ(snapshot->texts.size(), 1u);
        std::string error; ASSERT_TRUE(runtime.Control(*world, ScriptPlayState::Editing, error));
    }
}
TEST_F(ScriptDebugDrawTest, TimedLifetimesPauseAndStopClearsPersistentValues)
{
    Prepare(Json::array({Blueprint()})); Play();
    const auto frozen = world->GetDebugDraw().Freeze();
    Tick(.25f);
    std::string error; ASSERT_TRUE(runtime.Control(*world, ScriptPlayState::Paused, error));
    Tick(10);
    EXPECT_EQ(world->GetDebugDraw().Count(), 2u);
    ASSERT_TRUE(runtime.Control(*world, ScriptPlayState::Playing, error));
    Tick(.25f); EXPECT_EQ(world->GetDebugDraw().Count(), 1u);
    ASSERT_TRUE(runtime.Control(*world, ScriptPlayState::Editing, error));
    EXPECT_EQ(world->GetDebugDraw().Count(), 0u); EXPECT_EQ(frozen->texts.size(), 1u);
}
TEST_F(ScriptDebugDrawTest, DestroyOrFailedCallbackCleansPreviouslyPublishedGeometry)
{
    for (int mode : {4, 5, 9, 15, 19})
    {
        Prepare(Json::array({Blueprint(mode)})); Play(); Tick();
        EXPECT_EQ(world->GetDebugDraw().Count(), 0u) << mode;
        std::string error; ASSERT_TRUE(runtime.Control(*world, ScriptPlayState::Editing, error));
    }
}
TEST_F(ScriptDebugDrawTest, DisabledRemovedOrFailedComponentOnlyCleansThatPublisher)
{
    for (int mode : {0, 6, 7})
    {
        auto blueprint = Blueprint(mode);
        blueprint["scriptComponents"] = Json::array({Component("first", mode == 0), Component("second")});
        Prepare(Json::array({blueprint})); Play(); Tick();
        auto snapshot = world->GetDebugDraw().Freeze();
        EXPECT_EQ(snapshot->points.size(), 2u); EXPECT_EQ(snapshot->texts.size(), 1u);
        std::string error; ASSERT_TRUE(runtime.Control(*world, ScriptPlayState::Editing, error));
    }
}
TEST_F(ScriptDebugDrawTest, RejectsCreateWorkerStaleForeignAndInvalidDepthCalls)
{
    EXPECT_THROW(Prepare(Json::array({Blueprint(10)})), std::exception);
    for (int mode : {11, 14, 8})
    {
        Prepare(Json::array({Blueprint(mode)})); Play(); Tick();
        EXPECT_EQ(world->GetScriptBlueprintStates()[0]["position"], Json::array({1, 0, 0}));
        std::string error; ASSERT_TRUE(runtime.Control(*world, ScriptPlayState::Editing, error));
    }
    Prepare(Json::array({Blueprint(12, "first"), Blueprint(13, "second")})); Play();
    EXPECT_EQ(world->GetScriptBlueprintStates()[1]["position"], Json::array({1, 0, 0}));
    std::string error; ASSERT_TRUE(runtime.Control(*world, ScriptPlayState::Editing, error));
    Prepare(Json::array({Blueprint(12)})); Play();
    ASSERT_TRUE(runtime.Control(*world, ScriptPlayState::Editing, error));
    Prepare(Json::array({Blueprint(13)})); Play();
    EXPECT_EQ(world->GetScriptBlueprintStates()[0]["position"], Json::array({1, 0, 0}));
}
TEST_F(ScriptDebugDrawTest, OptionalDepthReachesNativeLinesPointsAndTextWhileDefaultsRemainOverlay)
{
    Prepare(Json::array({Blueprint(20)})); Play(); Tick();
    const auto view = world->GetDebugDraw().Freeze();
    ASSERT_EQ(view->points.size(), 2u); ASSERT_EQ(view->texts.size(), 2u); ASSERT_EQ(view->lines.size(), 1u);
    EXPECT_EQ(view->points.front().depth, Debug::DepthMode::Overlay);
    EXPECT_EQ(view->points.back().depth, Debug::DepthMode::DepthTest);
    EXPECT_EQ(view->texts.front().depth, Debug::DepthMode::Overlay);
    EXPECT_EQ(view->texts.back().depth, Debug::DepthMode::DepthTest);
    EXPECT_EQ(view->lines.front().depth, Debug::DepthMode::DepthTest);
}
TEST_F(ScriptDebugDrawTest, WorldReplacementClearsOldCollectorAndDoesNotMutateFrozenSnapshot)
{
    Prepare(Json::array({Blueprint()})); Play();
    const auto frozen = world->GetDebugDraw().Freeze();
    auto next = std::make_shared<World>(2, "Maps/SC_empty/SC_empty.json");
    runtime.SynchronizeWorld(next);
    EXPECT_EQ(world->GetDebugDraw().Count(), 0u); EXPECT_EQ(next->GetDebugDraw().Count(), 0u);
    EXPECT_EQ(frozen->points.size(), 1u);
}
TEST_F(ScriptDebugDrawTest, WorldBudgetFailureDoesNotStopGameplayOrPartiallyPublishDebugBatch)
{
    Debug::Style s; s.duration = -1; s.owner = "Native";
    for (int i = 0; i < 128; ++i) world->GetDebugDraw().DrawDebugString({0, 0, 0}, "native", 14, s);
    Prepare(Json::array({Blueprint()})); Play();
    // 原生文字已占满预算；point + timed label 整批拒绝，原生图形保持，脚本仍能 Tick。
    auto view = world->GetDebugDraw().Freeze();
    EXPECT_EQ(view->texts.size(), 128u); EXPECT_TRUE(view->points.empty());
    Tick(); EXPECT_FALSE(runtime.IsEditing());
}
TEST_F(ScriptDebugDrawTest, LifecycleCleanupCommitsEvenWhenNewGeometryBatchIsRejected)
{
    Debug::Style style; style.duration = -1; style.owner = "Script/owner"; style.publisher = "retired:1";
    world->GetDebugDraw().DrawDebugPoint({0, 0, 0}, 8, style);
    const Json commands = Json::array({
        {{"op", "clear"}, {"publisher", "retired:1"}, {"category", nullptr}, {"cleanup", true}},
        {{"op", "draw"}, {"shape", 0}, {"publisher", ""}, {"category", "General"}, {"key", "invalid"},
         {"duration", -1}, {"thickness", -1}, {"color", {0, 1, 1, 1}},
         {"begin", {0, 0, 0}}, {"end", {0, 0, 0}}, {"extents", {1, 1, 1}}, {"orientation", {0, 0, 0, 1}},
         {"radius", 1}, {"halfHeight", 1}, {"size", 8}, {"segments", 24}, {"closed", false},
         {"points", Json::array()}, {"text", ""}}
    });
    EXPECT_THROW(ApplyScriptDebugDraw(world->GetDebugDraw(), {{"id", "owner"}}, commands), std::invalid_argument);
    EXPECT_EQ(world->GetDebugDraw().Count(), 0u);
}
TEST_F(ScriptDebugDrawTest, EachFrameDrawsOneLineAndPauseKeepsSingleFrameLines)
{
    Prepare(Json::array({Blueprint(17)})); Play(); Tick(.05f);
    {
        const auto lines = world->GetDebugDraw().Freeze()->lines;
        ASSERT_EQ(lines.size(), 1u);
        EXPECT_FLOAT_EQ(lines[0].end.x(), 1); // 每帧一次 Tick，端点读取本帧 FrameIndex。
    }
    std::string error; ASSERT_TRUE(runtime.Control(*world, ScriptPlayState::Paused, error));
    Tick(1); EXPECT_EQ(world->GetDebugDraw().Freeze()->lines.size(), 1u);
    ASSERT_TRUE(runtime.Control(*world, ScriptPlayState::Playing, error));
    Tick(0);
    const auto lines = world->GetDebugDraw().Freeze()->lines;
    ASSERT_EQ(lines.size(), 1u);
    EXPECT_FLOAT_EQ(lines[0].end.x(), 2); // dt=0 仍是一帧：Tick 与 FrameIndex 照常推进一次。
}
TEST_F(ScriptDebugDrawTest, ZeroScaleFreezesTimedExpiryWhileGameplayCallbacksContinue)
{
    Prepare(Json::array({Blueprint(1)})); Play();
    std::string error; ASSERT_TRUE(runtime.SetTimeScale(0, error));
    Tick(1); EXPECT_EQ(world->GetDebugDraw().Count(), 2u);
    EXPECT_FLOAT_EQ(world->GetDebugDraw().Freeze()->points[0].position.x(), 1);
    ASSERT_TRUE(runtime.SetTimeScale(1, error));
    Tick(.5f); EXPECT_EQ(world->GetDebugDraw().Count(), 1u);
}
TEST_F(ScriptDebugDrawTest, SuccessfulPausedAssemblyReloadClearsOldScriptDrawingsButKeepsNative)
{
    Prepare(Json::array({Blueprint()})); Play();
    Debug::Style s; s.owner = "Native"; s.duration = -1;
    world->GetDebugDraw().DrawDebugPoint({0, 0, 0}, 8, s);
    std::string error; ASSERT_TRUE(runtime.Control(*world, ScriptPlayState::Paused, error));
    ASSERT_TRUE(runtime.ReloadAssembly(*world, error)) << error;
    EXPECT_EQ(world->GetDebugDraw().Count(), 1u);
    EXPECT_TRUE(runtime.IsPaused());
}
TEST_F(ScriptDebugDrawTest, OnReloadCannotQueueDebugAndRejectedCandidateKeepsActiveDrawings)
{
    Prepare(Json::array({Blueprint(18)})); Play();
    std::string error;
    EXPECT_FALSE(runtime.ReloadAssembly(*world, error));
    EXPECT_EQ(world->GetDebugDraw().Count(), 2u);
    EXPECT_FALSE(runtime.IsEditing());
}
}
#endif
