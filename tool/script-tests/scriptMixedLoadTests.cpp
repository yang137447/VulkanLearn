#include <gtest/gtest.h>
#include <algorithm>
#include <chrono>
#include <cmath>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>
#include "script/scriptBlueprintAsset.h"
#include "script/scriptRuntime.h"
#include "world/world.h"
#include "world/worldSnapshotBuilder.h"
#include "engine/diagnosticsSubsystem.h"
#include "input/inputSubsystem.h"
#include "sceneNode.h"

namespace
{
using namespace VL;
using Json = nlohmann::json;

#ifdef VULKANLEARN_ENABLE_CSHARP_BLUEPRINTS
constexpr int BodyCount = 64;
constexpr int MovingMeshCount = 64;
constexpr int TimerCount = 8;
constexpr float FrameDelta = 1.0f / 60;

Json MixedBlueprint(const std::string& id, const std::string& type = "ScriptFixtures.PassiveBlueprint")
{
    return {{"type", "blueprintInstance"}, {"id", id}, {"name", id}, {"class", type},
        {"position", {0, 0, 0}}, {"rotation", {0, 0, 0}}, {"scale", {1, 1, 1}},
        {"properties", Json::object()}, {"components", Json::array()}};
}

Json MixedMeshTree()
{
    return Json::array({
        {{"id", "arm"}, {"type", "scene"}, {"position", {0, 20, 0}}, {"scale", {1, 2, 1}}},
        {{"id", "visual"}, {"type", "mesh"}, {"parent", "arm"}, {"position", {5, 0, 0}},
            {"rotation", {0, 15, 0}}, {"modelPath", "Common/Meshes/SM_mixed_test.json"}}});
}

Json MixedDeclarations()
{
    auto result = Json::array();
    auto floor = MixedBlueprint("floor");
    floor["position"] = {0, -50, 0};
    floor["physics"] = {{"motion", "static"}, {"shape", "box"}, {"halfExtents", {10000, 50, 10000}}};
    result.push_back(floor);
    for (int index = 0; index < BodyCount; ++index)
    {
        auto body = MixedBlueprint("body-" + std::to_string(index), "ScriptFixtures.DeltaImpulseBlueprint");
        body["position"] = {index % 8 * 200, 100, index / 8 * 200};
        body["physics"] = {{"motion", "dynamic"}, {"shape", "sphere"}, {"radius", 10}, {"mass", 1}};
        body["components"] = MixedMeshTree();
        result.push_back(std::move(body));
    }
    for (int index = 0; index < MovingMeshCount; ++index)
    {
        auto mover = MixedBlueprint("mesh-" + std::to_string(index), "ScriptFixtures.DeltaMovementBlueprint");
        mover["position"] = {0, 500, index * 200};
        mover["components"] = MixedMeshTree();
        result.push_back(std::move(mover));
    }
    for (int index = 0; index < TimerCount; ++index)
    {
        auto timer = MixedBlueprint("timer-" + std::to_string(index), "ScriptFixtures.TimerBlueprint");
        timer["position"] = {0, 800, index * 200};
        timer["properties"]["Repeat"] = true;
        timer["components"] = MixedMeshTree();
        result.push_back(std::move(timer));
    }
    for (const auto* id : {"sender", "receiver"})
    {
        auto message = MixedBlueprint(id, "ScriptFixtures.MessageBlueprint");
        message["properties"]["Sender"] = std::string(id) == "sender";
        message["components"] = MixedMeshTree();
        result.push_back(std::move(message));
    }
    auto camera = MixedBlueprint("camera", "ScriptFixtures.CameraBlueprint");
    camera["position"] = {0, 300, 1000};
    camera["components"] = Json::array({{{"id", "lens"}, {"type", "camera"}, {"autoActivate", true},
        {"position", {0, 50, 0}}, {"fov", 65}, {"near_clip", 1}, {"far_clip", 10000}}});
    result.push_back(std::move(camera));
    auto light = MixedBlueprint("light", "ScriptFixtures.LightBlueprint");
    light["components"] = Json::array({
        {{"id", "arm"}, {"type", "scene"}, {"position", {100, 200, 0}}},
        {{"id", "lamp"}, {"type", "spotLight"}, {"parent", "arm"}, {"position", {0, 0, 100}},
            {"intensityUnit", "cd"}}});
    result.push_back(std::move(light));
    return result;
}

class ScriptMixedLoadTest : public testing::Test
{
protected:
    void SetUp() override
    {
        runtime.Initialize(true, VL_SCRIPT_OUTPUT_DIRECTORY,
            std::string(VL_SCRIPT_OUTPUT_DIRECTORY) + "/ScriptFixtures.dll");
        declarations = MixedDeclarations();
        world = PrepareWorld(1);
        runtime.SynchronizeWorld(world);
    }
    void TearDown() override { runtime.Shutdown(); }

