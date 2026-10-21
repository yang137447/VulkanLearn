#include <gtest/gtest.h>
#include <chrono>
#include <fstream>
#include "script/scriptRuntime.h"
#include "world/world.h"
#include "sceneNode.h"
#include "input/inputSubsystem.h"
#include "engine/diagnosticsSubsystem.h"

#ifdef VULKANLEARN_ENABLE_CSHARP_BLUEPRINTS
#define WIN32_LEAN_AND_MEAN
#include <windows.h>

namespace
{
using namespace VL;
using Json = nlohmann::json;

class ScriptAssemblyReloadTest : public testing::Test
{
protected:
    void SetUp() override
    {
        root = std::filesystem::temp_directory_path() / ("vl-assembly-reload-" +
            std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
        std::filesystem::create_directories(root);
        CopyVersion(1);
        runtime.Initialize(true, VL_SCRIPT_OUTPUT_DIRECTORY, (root / "ReloadFixtures.dll").string());
        world = std::make_shared<World>(1, "reload-scene");
        world->SetCamera(std::make_shared<Camera>());
        blueprint = {{"type", "blueprintInstance"}, {"id", "reload"}, {"name", "Reload"},
            {"class", "ReloadFixtures.ReloadBlueprint"}, {"properties", Json::object()},
            {"position", {0, 0, 0}}, {"rotation", {0, 0, 0}}, {"scale", {1, 1, 1}},
            {"components", Json::array()}};
        Prepare();
    }
    void TearDown() override
    {
        runtime.Shutdown();
        world.reset();
        std::filesystem::remove_all(root);
    }
    void CopyVersion(int version)
    {
        for (const char* extension : {".dll", ".pdb"})
            std::filesystem::copy_file(std::filesystem::path(VL_SCRIPT_OUTPUT_DIRECTORY) /
                ("reload-v" + std::to_string(version)) / (std::string("ReloadFixtures") + extension),
                root / (std::string("ReloadFixtures") + extension), std::filesystem::copy_options::overwrite_existing);
    }
    void Prepare()
    {
        runtime.PrepareWorld(*world, {{"objects", Json::array({blueprint})}});
        runtime.SynchronizeWorld(world);
    }
    void Play()
    {
        std::string error;
        ASSERT_TRUE(runtime.Control(*world, ScriptPlayState::Playing, error)) << error;
    }
    void Stop()
    {
        std::string error;
        ASSERT_TRUE(runtime.Control(*world, ScriptPlayState::Editing, error)) << error;
    }
    const Json& State() const { return world->GetScriptBlueprintStates().at(0); }
    void PrepareMigration()
    {
        blueprint["class"] = "ReloadFixtures.MigrationBlueprint";
        Prepare();
    }
    Json Marked(const std::string& id = "reload")
    {
        const auto response = world->GetScriptContext()->Call("migration_capture");
        for (const auto& instance : response.at("result").at("instances"))
            if (instance.at("id") == id)
                for (const auto& property : instance.at("properties"))
                    if (property.at("name") == "Counter") return property.at("value");
        return nullptr;
    }
    std::filesystem::path root;
    ScriptRuntimeSubsystem runtime;
    std::shared_ptr<World> world;
    Json blueprint;
};

TEST_F(ScriptAssemblyReloadTest, SamePathReplacementRefreshesDefaultsAndResetsGameplayStatics)
{
    Play();
    EXPECT_EQ(State()["position"], Json::array({10, 1, 0}));
    Stop(); Play();
    EXPECT_EQ(State()["position"][1], 2);
    Stop();
    auto before = world->GetScriptBlueprintSources();
    CopyVersion(2);
    std::string error;
    ASSERT_TRUE(runtime.ReloadAssembly(*world, error)) << error;
    EXPECT_EQ(world->GetScriptBlueprints()[0]["properties"]["Speed"], 20);
    EXPECT_EQ(world->GetScriptBlueprintSources(), before);
    EXPECT_EQ(world->GetGeneration(), 1u);
    Play();
    EXPECT_EQ(State()["position"], Json::array({20, 1, 0}));
    runtime.Tick(world, 0.1f, {}, DiagnosticsSubsystem{});
    EXPECT_EQ(State()["position"][2], 1); // 只有新代的 timer 被投递。
}

TEST_F(ScriptAssemblyReloadTest, ExplicitOverridesSurviveNewDefaultsAndOldContextIsRejected)
{
    blueprint["properties"]["Speed"] = 77; Prepare();
    const auto previousContext = world->GetScriptContext();
    CopyVersion(2);
    std::string error;
    ASSERT_TRUE(runtime.ReloadAssembly(*world, error)) << error;
    EXPECT_THROW(previousContext->Call("start"), std::runtime_error);
    EXPECT_EQ(world->GetScriptBlueprints()[0]["properties"]["Speed"], 77);
    Play();
    EXPECT_EQ(State()["position"][0], 77);
}

TEST_F(ScriptAssemblyReloadTest, LiveAuthorTransformSurvivesAssemblyReload)
{
    ASSERT_TRUE(world->SetScriptBlueprintTransform("reload", "position", 0, 123));
    CopyVersion(2);
    std::string error;
    ASSERT_TRUE(runtime.ReloadAssembly(*world, error)) << error;
    EXPECT_EQ(world->GetScriptBlueprints()[0]["position"][0], 123);
}

TEST_F(ScriptAssemblyReloadTest, ChangedPropertyTypeRejectsCandidateWithoutLosingOverrides)
{
    blueprint["properties"]["SchemaValue"] = 7; Prepare();
    const auto context = world->GetScriptContext();
    CopyVersion(2);
    std::string error;
    EXPECT_FALSE(runtime.ReloadAssembly(*world, error));
    EXPECT_EQ(world->GetScriptContext(), context);
    EXPECT_EQ(world->GetScriptBlueprints()[0]["properties"]["SchemaValue"], 7);
}

TEST_F(ScriptAssemblyReloadTest, RemovedInstancePermissionRejectsCandidate)
{
    blueprint["properties"]["Restricted"] = true; Prepare();
    const auto context = world->GetScriptContext();
    CopyVersion(2);
    std::string error;
    EXPECT_FALSE(runtime.ReloadAssembly(*world, error));
    EXPECT_NE(error.find("Instance Editable"), std::string::npos);
    EXPECT_EQ(world->GetScriptContext(), context);
}

TEST_F(ScriptAssemblyReloadTest, InvalidPreloadedTemplateAlsoRejectsCompleteReload)
{
    auto child = blueprint;
    child["id"] = "template";
    child["__templatePath"] = "Common/Blueprints/BP_child.json";
    child["properties"]["RejectV2"] = true;
    runtime.PrepareWorld(*world, {{"objects", Json::array({blueprint, child})}});
    const auto context = world->GetScriptContext();
    CopyVersion(2);
    std::string error;
    EXPECT_FALSE(runtime.ReloadAssembly(*world, error));
    EXPECT_EQ(world->GetScriptContext(), context);
    EXPECT_EQ(world->GetScriptTemplates()["Common/Blueprints/BP_child.json"]["properties"]["Speed"], 10);
}

TEST_F(ScriptAssemblyReloadTest, ValidPreloadedTemplateUsesNewDefaultsDuringEditingReload)
{
    auto child = blueprint;
    child["id"] = "template";
    child["__templatePath"] = "Common/Blueprints/BP_child.json";
    runtime.PrepareWorld(*world, {{"objects", Json::array({blueprint, child})}});
    CopyVersion(2);
    std::string error;
    ASSERT_TRUE(runtime.ReloadAssembly(*world, error)) << error;
    EXPECT_EQ(world->GetScriptTemplates()["Common/Blueprints/BP_child.json"]["properties"]["Speed"], 20);
}

TEST_F(ScriptAssemblyReloadTest, CandidateCreateFailureKeepsCatalogAndWorldConfiguration)
{
    blueprint["properties"]["RejectV2"] = true; Prepare();
    const auto context = world->GetScriptContext();
    const auto configuration = world->GetScriptBlueprints();
    const auto classes = runtime.Classes();
    CopyVersion(2);
    std::string error;
    EXPECT_FALSE(runtime.ReloadAssembly(*world, error));
    EXPECT_NE(error.find("V2 rejected"), std::string::npos);
    EXPECT_EQ(world->GetScriptContext(), context);
    EXPECT_EQ(world->GetScriptBlueprints(), configuration);
    EXPECT_EQ(runtime.Classes(), classes);
    Play();
    EXPECT_EQ(State()["position"][0], 10);
}

TEST_F(ScriptAssemblyReloadTest, BadImageAndMissingClassPreserveLastGoodAssembly)
{
    std::ofstream(root / "ReloadFixtures.dll", std::ios::binary) << "invalid assembly";
    std::string error;
    EXPECT_FALSE(runtime.ReloadAssembly(*world, error));
    std::filesystem::copy_file(std::filesystem::path(VL_SCRIPT_OUTPUT_DIRECTORY) / "Gameplay.dll",
        root / "ReloadFixtures.dll", std::filesystem::copy_options::overwrite_existing);
    std::filesystem::remove(root / "ReloadFixtures.pdb");
    EXPECT_FALSE(runtime.ReloadAssembly(*world, error));
    Play();
    EXPECT_EQ(State()["position"][0], 10);
}

TEST_F(ScriptAssemblyReloadTest, PlayingAndPausedReloadPreservesWorldAndInvalidatesOldContext)
{
    Play();
    const auto context = world->GetScriptContext();
    CopyVersion(2);
    std::string error;
    auto before = world->GetScriptBlueprintStates();
    ASSERT_TRUE(runtime.ReloadAssembly(*world, error)) << error;
    EXPECT_NE(world->GetScriptContext(), context);
    EXPECT_THROW(context->Call("start"), std::runtime_error);
    // 运行位姿/生命周期保持；作者配置快照在提交时立即反映新代默认值。
    before[0]["properties"] = world->GetScriptBlueprints()[0]["properties"];
    EXPECT_EQ(before[0]["properties"]["Speed"], 20);
    EXPECT_EQ(before[0]["properties"]["SchemaValue"], "new");
    EXPECT_EQ(world->GetScriptBlueprintStates(), before);
    EXPECT_EQ(runtime.View(1).state, ScriptPlayState::Playing);
    ASSERT_TRUE(runtime.Control(*world, ScriptPlayState::Paused, error));
    const auto paused = world->GetScriptContext();
    ASSERT_TRUE(runtime.ReloadAssembly(*world, error)) << error;
    EXPECT_NE(world->GetScriptContext(), paused);
    EXPECT_EQ(runtime.View(1).state, ScriptPlayState::Paused);
    EXPECT_EQ(world->GetGeneration(), 1u);
    Stop();
    EXPECT_TRUE(runtime.ReloadAssembly(*world, error)) << error;
}

TEST_F(ScriptAssemblyReloadTest, LiveMigrationKeepsOnlyMarkedValuesAndRebindsNewServices)
{
    PrepareMigration(); Play();
    runtime.Tick(world, 0.01f, {}, DiagnosticsSubsystem{});
    EXPECT_EQ(Marked(), 50);
    EXPECT_EQ(State()["position"], Json::array({50, 101, 1}));
    const auto time = runtime.ContextTime();
    CopyVersion(2);
    std::string error;
    ASSERT_TRUE(runtime.ReloadAssembly(*world, error)) << error;
    EXPECT_EQ(Marked(), 50);
    EXPECT_EQ(runtime.ContextTime(), time);
    runtime.Tick(world, 0.01f, {}, DiagnosticsSubsystem{});
    EXPECT_EQ(Marked(), 60);
    EXPECT_EQ(State()["position"], Json::array({60, 201, 2}));
    const auto snapshot = world->GetScriptContext()->Call("runtime_snapshot").at("result");
    EXPECT_EQ(snapshot["pending"]["subscriptions"], 1);
    bool reloaded = false;
    for (const auto& callback : snapshot["blueprints"][0]["callbacks"])
        if (callback["phase"] == "OnReload") reloaded = callback["calls"] == 1;
    EXPECT_TRUE(reloaded);
}

TEST_F(ScriptAssemblyReloadTest, ComponentMigrationDoesNotCarryUnmarkedExposedRuntimeValues)
{
    PrepareMigration();
    blueprint["__scriptComponentAsset"] = true;
    blueprint["scriptComponents"] = Json::array({{{"id", "counter"},
        {"class", "ReloadFixtures.MigrationComponent"}, {"properties", Json::object()}}});
    Prepare(); Play();
    runtime.Tick(world, 0.01f, {}, DiagnosticsSubsystem{});
    CopyVersion(2);
    std::string error;
    ASSERT_TRUE(runtime.ReloadAssembly(*world, error)) << error;
    runtime.Tick(world, 0.01f, {}, DiagnosticsSubsystem{});
    const auto& component = State()["scriptComponents"][0];
    EXPECT_EQ(component["properties"]["Counter"], 62);
    EXPECT_EQ(component["properties"]["Unmarked"], 201);
}

TEST_F(ScriptAssemblyReloadTest, PausedReloadImmediatelyPublishesDefaultsForComponentsWithoutFrameCallbacks)
{
    blueprint["class"] = "ReloadFixtures.PassiveMigrationBlueprint";
    blueprint["__scriptComponentAsset"] = true;
    blueprint["scriptComponents"] = Json::array({{{"id", "passive"},
        {"class", "ReloadFixtures.PassiveMigrationComponent"}, {"properties", Json::object()}}});
    Prepare(); Play();
    const auto previous = State();
    ASSERT_EQ(previous["scriptComponents"][0]["properties"]["DefaultValue"], 100);
    std::string error;
    ASSERT_TRUE(runtime.Control(*world, ScriptPlayState::Paused, error)) << error;
    CopyVersion(2);
    ASSERT_TRUE(runtime.ReloadAssembly(*world, error)) << error;
    EXPECT_EQ(State()["scriptComponents"][0]["properties"]["DefaultValue"], 200);
    EXPECT_EQ(previous["scriptComponents"][0]["properties"]["DefaultValue"], 100);
    ASSERT_TRUE(runtime.Control(*world, ScriptPlayState::Playing, error)) << error;
    runtime.Tick(world, 1.0f / 60, {}, DiagnosticsSubsystem{});
    EXPECT_EQ(State()["scriptComponents"][0]["properties"]["DefaultValue"], 200);
}

TEST_F(ScriptAssemblyReloadTest, HookFailureKeepsOldContextClockAndCallbacks)
{
    blueprint["class"] = "ReloadFixtures.MigrationBlueprint";
    blueprint["properties"]["RejectReload"] = true;
    Prepare(); Play();
    const auto before = world->GetScriptBlueprintStates(), clock = runtime.ContextTime();
    const auto context = world->GetScriptContext();
    CopyVersion(2);
    std::string error;
    EXPECT_FALSE(runtime.ReloadAssembly(*world, error));
    EXPECT_NE(error.find("migration-hook-rejected"), std::string::npos);
    EXPECT_EQ(world->GetScriptContext(), context);
    EXPECT_EQ(world->GetScriptBlueprintStates(), before);
    EXPECT_EQ(runtime.ContextTime(), clock);
    runtime.Tick(world, 0.01f, {}, DiagnosticsSubsystem{});
    EXPECT_EQ(State()["position"], Json::array({50, 101, 1}));
}

TEST_F(ScriptAssemblyReloadTest, HookWorldAndStructureMutationsAreRejectedBeforeSwap)
{
    for (const char* property : {"MoveOnReload", "ChangeStructure"})
    {
        blueprint["class"] = "ReloadFixtures.MigrationBlueprint";
        blueprint["properties"] = {{property, true}};
        Prepare(); Play();
        const auto context = world->GetScriptContext();
        const auto before = world->GetScriptBlueprintStates();
        CopyVersion(2);
        std::string error;
        EXPECT_FALSE(runtime.ReloadAssembly(*world, error));
        EXPECT_NE(error.find("cannot change"), std::string::npos) << error;
        EXPECT_EQ(world->GetScriptContext(), context);
        EXPECT_EQ(world->GetScriptBlueprintStates(), before);
        Stop();
    }
}

TEST_F(ScriptAssemblyReloadTest, ChangedMarkedPropertyTypeRejectsWithoutTouchingLiveContext)
{
    blueprint["class"] = "ReloadFixtures.ChangedMigrationBlueprint";
    Prepare(); Play();
    const auto context = world->GetScriptContext();
    CopyVersion(2);
    std::string error;
    EXPECT_FALSE(runtime.ReloadAssembly(*world, error));
    EXPECT_NE(error.find("changed type"), std::string::npos);
    EXPECT_EQ(world->GetScriptContext(), context);
}

TEST_F(ScriptAssemblyReloadTest, LiveReloadPreservesSpawnedJoltBodyAndFrameClock)
{
    PrepareMigration();
    blueprint["properties"]["SpawnChild"] = true;
    auto child = blueprint;
    child["properties"] = Json::object();
    child["id"] = "child";
    child["__templatePath"] = "Common/Blueprints/BP_migration.json";
    child["physics"] = {{"motion", "dynamic"}, {"shape", "sphere"}, {"radius", 20}};
    runtime.PrepareWorld(*world, {{"objects", Json::array({blueprint, child})}});
    Play();
    runtime.Tick(world, 0.025f, {}, DiagnosticsSubsystem{});
    ASSERT_EQ(world->GetScriptBlueprintStates().size(), 2u);
    const auto before = world->GetScriptBlueprintStates();
    const auto clock = runtime.ContextTime();
    CopyVersion(2);
    std::string error;
    ASSERT_TRUE(runtime.ReloadAssembly(*world, error)) << error;
    EXPECT_EQ(world->GetScriptBlueprintStates(), before);
    EXPECT_EQ(runtime.ContextTime(), clock);
    runtime.Tick(world, 0.01f, {}, DiagnosticsSubsystem{});
    EXPECT_LT(world->GetScriptBlueprintStates()[1]["position"][1].get<float>(),
        before[1]["position"][1].get<float>());
    EXPECT_LT(world->GetScriptBlueprintStates()[1]["bodyVelocity"][1].get<float>(),
        before[1]["bodyVelocity"][1].get<float>());
    EXPECT_EQ(runtime.ContextTime()["frameIndex"], 2);
}

TEST_F(ScriptAssemblyReloadTest, LiveReloadPreservesCollisionAndTriggerEdgeBaselines)
{
    blueprint["class"] = "ReloadFixtures.ContactMigrationBlueprint";
    auto other = blueprint;
    other["id"] = "other";
    runtime.PrepareWorld(*world, {{"objects", Json::array({blueprint, other})}});
    Play();
    const auto pairs = Json::array({Json::array({"reload", "other"})});
    world->GetScriptContext()->Call("late", {{"states", world->GetScriptBlueprintStates()},
        {"contacts", pairs}, {"triggers", pairs}});
    ASSERT_EQ(Marked(), 11);
    CopyVersion(2);
    std::string error;
    ASSERT_TRUE(runtime.ReloadAssembly(*world, error)) << error;
    world->GetScriptContext()->Call("late", {{"states", world->GetScriptBlueprintStates()},
        {"contacts", pairs}, {"triggers", pairs}});
    EXPECT_EQ(Marked(), 11);
    world->GetScriptContext()->Call("late", {{"states", world->GetScriptBlueprintStates()},
        {"contacts", Json::array()}, {"triggers", Json::array()}});
    EXPECT_EQ(Marked(), 1111);
    world->GetScriptContext()->Call("late", {{"states", world->GetScriptBlueprintStates()},
        {"contacts", pairs}, {"triggers", pairs}});
    EXPECT_EQ(Marked(), 1122);
}

TEST_F(ScriptAssemblyReloadTest, PendingOldContextStructureCommandsRejectLiveReload)
{
    blueprint["class"] = "ReloadFixtures.PendingMigrationBlueprint";
    auto child = blueprint;
    child["id"] = "child";
    child["__templatePath"] = "Common/Blueprints/BP_pending_migration.json";
    runtime.PrepareWorld(*world, {{"objects", Json::array({blueprint, child})}});
    Play();
    const auto context = world->GetScriptContext();
    const auto before = world->GetScriptBlueprintStates();
    const auto snapshot = context->Call("runtime_snapshot").at("result");
    ASSERT_GT(snapshot["pending"]["commands"].get<int>(), 0);
    CopyVersion(2);
    std::string error;
    EXPECT_FALSE(runtime.ReloadAssembly(*world, error));
    EXPECT_NE(error.find("pending World structure commands and events"), std::string::npos) << error;
    EXPECT_EQ(world->GetScriptContext(), context);
    EXPECT_EQ(world->GetScriptBlueprintStates(), before);
    EXPECT_EQ(context->Call("runtime_snapshot").at("result").at("pending"), snapshot.at("pending"));
}

TEST_F(ScriptAssemblyReloadTest, PendingOldContextEventsRejectLiveReload)
{
    blueprint["class"] = "ReloadFixtures.PendingMigrationBlueprint";
    blueprint["properties"]["PublishEvents"] = true;
    Prepare(); Play();
    runtime.Tick(world, 0.01f, {}, DiagnosticsSubsystem{});
    const auto context = world->GetScriptContext();
    const auto snapshot = context->Call("runtime_snapshot").at("result");
    ASSERT_EQ(snapshot["pending"]["events"], 1);
    CopyVersion(2);
    std::string error;
    EXPECT_FALSE(runtime.ReloadAssembly(*world, error));
    EXPECT_NE(error.find("pending World structure commands and events"), std::string::npos) << error;
    EXPECT_EQ(world->GetScriptContext(), context);
    EXPECT_EQ(context->Call("runtime_snapshot").at("result").at("pending"), snapshot.at("pending"));
}

TEST_F(ScriptAssemblyReloadTest, PendingOldContextRequestsRejectLiveReload)
{
    PrepareMigration(); Play();
    const auto context = world->GetScriptContext();
    context->assetLoads["1"] = {{"path", "Common/Blueprints/BP_pending.json"}};
    CopyVersion(2);
    std::string error;
    EXPECT_FALSE(runtime.ReloadAssembly(*world, error));
    EXPECT_NE(error.find("asset/physics requests"), std::string::npos);
    EXPECT_EQ(world->GetScriptContext(), context);
}

TEST_F(ScriptAssemblyReloadTest, LiveReloadReleasesOldDelegateGraphsForCollection)
{
    PrepareMigration(); Play();
    ScriptHost inspector;
    inspector.Initialize(VL_SCRIPT_OUTPUT_DIRECTORY);
    for (int iteration = 0; iteration < 3; ++iteration)
    {
        CopyVersion(iteration % 2 + 1);
        std::string error;
        ASSERT_TRUE(runtime.ReloadAssembly(*world, error)) << error;
        const auto status = inspector.Invoke({{"op", "assembly_status"}, {"collect", true}}).at("result");
        EXPECT_EQ(status["retiredAlive"], 0) << status;
        EXPECT_EQ(status["contexts"], 1);
    }
}

TEST_F(ScriptAssemblyReloadTest, ReleasedCallbacksAndCatalogsAllowOldAssemblyCollection)
{
    ScriptHost inspector;
    inspector.Initialize(VL_SCRIPT_OUTPUT_DIRECTORY);
    for (int iteration = 0; iteration < 4; ++iteration)
    {
        Play(); Stop();
        CopyVersion(iteration % 2 + 1);
        std::string error;
        ASSERT_TRUE(runtime.ReloadAssembly(*world, error)) << error;
        const auto status = inspector.Invoke({{"op", "assembly_status"}, {"collect", true}}).at("result");
        EXPECT_EQ(status["retiredAlive"], 0) << status;
        EXPECT_EQ(status["loaded"], 1);
        EXPECT_EQ(status["contexts"], 1);
    }
    runtime.Shutdown();
    const auto status = inspector.Invoke({{"op", "assembly_status"}, {"collect", true}}).at("result");
    EXPECT_EQ(status["retiredAlive"], 0) << status;
    EXPECT_EQ(status["loaded"], 0);
    EXPECT_EQ(status["contexts"], 0);
}

TEST(ScriptDeployment, ExecutableRelativePrivateRuntimeHostsGameplay)
{
    const auto directory = ScriptHost::DefaultRuntimeDirectory();
    if (!std::filesystem::is_directory(directory / "dotnet"))
        GTEST_SKIP() << "Run the relocated output check after building vl_script_package";
    ScriptRuntimeSubsystem runtime;
    runtime.Initialize(true, directory.string(), (directory / "Gameplay.dll").string());
    EXPECT_FALSE(runtime.Classes().empty());
    std::vector<wchar_t> path(32768);
    const auto size = GetModuleFileNameW(GetModuleHandleW(L"coreclr.dll"), path.data(), static_cast<DWORD>(path.size()));
    ASSERT_GT(size, 0u);
    // 检查实际加载的 CoreCLR 来源，不能把开发机 SDK 的成功误记为私有运行时部署成功。
    const auto loaded = std::filesystem::weakly_canonical(std::filesystem::path(std::wstring(path.data(), size)));
    const auto expected = std::filesystem::weakly_canonical(directory / "dotnet");
    const auto relative = loaded.lexically_relative(expected);
    EXPECT_FALSE(relative.empty());
    EXPECT_NE(*relative.begin(), "..");
}
}
#endif
