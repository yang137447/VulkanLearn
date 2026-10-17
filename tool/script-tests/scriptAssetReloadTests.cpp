#include <gtest/gtest.h>
#include <chrono>
#include <fstream>
#include "script/scriptBlueprintAsset.h"
#include "script/scriptRuntime.h"
#include "world/world.h"

namespace
{
using namespace VL;
using Json = nlohmann::json;

#ifdef VULKANLEARN_ENABLE_CSHARP_BLUEPRINTS
// 独占临时 ACT 文件；驱动生产观察器与候选准备，不依赖正式内容、时间等待或 Vulkan。
class ScriptAssetReloadTest : public testing::Test
{
protected:
    void SetUp() override
    {
        root = std::filesystem::temp_directory_path() / ("vl-script-reload-" +
            std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
        std::filesystem::create_directories(root / "Common/Blueprints");
        asset = {{"type", "blueprint"}, {"name", "Rotator"}, {"class", "Gameplay.RotatorBlueprint"},
            {"properties", {{"DegreesPerSecond", 45}}}};
        Write();
        blueprint = {{"type", "blueprintInstance"}, {"id", "a"}, {"name", "A"},
            {"blueprintPath", "Common/Blueprints/BP_watch.json"}, {"position", {0, 0, 0}},
            {"rotation", {0, 0, 0}}, {"scale", {1, 1, 1}}, {"properties", Json::object()}};
        scene = {{"objects", Json::array({blueprint})}};
        runtime.Initialize(true, VL_SCRIPT_OUTPUT_DIRECTORY,
            std::string(VL_SCRIPT_OUTPUT_DIRECTORY) + "/ScriptFixtures.dll");
        runtime.SetResourceRoot(root.string());
        world = std::make_shared<World>(1, "scene");
        runtime.PrepareWorld(*world, ResolveScriptBlueprintScene(root, scene, "scene"), scene);
        runtime.SynchronizeWorld(world);
    }
    void TearDown() override
    {
        runtime.Shutdown();
        world.reset();
        std::filesystem::remove_all(root);
    }
    void Write() { std::ofstream(root / "Common/Blueprints/BP_watch.json") << asset.dump(); }
    bool Poll() { return runtime.PollBlueprintAssetChanges(*world, 0.5f); }
    std::filesystem::path root;
    Json asset, blueprint, scene;
    ScriptRuntimeSubsystem runtime;
    std::shared_ptr<World> world;
};

TEST_F(ScriptAssetReloadTest, RequiresTwoStableContentSamplesAndDoesNotRetrySameGeneration)
{
    EXPECT_FALSE(Poll()); EXPECT_FALSE(Poll());
    asset["properties"]["DegreesPerSecond"] = 90; Write();
    EXPECT_FALSE(runtime.PollBlueprintAssetChanges(*world, 0.25f));
    EXPECT_FALSE(runtime.PollBlueprintAssetChanges(*world, 0.25f));
    asset["properties"]["DegreesPerSecond"] = 120; Write();
    EXPECT_FALSE(Poll()); EXPECT_TRUE(Poll());
    EXPECT_FALSE(Poll()); EXPECT_FALSE(Poll());
}

TEST_F(ScriptAssetReloadTest, PlayingAndPausedDeferReloadUntilStop)
{
    std::string error;
    ASSERT_TRUE(runtime.Control(*world, ScriptPlayState::Playing, error)) << error;
    asset["properties"]["DegreesPerSecond"] = 90; Write();
    EXPECT_FALSE(Poll()); EXPECT_FALSE(Poll());
    ASSERT_TRUE(runtime.Control(*world, ScriptPlayState::Paused, error));
    EXPECT_FALSE(Poll());
    ASSERT_TRUE(runtime.Control(*world, ScriptPlayState::Editing, error));
    EXPECT_FALSE(Poll()); EXPECT_TRUE(Poll());
}

TEST_F(ScriptAssetReloadTest, InvalidCandidateKeepsActiveWorldAndFixTriggersNewAttempt)
{
    asset["class"] = "Gameplay.Missing"; Write();
    EXPECT_FALSE(Poll()); EXPECT_TRUE(Poll());
    World candidate(2, "scene");
    EXPECT_THROW(runtime.PrepareWorld(candidate, ResolveScriptBlueprintScene(root, scene, "scene"), scene),
        std::runtime_error);
    EXPECT_EQ(world->GetGeneration(), 1u);
    EXPECT_EQ(world->GetScriptBlueprints()[0].at("properties").at("DegreesPerSecond"), 45);
    EXPECT_FALSE(Poll());
    asset["class"] = "Gameplay.RotatorBlueprint"; asset["properties"]["DegreesPerSecond"] = 90; Write();
    EXPECT_FALSE(Poll()); EXPECT_TRUE(Poll());
    auto replacement = std::make_shared<World>(3, "scene");
    runtime.PrepareWorld(*replacement, ResolveScriptBlueprintScene(root, scene, "scene"), scene);
    runtime.SynchronizeWorld(replacement);
    EXPECT_EQ(replacement->GetScriptBlueprints()[0].at("properties").at("DegreesPerSecond"), 90);
    EXPECT_FALSE(runtime.PollBlueprintAssetChanges(*replacement, 0.5f));
}

TEST_F(ScriptAssetReloadTest, DeletedAndMalformedAssetRetainLastGoodStateWithoutRetryStorm)
{
    std::filesystem::remove(root / "Common/Blueprints/BP_watch.json");
    EXPECT_FALSE(Poll()); EXPECT_TRUE(Poll()); EXPECT_FALSE(Poll());
    EXPECT_THROW(ResolveScriptBlueprintScene(root, scene, "scene"), std::exception);
    std::ofstream(root / "Common/Blueprints/BP_watch.json") << "{";
    EXPECT_FALSE(Poll()); EXPECT_TRUE(Poll()); EXPECT_FALSE(Poll());
    EXPECT_THROW(ResolveScriptBlueprintScene(root, scene, "scene"), std::exception);
    EXPECT_EQ(world->GetScriptBlueprints()[0].at("class"), "Gameplay.RotatorBlueprint");
}

TEST_F(ScriptAssetReloadTest, PreloadAddsHiddenTemplatesWithoutMutatingAuthorScene)
{
    const auto authored = scene;
    scene["spawnAssets"] = Json::array({"Common/Blueprints/BP_watch.json", "Common/Blueprints/BP_watch.json"});
    const auto effective = ResolveScriptBlueprintScene(root, scene, "scene");
    EXPECT_EQ(effective.at("objects").size(), 2u);
    EXPECT_EQ(effective.at("__scriptAssets").size(), 1u);
    EXPECT_EQ(scene.at("objects"), authored.at("objects"));
    auto invalid = scene;
    invalid["objects"][0]["class"] = "Gameplay.RotatorBlueprint";
    EXPECT_THROW(ResolveScriptBlueprintScene(root, invalid, "scene"), std::invalid_argument);
}

TEST_F(ScriptAssetReloadTest, InheritedBodyRejectsNonuniformInstanceScale)
{
    asset["physics"] = {{"shape", "sphere"}, {"radius", 10}}; Write();
    blueprint["scale"] = {1, 2, 1};
    EXPECT_THROW(ResolveScriptBlueprint(root, blueprint), std::invalid_argument);
}
#endif
} // namespace