    std::shared_ptr<World> PrepareWorld(uint64_t generation)
    {
        auto candidate = std::make_shared<World>(generation, "mixed-" + std::to_string(generation));
        candidate->SetCamera(std::make_shared<Camera>());
        // 此处只建立正式 World 的 CPU section；验收 CLR/Jolt 到冻结快照，不声称验证 GPU 绘制。
        for (const auto& blueprint : declarations)
            for (const auto& node : blueprint.at("components"))
                if (node.at("type") == "mesh")
                {
                    const auto identity = ScriptMeshIdentity(blueprint.at("id"), node);
                    const int sections = blueprint.at("id") == "body-0" ? 2 : 1;
                    for (int section = 0; section < sections; ++section)
                    {
                        const auto key = identity + "|section-" + std::to_string(section);
                        WorldMeshObject mesh;
                        mesh.debugName = key;
                        mesh.sceneObjectIdentity = identity;
                        mesh.meshKey = "mixed-mesh";
                        mesh.materialKey = "mixed-material-" + std::to_string(section);
                        mesh.materialInstanceKey = "mixed-instance-" + std::to_string(section);
                        mesh.materialSlotIndex = uint32_t(section);
                        mesh.localBoundsMin = Eigen::Vector3f::Constant(-10);
                        mesh.localBoundsMax = Eigen::Vector3f::Constant(10);
                        mesh.model(1, 3) = float(section * 7);
                        candidate->AddMeshObject(key, std::move(mesh));
                    }
                }
        runtime.PrepareWorld(*candidate, {{"objects", declarations}});
        return candidate;
    }

    void Control(ScriptPlayState state)
    {
        std::string error;
        ASSERT_TRUE(runtime.Control(*world, state, error)) << error;
    }
    void Tick(float dt = FrameDelta) { runtime.Tick(world, dt, {}, diagnostics); }
    void Frames(int count)
    {
        for (int index = 0; index < count; ++index) Tick();
    }
    const Json& State(const std::string& id) const
    {
        for (const auto& value : world->GetScriptBlueprintStates())
            if (value.at("id") == id) return value;
        throw std::runtime_error("Missing mixed-load Blueprint " + id);
    }
    Json Performance() const { return runtime.View(world->GetGeneration()).performance; }
    WorldSnapshot Snapshot()
    {
        WorldSnapshotBuildDesc desc;
        desc.worldGeneration = world->GetGeneration();
        desc.frameIndex = runtime.ContextTime().at("frameIndex");
        auto result = snapshots.Build(*world, desc);
        if (!result.IsSuccess()) throw std::runtime_error("Mixed-load World snapshot failed");
        return std::move(result.Value());
    }
    const MeshDrawSnapshot& Draw(const WorldSnapshot& snapshot, const std::string& key) const
    {
        for (const auto& draw : snapshot.meshDraws)
            if (draw.debugName == key) return draw;
        throw std::runtime_error("Missing mixed-load draw " + key);
    }

