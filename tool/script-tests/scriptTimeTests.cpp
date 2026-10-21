#include <gtest/gtest.h>
#include <limits>
#include <algorithm>
#include <chrono>
#include <iostream>
#include <cstdlib>
#include "script/scriptRuntime.h"
#include "world/world.h"
#include "sceneNode.h"
#include "input/inputSubsystem.h"
#include "engine/diagnosticsSubsystem.h"
#include "core/logStore.h"

namespace
{
using namespace VL;
using Json = nlohmann::json;

#ifdef VULKANLEARN_ENABLE_CSHARP_BLUEPRINTS
class ScriptTimeTest : public testing::Test
{
protected:
    void SetUp() override
    {
        runtime.Initialize(true, VL_SCRIPT_OUTPUT_DIRECTORY,
            std::string(VL_SCRIPT_OUTPUT_DIRECTORY) + "/ScriptFixtures.dll");
        world = std::make_shared<World>(1, "time");
        world->SetCamera(std::make_shared<Camera>());
    }
    void TearDown() override { runtime.Shutdown(); }
    Json Blueprint(const std::string& id = "probe", const std::string& type = "ScriptFixtures.TimeProbeBlueprint")
    {
        return {{"type", "blueprintInstance"}, {"id", id}, {"name", id}, {"class", type},
            {"position", {0, 0, 0}}, {"rotation", {0, 0, 0}}, {"scale", {1, 1, 1}},
            {"properties", Json::object()}, {"components", Json::array()}};
    }
    void Prepare(const Json& blueprints)
    {
        runtime.PrepareWorld(*world, {{"objects", blueprints}});
        runtime.SynchronizeWorld(world);
    }
    void Control(ScriptPlayState state)
    {
        std::string error;
        ASSERT_TRUE(runtime.Control(*world, state, error)) << error;
    }
    void Tick(float dt) { runtime.Tick(world, dt, {}, diagnostics); }
    Json State(const std::string& id = "probe")
    {
        for (const auto& state : world->GetScriptBlueprintStates())
            if (state.at("id") == id) return state;
        throw std::runtime_error("Missing time probe");
    }
    Json Performance() { return runtime.View(world->GetGeneration()).performance; }
    void BenchmarkFrames(int moving);
    Json Metric(const std::string& id)
    {
        // 回调次数断言显式读取完整诊断；生产 UI 的定频采样由独立用例验证。
        const auto performance = world->GetScriptContext()->Call("runtime_snapshot").at("result");
        for (const auto& metric : performance.at("blueprints"))
            if (metric.at("id") == id) return metric;
        throw std::runtime_error("Missing performance metric");
    }
    Json Timing(const Json& metric, const std::string& phase)
    {
        for (const auto& timing : metric.at("callbacks"))
            if (timing.at("phase") == phase) return timing;
        throw std::runtime_error("Missing callback timing");
    }
    ScriptRuntimeSubsystem runtime;
    std::shared_ptr<World> world;
    DiagnosticsSubsystem diagnostics;
};

TEST_F(ScriptTimeTest, TickAndLateTickRunOncePerFrameWithVariableDeltaTime)
{
    Prepare(Json::array({Blueprint()}));
    Control(ScriptPlayState::Playing);
    Tick(1.0f / 120);
    EXPECT_EQ(State()["rotation"][0], 1);
    EXPECT_EQ(Performance()["time"]["frameIndex"], 1);
    EXPECT_NEAR(State()["position"][0].get<float>(), 1.0f / 120, 1e-6f);
    Tick(1.0f / 30);
    EXPECT_EQ(State()["rotation"][0], 2);
    EXPECT_EQ(Performance()["time"]["frameIndex"], 2);
    EXPECT_NEAR(State()["position"][0].get<float>(), 1.0f / 120 + 1.0f / 30, 1e-6f);
    Tick(0.25f);
    EXPECT_EQ(State()["rotation"][0], 3);
    EXPECT_EQ(State()["scale"][1], 4); // 1 + Tick 次数：每个渲染帧恰好一次，不随 dt 拆成子步。
    EXPECT_EQ(Performance()["time"]["frameIndex"], 3);
    // LateTick 与 Tick 共用本帧时钟：position.x 是累加的 play 时间，rotation.x 是 Tick 次数。
    EXPECT_NEAR(State()["position"][0].get<float>(), 1.0f / 120 + 1.0f / 30 + 0.25f, 1e-6f);
    const auto time = Performance()["time"];
    EXPECT_FALSE(time.contains("fixedFrameIndex"));
    EXPECT_FALSE(time.contains("fixedSteps"));
    EXPECT_FALSE(time.contains("droppedSteps"));
    EXPECT_FALSE(time.contains("droppedTime"));
    EXPECT_EQ(Timing(Metric("probe"), "Tick")["calls"], 3);
    EXPECT_EQ(Timing(Metric("probe"), "LateTick")["calls"], 3);
}

TEST_F(ScriptTimeTest, OneFrameVariableDeltaAdvancesRigidBodyByFrameTime)
{
    // 每帧变长 dt 直接交给物理世界：刚体内部的 1/60 子步会补足 4/60 秒，不依赖脚本子步。
    auto probe = Blueprint("probe", "ScriptFixtures.PhysicsBlueprint");
    probe["physics"] = {{"motion", "dynamic"}, {"shape", "sphere"}, {"radius", 10}, {"mass", 1}};
    probe["properties"]["ImpulseX"] = 120;
    Prepare(Json::array({probe}));
    Control(ScriptPlayState::Playing);
    Tick(4.0f / 60);
    EXPECT_FALSE(State()["failed"].get<bool>()) << runtime.View(1).feedback;
    EXPECT_NEAR(State()["position"][0].get<float>(), 8, 0.05f);
    Tick(1.0f / 60);
    EXPECT_NEAR(State()["position"][0].get<float>(), 10, 0.05f);
}

TEST_F(ScriptTimeTest, TimeScaleChangesScaledClockAndTimers)
{
    auto probe = Blueprint();
    probe["properties"]["Services"] = true;
    Prepare(Json::array({probe}));
    Control(ScriptPlayState::Playing);
    std::string error;
    ASSERT_TRUE(runtime.SetTimeScale(0.5f, error));
    Tick(0.1f);
    EXPECT_NEAR(State()["position"][0].get<float>(), 0.05f, 1e-6f);
    EXPECT_NEAR(State()["position"][1].get<float>(), 0.1f, 1e-6f);
    EXPECT_EQ(State()["rotation"][0], 1);
    EXPECT_EQ(State()["scale"][0], 1);
    Tick(0.1f);
    EXPECT_EQ(State()["scale"][0], 2);
    EXPECT_EQ(State()["rotation"][0], 2);
    ASSERT_TRUE(runtime.SetTimeScale(2, error));
    Tick(1.0f / 60);
    EXPECT_EQ(Performance()["time"]["frameIndex"], 3);
    EXPECT_NEAR(State()["position"][0].get<float>(), 0.1f + 2.0f / 60, 1e-6f);
    EXPECT_FLOAT_EQ(runtime.View(1).timeScale, 2);
}

TEST_F(ScriptTimeTest, ZeroScaleKeepsUnscaledCallbacksAndFreezesPhysicsAndTimers)
{
    auto probe = Blueprint(), body = Blueprint("body", "ScriptFixtures.PassiveBlueprint");
    probe["properties"]["Services"] = true;
    body["physics"] = {{"motion", "dynamic"}, {"shape", "sphere"}, {"radius", 10}, {"mass", 1}};
    body["position"] = {0, 1000, 0};
    Prepare(Json::array({probe, body}));
    Control(ScriptPlayState::Playing);
    std::string error;
    ASSERT_TRUE(runtime.SetTimeScale(0, error));
    for (int index = 0; index < 3; ++index) Tick(0.1f);
    EXPECT_EQ(State()["position"][0], 0);
    EXPECT_NEAR(State()["position"][1].get<float>(), 0.3f, 1e-6f);
    EXPECT_EQ(State()["scale"][0], 1);
    EXPECT_EQ(State()["scale"][1], 4);
    EXPECT_EQ(State("body")["position"][1], 1000);
    // 帧索引只数渲染帧；零时间倍率不冻结帧推进，只冻结脚本/物理时间。
    EXPECT_EQ(Performance()["time"]["frameIndex"], 3);
    EXPECT_EQ(Performance()["pending"]["timers"], 1);
    ASSERT_TRUE(runtime.SetTimeScale(1, error));
    Tick(0.1f);
    EXPECT_EQ(State()["scale"][0], 2);
}

TEST_F(ScriptTimeTest, PauseKeepsFrameClockAndStopResetsPlayClockAndScale)
{
    Prepare(Json::array({Blueprint()}));
    Tick(2);
    Control(ScriptPlayState::Playing);
    Tick(1.0f / 120);
    Control(ScriptPlayState::Paused);
    const auto frozen = State();
    Tick(1);
    EXPECT_EQ(State(), frozen);
    EXPECT_EQ(Performance()["time"]["frameIndex"], 1);
    EXPECT_GT(Performance()["time"]["timeSinceStartup"].get<double>(), 3);
    Control(ScriptPlayState::Playing);
    Tick(1.0f / 120);
    EXPECT_EQ(State()["rotation"][0], 2);
    std::string error;
    ASSERT_TRUE(runtime.SetTimeScale(0.25f, error));
    const double startup = Performance()["time"]["timeSinceStartup"];
    Control(ScriptPlayState::Editing);
    EXPECT_FLOAT_EQ(runtime.View(1).timeScale, 1);
    EXPECT_EQ(Performance()["time"]["timeSincePlay"], 0);
    EXPECT_DOUBLE_EQ(Performance()["time"]["timeSinceStartup"].get<double>(), startup);
    Control(ScriptPlayState::Playing);
    Tick(1.0f / 120);
    EXPECT_EQ(State()["rotation"][0], 1);
}

TEST_F(ScriptTimeTest, InvalidTimeScalePreservesCurrentValue)
{
    Prepare(Json::array({Blueprint()}));
    std::string error;
    ASSERT_TRUE(runtime.SetTimeScale(2, error));
    for (float value : {-1.0f, 101.0f, std::numeric_limits<float>::infinity(),
        std::numeric_limits<float>::quiet_NaN()})
    {
        EXPECT_FALSE(runtime.SetTimeScale(value, error));
        EXPECT_FALSE(error.empty());
        EXPECT_FLOAT_EQ(runtime.View(1).timeScale, 2);
    }
}

TEST_F(ScriptTimeTest, ThrowingTickRollsBackServicesAndRetainsValueDiagnostics)
{
    // Tick 抛异常沿同一条回滚路径：变换、计时器、订阅和一次性命令都不得泄漏到下一帧。
    auto bad = Blueprint("bad", "ScriptFixtures.ThrowingFrameBlueprint"), good = Blueprint("good");
    Prepare(Json::array({bad, good}));
    Control(ScriptPlayState::Playing);
    ProcessLog().Clear();
    Tick(2.0f / 60);
    EXPECT_TRUE(State("bad")["failed"].get<bool>());
    EXPECT_EQ(State("bad")["position"], Json::array({0, 0, 0}));
    EXPECT_FALSE(State("good")["failed"].get<bool>());
    EXPECT_EQ(State("good")["rotation"][0], 1);
    const auto metric = Metric("bad");
    EXPECT_EQ(metric["lifecycle"], "Failed");
    EXPECT_NE(metric["lastException"].get<std::string>().find("inherited-frame-failed"), std::string::npos);
    EXPECT_EQ(Timing(metric, "Tick")["calls"], 1);
    EXPECT_GE(Timing(metric, "Tick")["lastMs"].get<double>(), 0);
    EXPECT_EQ(Performance()["pending"]["timers"], 0);
    EXPECT_EQ(Performance()["pending"]["subscriptions"], 0);
    EXPECT_GT(Performance()["gc"]["heapBytes"].get<int64_t>(), 0);
    const auto logs = ProcessLog().Freeze();
    ASSERT_EQ(logs->entries.size(), 1u);
    EXPECT_EQ(logs->entries[0].severity, LogSeverity::Error);
    const auto diagnostic = Json::parse(logs->entries[0].message);
    EXPECT_EQ(diagnostic["worldGeneration"], 1);
    EXPECT_EQ(diagnostic["scenePath"], "time");
    EXPECT_EQ(diagnostic["exceptionType"], "System.InvalidOperationException");
    EXPECT_EQ(diagnostic["action"], "DisableBlueprint");
    EXPECT_NE(diagnostic["assembly"].get<std::string>().find("ScriptFixtures.dll"), std::string::npos);
    EXPECT_GT(diagnostic["assemblyGeneration"].get<uint64_t>(), 0);
    EXPECT_EQ(Performance()["detailsFrame"], 1);
    Tick(1.0f / 60);
    EXPECT_EQ(Timing(Metric("bad"), "Tick")["calls"], 1);
}

TEST_F(ScriptTimeTest, PerformanceSnapshotsAreFrozenAndStopReleasesServicesAcrossRepeatedPlay)
{
    auto probe = Blueprint();
    probe["properties"]["Services"] = true;
    Prepare(Json::array({probe}));
    for (int iteration = 0; iteration < 4; ++iteration)
    {
        Control(ScriptPlayState::Playing);
        Tick(1.0f / 60);
        const auto frozen = runtime.View(1);
        EXPECT_EQ(frozen.performance["pending"]["timers"], 1);
        EXPECT_EQ(frozen.performance["pending"]["subscriptions"], 1);
        EXPECT_EQ(frozen.performance["assembly"]["contexts"], 1);
        EXPECT_EQ(Timing(Metric("probe"), "OnCreate")["calls"], 1);
        Tick(1.0f / 60);
        EXPECT_EQ(frozen.performance["time"]["frameIndex"], 1);
        Control(ScriptPlayState::Editing);
        EXPECT_TRUE(Performance()["blueprints"].empty());
        EXPECT_EQ(Performance()["pending"]["timers"], 0);
        EXPECT_EQ(Performance()["pending"]["subscriptions"], 0);
    }
}

TEST_F(ScriptTimeTest, DetailedProfilerIsSampledWhileSummaryAndFrameCountersStayCurrent)
{
    Prepare(Json::array({Blueprint("idle", "ScriptFixtures.PassiveBlueprint")}));
    Control(ScriptPlayState::Playing);
    const auto initial = Performance();
    ASSERT_EQ(initial["detailsFrame"], 0);
    Tick(0.01f);
    EXPECT_EQ(Performance()["detailsFrame"], 0);
    EXPECT_EQ(Performance()["time"]["frameIndex"], 1);
    EXPECT_EQ(Performance()["pipeline"]["detailSamples"], 0);
    EXPECT_GT(Performance()["detailsAgeSeconds"].get<double>(), 0);
    for (int frame = 1; frame < 10; ++frame) Tick(0.01f);
    EXPECT_EQ(Performance()["detailsFrame"], 10);
    EXPECT_EQ(Performance()["pipeline"]["detailSamples"], 1);
    EXPECT_DOUBLE_EQ(Performance()["detailsAgeSeconds"].get<double>(), 0);
    EXPECT_EQ(initial["detailsFrame"], 0);
}

TEST_F(ScriptTimeTest, PausedProfilerDoesNotResampleOrAdvanceDetailedSnapshotAge)
{
    Prepare(Json::array({Blueprint("idle", "ScriptFixtures.PassiveBlueprint")}));
    Control(ScriptPlayState::Playing);
    Tick(0.01f);
    const auto sampled = Performance();
    Control(ScriptPlayState::Paused);
    Tick(1);
    EXPECT_EQ(Performance()["detailsFrame"], sampled["detailsFrame"]);
    EXPECT_EQ(Performance()["detailsAgeSeconds"], sampled["detailsAgeSeconds"]);
    EXPECT_EQ(Performance()["pipeline"]["calls"], 0);
}

void ScriptTimeTest::BenchmarkFrames(int moving)
{
    auto blueprints = Json::array();
    for (int index = 0; index < 1000; ++index)
        blueprints.push_back(Blueprint("load-" + std::to_string(index),
            index < moving ? "ScriptFixtures.DeltaMovementBlueprint" : "ScriptFixtures.PassiveBlueprint"));
    Prepare(blueprints);
    Control(ScriptPlayState::Playing);
    for (int index = 0; index < 3; ++index) Tick(1.0f / 60);
    const auto allocatedBefore = Performance().at("gc").at("allocatedBytes").get<uint64_t>();
    std::vector<double> frames;
    double total = 0;
    Json phases = Json::object();
    for (int index = 0; index < 30; ++index)
    {
        const auto started = std::chrono::steady_clock::now();
        Tick(1.0f / 60);
        const double milliseconds = std::chrono::duration<double, std::milli>(
            std::chrono::steady_clock::now() - started).count();
        frames.push_back(milliseconds);
        total += milliseconds;
        const auto current = Performance();
        for (const auto& field : current.at("pipeline").items())
            phases[field.key()] = phases.value(field.key(), 0.0) + field.value().get<double>() / 30;
        phases["physicsMs"] = phases.value("physicsMs", 0.0) + current.at("physicsMs").get<double>() / 30;
    }
    ASSERT_EQ(world->GetScriptBlueprintStates().size(), 1000u);
    for (const auto& state : world->GetScriptBlueprintStates()) EXPECT_FALSE(state.at("failed").get<bool>());
    const auto performance = Performance();
    EXPECT_EQ(performance["blueprintCount"], 1000);
    EXPECT_EQ(performance["blueprints"].size(), 128u);
    EXPECT_TRUE(performance["truncated"].get<bool>());
    EXPECT_LT(performance.dump().size(), 1024u * 1024u);
    const auto allocationPerFrame =
        (performance.at("gc").at("allocatedBytes").get<uint64_t>() - allocatedBefore) / 30;
    EXPECT_EQ(performance["pipeline"]["updates"], moving);
    EXPECT_LT(performance["pipeline"]["requestBytes"].get<uint64_t>(), 4096u);
    for (int index = 0; index < 1000; ++index)
        EXPECT_NEAR(world->GetScriptBlueprintStates()[index]["position"][0].get<float>(),
            index < moving ? 33.0f / 60 : 0, 1e-5f);
    if (moving == 0)
    {
        EXPECT_LT(performance["pipeline"]["responseBytes"].get<uint64_t>(), 128u * 1024u);
        EXPECT_LT(allocationPerFrame, 512u * 1024u);
    }
    std::sort(frames.begin(), frames.end());
    const double mean = total / frames.size(), p95 = frames[28], maximum = frames.back();
    testing::Test::RecordProperty("blueprints", 1000);
    testing::Test::RecordProperty("moving", moving);
    testing::Test::RecordProperty("allocatedBytesPerFrame", std::to_string(allocationPerFrame));
    testing::Test::RecordProperty("measuredFrames", 30);
    testing::Test::RecordProperty("meanMs", std::to_string(mean));
    testing::Test::RecordProperty("p95Ms", std::to_string(p95));
    testing::Test::RecordProperty("maxMs", std::to_string(maximum));
    std::cout << "1000 Blueprints (" << moving << " moving), native Tick/JSON bridge/World synchronization/profiler, 3 warm + 30 measured: "
        << "mean=" << mean << " ms, p95=" << p95 << " ms, max=" << maximum << " ms\n";
    std::cout << "Mean pipeline: " << phases.dump() << ", allocatedBytesPerFrame=" << allocationPerFrame << '\n';
#ifdef NDEBUG
    // 墙钟预算只在显式 Release 验收启用；普通模块门禁使用稳定的更新数/字节/分配预算。
    if (moving == 0)
        if (const char* budget = std::getenv("VL_SCRIPT_IDLE_MAX_P95_MS"))
            EXPECT_LT(p95, std::stod(budget));
#endif
}

TEST_F(ScriptTimeTest, ThousandIdleBlueprintsKeepProfilerBoundedWithRecordedFrameTimings)
{
    BenchmarkFrames(0);
}

TEST_F(ScriptTimeTest, ThousandMovingBlueprintsPublishOnlyChangedPositions)
{
    BenchmarkFrames(1000);
}

TEST_F(ScriptTimeTest, ThousandMixedBlueprintsPublishOnlyActiveOwners)
{
    BenchmarkFrames(100);
}
#endif
} // namespace