    ScriptRuntimeSubsystem runtime;
    std::shared_ptr<World> world;
    Json declarations;
    DiagnosticsSubsystem diagnostics;
    WorldSnapshotBuilder snapshots;
};

TEST_F(ScriptMixedLoadTest, MixedFramesPublishPhysicsVisualsAndServicesIntoIndependentSnapshots)
{
    ASSERT_NO_FATAL_FAILURE(Control(ScriptPlayState::Playing));
    const auto frozen = Snapshot();
    const auto frozenStates = world->GetScriptBlueprintStates();
    ASSERT_EQ(frozen.meshDraws.size(), BodyCount + MovingMeshCount + TimerCount + 3u);
    ASSERT_EQ(frozen.lights.size(), 1u);
    const auto bodyKey = "__blueprint/body-0/visual|section-1";
    const Eigen::Matrix4f frozenBody = Draw(frozen, bodyKey).model;
    Frames(3);
    std::vector<double> elapsed;
    double total = 0, tickTotal = 0, snapshotTotal = 0;
    for (int frame = 0; frame < 30; ++frame)
    {
        const auto begin = std::chrono::steady_clock::now();
        Tick();
        const auto tickEnd = std::chrono::steady_clock::now();
        const auto current = Snapshot();
        const auto end = std::chrono::steady_clock::now();
        const double milliseconds = std::chrono::duration<double, std::milli>(end - begin).count();
        EXPECT_TRUE(std::isfinite(milliseconds));
        EXPECT_GE(milliseconds, 0);
        elapsed.push_back(milliseconds);
        total += milliseconds;
        tickTotal += std::chrono::duration<double, std::milli>(tickEnd - begin).count();
        snapshotTotal += std::chrono::duration<double, std::milli>(end - tickEnd).count();
        EXPECT_EQ(current.meshDraws.size(), frozen.meshDraws.size());
        EXPECT_EQ(current.lights.size(), 1u);
        EXPECT_TRUE(Draw(current, bodyKey).model.isApprox(world->GetMeshObjects().at(bodyKey).model, 1e-5f));
        EXPECT_TRUE(current.camera.position.isApprox(world->GetRenderCamera()->GetPosition(), 1e-5f));
    }
    ASSERT_EQ(world->GetScriptBlueprintStates().size(), declarations.size());
    for (const auto& state : world->GetScriptBlueprintStates()) EXPECT_FALSE(state.at("failed").get<bool>());
    EXPECT_GT(State("body-0")["position"][0].get<float>(), 0);
    EXPECT_LT(State("body-0")["position"][1].get<float>(), 100);
    EXPECT_NEAR(State("mesh-0")["position"][0].get<float>(), 33 * FrameDelta, 1e-5f);
    EXPECT_GE(State("timer-0")["position"][0].get<float>(), 4);
    EXPECT_EQ(State("receiver")["position"][1], 1);
    EXPECT_EQ(State("sender")["position"][1], 1);
    EXPECT_EQ(Performance()["pending"]["timers"], TimerCount);
    EXPECT_EQ(Performance()["pending"]["subscriptions"], 2);
    EXPECT_EQ(Performance()["time"]["frameIndex"], 33);
    const auto after = Snapshot();
    EXPECT_FLOAT_EQ(after.lights[0].intensity, 2500);
    EXPECT_FLOAT_EQ(frozen.lights[0].intensity, 1000);
    EXPECT_TRUE(after.lights[0].position.isApprox(
        ScriptComponentMatrix(State("light"), 1).block<3, 1>(0, 3), 1e-4f));
    EXPECT_GT(after.camera.position.x(), frozen.camera.position.x());
    EXPECT_NEAR(after.camera.position.x(), 33 * FrameDelta * 100, 1e-3f);
    Eigen::Matrix4f section = Eigen::Matrix4f::Identity();
    section(1, 3) = 7;
    const Eigen::Matrix4f expectedBody = ScriptComponentMatrix(State("body-0"), 1) * section;
    EXPECT_TRUE(Draw(after, bodyKey).model.isApprox(expectedBody, 1e-4f));
    EXPECT_TRUE(Draw(after, "__blueprint/mesh-0/visual|section-0").model.isApprox(
        ScriptComponentMatrix(State("mesh-0"), 1), 1e-4f));
    EXPECT_TRUE(Draw(frozen, bodyKey).model.isApprox(frozenBody));
    EXPECT_FALSE(Draw(after, bodyKey).model.isApprox(frozenBody));
    EXPECT_EQ(frozenStates[1]["position"][1], 100);
    EXPECT_EQ(Draw(after, bodyKey).mesh.key, "mixed-mesh");
    EXPECT_EQ(Draw(after, bodyKey).material.key, "mixed-material-1");

    std::sort(elapsed.begin(), elapsed.end());
    testing::Test::RecordProperty("dynamicBodies", BodyCount);
    testing::Test::RecordProperty("movingMeshes", MovingMeshCount);
    testing::Test::RecordProperty("repeatingTimers", TimerCount);
    testing::Test::RecordProperty("snapshotDraws", int(after.meshDraws.size()));
    testing::Test::RecordProperty("measuredFrames", int(elapsed.size()));
    testing::Test::RecordProperty("meanMs", std::to_string(total / elapsed.size()));
    testing::Test::RecordProperty("p95Ms", std::to_string(elapsed[28]));
    testing::Test::RecordProperty("maxMs", std::to_string(elapsed.back()));
    testing::Test::RecordProperty("meanTickMs", std::to_string(tickTotal / elapsed.size()));
    testing::Test::RecordProperty("meanWorldSnapshotMs", std::to_string(snapshotTotal / elapsed.size()));
    std::cout << "Mixed CLR/Jolt/World: " << BodyCount << " dynamic bodies, " << after.meshDraws.size()
        << " CPU draws, " << TimerCount << " timers, event/camera/light; 3 warm + 30 measured, mean="
        << total / elapsed.size() << " ms, p95=" << elapsed[28] << " ms, max=" << elapsed.back()
        << " ms (Tick + WorldSnapshot, no Vulkan)\n";
}

TEST_F(ScriptMixedLoadTest, PauseFreezesMixedStateAndStopRestoresAuthorVisualsAndReleasesServices)
{
    const auto authored = Snapshot();
    const auto camera = world->GetCamera();
    ASSERT_NO_FATAL_FAILURE(Control(ScriptPlayState::Playing));
    Frames(12);
    ASSERT_NO_FATAL_FAILURE(Control(ScriptPlayState::Paused));
    const auto playingStates = world->GetScriptBlueprintStates();
    const auto paused = Snapshot();
    const auto playTime = Performance()["time"]["timeSincePlay"];
    Tick(1);
    EXPECT_EQ(world->GetScriptBlueprintStates(), playingStates);
    EXPECT_EQ(Performance()["time"]["timeSincePlay"], playTime);
    EXPECT_TRUE(Snapshot().camera.view.isApprox(paused.camera.view));
    EXPECT_EQ(Performance()["pending"]["timers"], TimerCount);
    ASSERT_NO_FATAL_FAILURE(Control(ScriptPlayState::Editing));
    EXPECT_FALSE(world->GetScriptContext());
    EXPECT_TRUE(world->GetScriptBlueprintStates().empty());
    EXPECT_EQ(world->GetRenderCamera(), camera);
    EXPECT_EQ(Performance()["pending"]["timers"], 0);
    EXPECT_EQ(Performance()["pending"]["subscriptions"], 0);
    EXPECT_EQ(Performance()["time"]["frameIndex"], 0);
    const auto stopped = Snapshot();
    ASSERT_EQ(stopped.meshDraws.size(), authored.meshDraws.size());
    for (const auto& draw : authored.meshDraws)
        EXPECT_TRUE(Draw(stopped, draw.debugName).model.isApprox(draw.model, 1e-5f)) << draw.debugName;
    EXPECT_FLOAT_EQ(stopped.lights[0].intensity, authored.lights[0].intensity);
    EXPECT_FLOAT_EQ(paused.lights[0].intensity, 2500);
    EXPECT_FALSE(Draw(paused, "__blueprint/body-0/visual|section-0").model.isApprox(
        Draw(stopped, "__blueprint/body-0/visual|section-0").model));
}

TEST_F(ScriptMixedLoadTest, WorldReplacementReleasesMixedContextAndStartsFreshServicesAndPhysics)
{
    ASSERT_NO_FATAL_FAILURE(Control(ScriptPlayState::Playing));
    Frames(12);
    const auto oldContext = world->GetScriptContext();
    const auto frozen = Snapshot();
    const auto bodyKey = "__blueprint/body-0/visual|section-0";
    const Eigen::Matrix4f oldBody = Draw(frozen, bodyKey).model;
    const auto oldCamera = frozen.camera.position;
    auto candidate = PrepareWorld(2);
    runtime.SynchronizeWorld(candidate);
    world = std::move(candidate);
    EXPECT_THROW(oldContext->Call("runtime_snapshot"), std::runtime_error);
    EXPECT_EQ(runtime.View(2).state, ScriptPlayState::Editing);
    EXPECT_TRUE(world->GetScriptBlueprintStates().empty());
    EXPECT_EQ(Performance()["pending"]["timers"], 0);
    EXPECT_EQ(Performance()["pending"]["subscriptions"], 0);
    EXPECT_EQ(Performance()["time"]["frameIndex"], 0);
    EXPECT_EQ(world->GetRenderCamera(), world->GetCamera());
    ASSERT_NO_FATAL_FAILURE(Control(ScriptPlayState::Playing));
    EXPECT_EQ(State("body-0")["position"][1], 100);
    EXPECT_EQ(State("timer-0")["position"][0], 0);
    Frames(12);
    EXPECT_EQ(State("receiver")["position"][1], 1);
    EXPECT_EQ(Performance()["pending"]["timers"], TimerCount);
    EXPECT_EQ(Performance()["pending"]["subscriptions"], 2);
    EXPECT_EQ(Performance()["assembly"]["contexts"], 1);
    const auto current = Snapshot();
    EXPECT_EQ(current.worldGeneration, 2u);
    EXPECT_EQ(frozen.worldGeneration, 1u);
    EXPECT_TRUE(Draw(current, bodyKey).previousModel.isApprox(Draw(current, bodyKey).model));
    EXPECT_TRUE(Draw(frozen, bodyKey).model.isApprox(oldBody));
    EXPECT_TRUE(frozen.camera.position.isApprox(oldCamera));
    // 两个 World 从相同作者状态开始，旧 Jolt 世界与 timer/event 状态不得串入新代。
    EXPECT_TRUE(Draw(current, bodyKey).model.isApprox(oldBody, 1e-4f));
    EXPECT_TRUE(current.camera.position.isApprox(oldCamera, 1e-4f));
}
#endif
} // namespace
