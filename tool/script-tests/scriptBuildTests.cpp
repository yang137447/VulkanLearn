#include <gtest/gtest.h>
#include <chrono>
#include <fstream>
#include <iterator>
#include "script/scriptBuildService.h"
#include "script/scriptSourceFiles.h"
#include "script/scriptBuildDiagnostics.h"
#include "script/scriptRuntime.h"
#include "editor/service/sceneDocumentService.h"
#include "world/world.h"

namespace
{
class ScriptBuildTest : public ::testing::Test
{
protected:
    void SetUp() override
    {
        // 和启动目录同盘，覆盖 Windows 相对资源根；跨盘 relative() 没有合法结果。
        root = std::filesystem::current_path() /
            ("vl-build-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
        std::filesystem::create_directories(root);
    }
    void TearDown() override { std::filesystem::remove_all(root); }
    void Write(const std::filesystem::path& relative, const std::string& bytes)
    {
        const auto path = root / relative;
        std::filesystem::create_directories(path.parent_path());
        std::ofstream(path) << bytes;
    }
    std::filesystem::path root;
};
}

TEST_F(ScriptBuildTest, SourceIdentityUsesBytesAndExcludesBuildArtifacts)
{
    Write("Gameplay/Main.blueprint.cs", "public class Test {}");
    Write("Gameplay/Gameplay.csproj", "<Project/>");
    const auto original = VL::ScriptBuildService::SourceDigest(root);
    Write("Gameplay/obj/Debug/generated.cs", "generated one");
    Write("Gameplay/bin/Assembly.cs", "generated two");
    EXPECT_EQ(original, VL::ScriptBuildService::SourceDigest(root));
    Write("Gameplay/Main.blueprint.cs", "public class Changed {}");
    EXPECT_NE(original, VL::ScriptBuildService::SourceDigest(root));
}

TEST_F(ScriptBuildTest, ResourceSourceCollectionIncludesBothDomainsAndOrdinaryUserData)
{
    Write("Common/Blueprints/Shared.blueprint.cs", "class Shared {}");
    Write("Maps/SC_test/Blueprints/Enemies/BP_enemy.blueprint.cs", "class Enemy {}");
    Write("Maps/SC_test/Data/WeaponStats.cs", "struct WeaponStats {}");
    Write("Generated/Runtime/Generated.cs", "class Generated {}");
    Write("Common/Blueprints/obj/Generated.cs", "class Intermediate {}");
    Write("Maps/SC_test/bin/Generated.cs", "class Binary {}");
    Write("Maps/SC_test/OBJ/Generated.cs", "class UpperIntermediate {}");
    Write("Maps/SC_test/Generated/Generated.cs", "class NestedGenerated {}");
    Write("Source/Outside.cs", "class Outside {}");
    Write("Common/Blueprints/BP_enemy.json", "{}");
    const auto files = VL::CollectScriptSourceFiles(root);
    ASSERT_EQ(files.size(), 3u);
    EXPECT_EQ(files[0], root / "Common/Blueprints/Shared.blueprint.cs");
    EXPECT_EQ(files[1], root / "Maps/SC_test/Blueprints/Enemies/BP_enemy.blueprint.cs");
    EXPECT_EQ(files[2], root / "Maps/SC_test/Data/WeaponStats.cs");
}

TEST_F(ScriptBuildTest, ResourceSourceCollectionAcceptsWindowsUppercaseExtensions)
{
    Write("Common/Blueprints/Upper.CS", "class Upper {}");
    const auto files = VL::CollectScriptSourceFiles(root);
    ASSERT_EQ(files.size(), 1u);
    EXPECT_EQ(files[0], root / "Common/Blueprints/Upper.CS");
}

TEST_F(ScriptBuildTest, EmptyResourceProjectHasNoImplicitEngineGameplaySources)
{
    EXPECT_TRUE(VL::CollectScriptSourceFiles(root).empty());
}

TEST_F(ScriptBuildTest, ResourceSourceManifestEscapesXmlAndMsBuildExpressions)
{
    Write("Common/Blueprints/A&$(Data);'Test.cs", "class Test {}");
    const auto text = VL::ScriptSourceItems(root);
    EXPECT_NE(text.find("A&amp;%24%28Data%29%3B%27Test.cs"), std::string::npos) << text;
}

TEST(ScriptBuildArguments, QuotesWindowsTrailingBackslashesAndEmbeddedQuotes)
{
    EXPECT_EQ(L"\"a b\"", VL::ScriptBuildService::QuoteArgument(L"a b"));
    EXPECT_EQ(L"\"a\\\\\"", VL::ScriptBuildService::QuoteArgument(L"a\\"));
    EXPECT_EQ(L"\"a\\\"b\"", VL::ScriptBuildService::QuoteArgument(L"a\"b"));
    EXPECT_EQ(L"\"\"", VL::ScriptBuildService::QuoteArgument(L""));
}

TEST(ScriptBuildArguments, CompilerLogKeepsWindowsPathsLocationsSeveritiesAndPlainLines)
{
    const auto lines = VL::ParseScriptBuildDiagnostics(
        "D:\\Game (copy)\\Test.cs(12,34): error CS1002: ; expected [Game.csproj]\r\n"
        "D:\\Game\\Warning.cs(7,2): warning CS0168: unused\n"
        "\nBuild FAILED.\n  0 warnings\n");
    ASSERT_EQ(lines.size(), 4u);
    EXPECT_EQ(lines[0].location.file, "D:\\Game (copy)\\Test.cs");
    EXPECT_EQ(lines[0].location.line, 12u);
    EXPECT_EQ(lines[0].location.column, 34u);
    EXPECT_EQ(lines[0].severity, VL::LogSeverity::Error);
    EXPECT_EQ(lines[1].severity, VL::LogSeverity::Warning);
    EXPECT_EQ(lines[2].severity, VL::LogSeverity::Error);
    EXPECT_EQ(lines[3].severity, VL::LogSeverity::Info);
}

TEST_F(ScriptBuildTest, MissingSdkRejectsWithoutStartingWorker)
{
    VL::ScriptBuildService service;
    service.Configure(root / "missing.exe", root / "missing.csproj", root / "output", root / "bridge.dll", "Debug");
    std::string error;
    EXPECT_FALSE(service.Available());
    EXPECT_FALSE(service.Request(7, error));
    EXPECT_FALSE(service.Busy());
    EXPECT_FALSE(error.empty());
    EXPECT_FALSE(service.Poll());
}

#ifdef VULKANLEARN_ENABLE_CSHARP_BLUEPRINTS
namespace
{
std::optional<VL::ScriptBuildResult> FinishBuild(VL::ScriptBuildService& service)
{
    std::optional<VL::ScriptBuildResult> result;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(60);
    while ((!result || service.Busy()) && std::chrono::steady_clock::now() < deadline)
    {
        if (!result) result = service.Poll();
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    return result;
}
}

TEST_F(ScriptBuildTest, ResourceBlueprintAndCustomDataCompileAndLoadWithoutEngineSourceChanges)
{
    Write("Content/Common/Data/Settings.cs",
        "namespace MyGame; public class Settings { public int Health {get;set;} = 125; }");
    Write("Content/Maps/SC_test/Blueprints/BP_enemy.blueprint.cs",
        "using VulkanLearn; namespace MyGame; public sealed class Enemy : Blueprint {"
        "[Expose] public Settings Stats {get;set;} = new(); }");
    Write("Content/Generated/Runtime/Bad.cs", "invalid generated syntax");
    Write("Content/Common/obj/Bad.cs", "invalid intermediate syntax");
    VL::ScriptBuildService service;
    service.Configure(VL_SCRIPT_DOTNET_EXECUTABLE, VL_SCRIPT_GAMEPLAY_PROJECT, root / "output",
        std::filesystem::path(VL_SCRIPT_OUTPUT_DIRECTORY) / "VulkanLearn.Runtime.dll",
        VL_SCRIPT_BUILD_CONFIGURATION, root / "Content", VL_SCRIPT_RUNTIME_SOURCE_DIRECTORY);
    std::string error;
    ASSERT_TRUE(service.Request(31, error)) << error;
    const auto result = FinishBuild(service);
    ASSERT_TRUE(result);
    ASSERT_TRUE(result->succeeded) << result->log;
    VL::ScriptRuntimeSubsystem runtime;
    runtime.Initialize(true, VL_SCRIPT_OUTPUT_DIRECTORY, result->assembly.string());
    ASSERT_EQ(runtime.Classes().size(), 1u);
    EXPECT_EQ(runtime.Classes()[0].at("name"), "MyGame.Enemy");
    EXPECT_EQ(runtime.Classes()[0].at("properties")[0].at("value").at("Health"), 125);
}

TEST_F(ScriptBuildTest, NewlyCreatedResourceBlueprintTemplateCompilesIntoTheGameplayCatalog)
{
    const auto content = root / "Content project (copy)";
    VL::Editor::SceneDocumentService documents(content);
    std::string error;
    ASSERT_TRUE(documents.CreateScriptBlueprintSource("Maps/SC_test/Blueprints/BP_door.json",
        "MyGame.DoorBlueprint", "Door", nlohmann::json::array(), error)) << error;
    VL::ScriptBuildService service;
    service.Configure(VL_SCRIPT_DOTNET_EXECUTABLE, VL_SCRIPT_GAMEPLAY_PROJECT, root / "output",
        std::filesystem::path(VL_SCRIPT_OUTPUT_DIRECTORY) / "VulkanLearn.Runtime.dll",
        VL_SCRIPT_BUILD_CONFIGURATION, content, VL_SCRIPT_RUNTIME_SOURCE_DIRECTORY);
    ASSERT_TRUE(service.Request(33, error)) << error;
    const auto result = FinishBuild(service);
    ASSERT_TRUE(result);
    ASSERT_TRUE(result->succeeded) << result->log;
    VL::ScriptRuntimeSubsystem runtime;
    runtime.Initialize(true, VL_SCRIPT_OUTPUT_DIRECTORY, result->assembly.string());
    ASSERT_EQ(runtime.Classes().size(), 1u);
    EXPECT_EQ(runtime.Classes()[0].at("name"), "MyGame.DoorBlueprint");
}

TEST_F(ScriptBuildTest, ResourceProjectWithoutUserSourcesCompilesAnEmptyGameplayCatalog)
{
    VL::ScriptBuildService service;
    service.Configure(VL_SCRIPT_DOTNET_EXECUTABLE, VL_SCRIPT_GAMEPLAY_PROJECT, root / "output",
        std::filesystem::path(VL_SCRIPT_OUTPUT_DIRECTORY) / "VulkanLearn.Runtime.dll",
        VL_SCRIPT_BUILD_CONFIGURATION, root / "EmptyContent", VL_SCRIPT_RUNTIME_SOURCE_DIRECTORY);
    std::string error;
    ASSERT_TRUE(service.Request(34, error)) << error;
    const auto result = FinishBuild(service);
    ASSERT_TRUE(result);
    ASSERT_TRUE(result->succeeded) << result->log;
    VL::ScriptRuntimeSubsystem runtime;
    runtime.Initialize(true, VL_SCRIPT_OUTPUT_DIRECTORY, result->assembly.string());
    EXPECT_TRUE(runtime.Classes().empty());
}

TEST_F(ScriptBuildTest, ResourceWatchSeesAddRenameDeleteAndIgnoresJsonAndGeneratedChanges)
{
    Write("Content/Common/Blueprints/First.blueprint.cs", "class First {}");
    VL::ScriptBuildService service;
    service.Configure(VL_SCRIPT_DOTNET_EXECUTABLE, VL_SCRIPT_GAMEPLAY_PROJECT, root / "output",
        std::filesystem::path(VL_SCRIPT_OUTPUT_DIRECTORY) / "VulkanLearn.Runtime.dll",
        VL_SCRIPT_BUILD_CONFIGURATION, root / "Content", VL_SCRIPT_RUNTIME_SOURCE_DIRECTORY);
    Write("Content/Maps/SC_test/Blueprints/New.blueprint.cs", "class New {}");
    EXPECT_FALSE(service.ObserveChanges(1));
    EXPECT_TRUE(service.ObserveChanges(1));
    std::filesystem::rename(root / "Content/Maps/SC_test/Blueprints/New.blueprint.cs",
        root / "Content/Maps/SC_test/Blueprints/Renamed.blueprint.cs");
    EXPECT_FALSE(service.ObserveChanges(1));
    EXPECT_TRUE(service.ObserveChanges(1));
    std::filesystem::remove(root / "Content/Maps/SC_test/Blueprints/Renamed.blueprint.cs");
    EXPECT_FALSE(service.ObserveChanges(1));
    EXPECT_TRUE(service.ObserveChanges(1));
    Write("Content/Generated/Runtime/Generated.cs", "generated");
    Write("Content/Common/Blueprints/BP_first.json", "{\"properties\":{}}");
    EXPECT_FALSE(service.ObserveChanges(1));
    EXPECT_FALSE(service.ObserveChanges(1));
}

TEST_F(ScriptBuildTest, ResourceChangesDuringCompilationRejectPublication)
{
    Write("Content/Common/Blueprints/Test.blueprint.cs",
        "using VulkanLearn; namespace MyGame; public class First : Blueprint {}");
    VL::ScriptBuildService service;
    service.Configure(VL_SCRIPT_DOTNET_EXECUTABLE, VL_SCRIPT_GAMEPLAY_PROJECT, root / "output",
        std::filesystem::path(VL_SCRIPT_OUTPUT_DIRECTORY) / "VulkanLearn.Runtime.dll",
        VL_SCRIPT_BUILD_CONFIGURATION, root / "Content", VL_SCRIPT_RUNTIME_SOURCE_DIRECTORY);
    std::string error;
    ASSERT_TRUE(service.Request(32, error)) << error;
    Write("Content/Common/Blueprints/Test.blueprint.cs",
        "using VulkanLearn; namespace MyGame; public class Second : Blueprint {}");
    const auto result = FinishBuild(service);
    ASSERT_TRUE(result);
    EXPECT_FALSE(result->succeeded);
    EXPECT_NE(result->log.find("Sources changed during compilation"), std::string::npos) << result->log;
}

TEST_F(ScriptBuildTest, IdenticalGameplayBuildsProduceByteIdenticalDllAndPdbInSeparateCandidates)
{
    VL::ScriptBuildService service;
    // 配置允许正斜杠，但 Roslyn 的 PathMap 来源必须规范为 Windows 本机路径。
    const auto output = std::filesystem::path((root / "output").generic_string());
    service.Configure(VL_SCRIPT_DOTNET_EXECUTABLE, VL_SCRIPT_GAMEPLAY_PROJECT, output,
        std::filesystem::path(VL_SCRIPT_OUTPUT_DIRECTORY) / "VulkanLearn.Runtime.dll",
        VL_SCRIPT_BUILD_CONFIGURATION, VL_SCRIPT_RESOURCE_ROOT, VL_SCRIPT_RUNTIME_SOURCE_DIRECTORY);
    std::vector<VL::ScriptBuildResult> builds;
    std::string error;
    for (int iteration = 0; iteration < 2; ++iteration)
    {
        ASSERT_TRUE(service.Request(23, error)) << error;
        std::optional<VL::ScriptBuildResult> result;
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(60);
        while ((!result || service.Busy()) && std::chrono::steady_clock::now() < deadline)
        {
            if (!result) result = service.Poll();
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
        }
        ASSERT_TRUE(result);
        ASSERT_FALSE(service.Busy());
        ASSERT_TRUE(result->succeeded) << result->log;
        builds.push_back(std::move(*result));
    }
    ASSERT_NE(builds[0].assembly.parent_path(), builds[1].assembly.parent_path());
    ASSERT_EQ(builds[0].sourceDigest, builds[1].sourceDigest);
    for (const auto* filename : {"Gameplay.dll", "Gameplay.pdb", "Gameplay.deps.json",
        "VulkanLearn.Runtime.dll", "VulkanLearn.Runtime.pdb"})
    {
        const auto firstPath = builds[0].assembly.parent_path() / filename;
        const auto secondPath = builds[1].assembly.parent_path() / filename;
        std::ifstream first(firstPath, std::ios::binary), second(secondPath, std::ios::binary);
        ASSERT_TRUE(first.is_open()) << firstPath;
        ASSERT_TRUE(second.is_open()) << secondPath;
        const std::string firstBytes((std::istreambuf_iterator<char>(first)), std::istreambuf_iterator<char>());
        const std::string secondBytes((std::istreambuf_iterator<char>(second)), std::istreambuf_iterator<char>());
        ASSERT_FALSE(firstBytes.empty()) << firstPath;
        ASSERT_EQ(firstBytes.size(), secondBytes.size()) << filename;
        // 对比完整产物，不能用 MVID、文件存在性或剔除调试段后的部分比较代替可重复构建。
        EXPECT_TRUE(firstBytes == secondBytes) << "Different " << filename << " bytes in "
            << firstPath.parent_path() << " and " << secondPath.parent_path();
    }
}

TEST_F(ScriptBuildTest, SuccessfulGameplayBuildPublishesIsolatedCandidate)
{
    VL::ScriptBuildService service;
    const auto bridge = std::filesystem::path(VL_SCRIPT_OUTPUT_DIRECTORY) / "VulkanLearn.Runtime.dll";
    // 资源根允许相对启动目录；编译器的工作目录不同，候选路径不能随子进程重新解释。
    const auto output = std::filesystem::relative(root / "output", std::filesystem::current_path());
    service.Configure(VL_SCRIPT_DOTNET_EXECUTABLE, VL_SCRIPT_GAMEPLAY_PROJECT,
        output, bridge, VL_SCRIPT_BUILD_CONFIGURATION, VL_SCRIPT_RESOURCE_ROOT, VL_SCRIPT_RUNTIME_SOURCE_DIRECTORY);
    std::string error;
    ASSERT_TRUE(service.Request(23, error)) << error;
    std::optional<VL::ScriptBuildResult> result;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(60);
    while (!result && std::chrono::steady_clock::now() < deadline)
    {
        result = service.Poll();
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    ASSERT_TRUE(result);
    ASSERT_TRUE(result->succeeded) << result->log;
    EXPECT_EQ(23u, result->worldGeneration);
    EXPECT_TRUE(result->assembly.is_absolute());
    EXPECT_EQ((root / "output").lexically_normal(), result->assembly.parent_path().parent_path());
    EXPECT_TRUE(std::filesystem::is_regular_file(result->assembly));
    EXPECT_NE(bridge.parent_path(), result->assembly.parent_path());
    VL::ScriptRuntimeSubsystem runtime;
    runtime.Initialize(true, VL_SCRIPT_OUTPUT_DIRECTORY,
        (std::filesystem::path(VL_SCRIPT_OUTPUT_DIRECTORY) / "Gameplay.dll").string());
    bool hasRotatorComponent = false;
    for (const auto& type : runtime.ComponentClasses())
        if (type.at("name") == "Gameplay.RotatorComponent") hasRotatorComponent = true;
    EXPECT_TRUE(hasRotatorComponent);
    bool hasWeaponConfiguration = false;
    for (const auto& type : runtime.ComponentClasses())
        if (type.at("name") == "Gameplay.WeaponConfigurationComponent")
        {
            hasWeaponConfiguration = true;
            for (const auto& property : type.at("properties"))
            {
                if (property.at("name") == "Stats") EXPECT_EQ(property.at("type"), "object");
                if (property.at("name") == "BurstDelays") EXPECT_EQ(property.at("type"), "array");
                if (property.at("name") == "StartingAmmo") EXPECT_EQ(property.at("type"), "dictionary");
            }
        }
    EXPECT_TRUE(hasWeaponConfiguration);
    VL::World world(23, "Maps/SC_compile/SC_compile.json");
    runtime.PrepareWorld(world, {{"objects", nlohmann::json::array({
        {{"type", "blueprintInstance"}, {"id", "compiled"}, {"name", "Compiled"},
         {"class", "Gameplay.RotatorBlueprint"}, {"position", {0, 0, 0}},
         {"rotation", {0, 0, 0}}, {"scale", {1, 1, 1}}, {"properties", nlohmann::json::object()}}
    })}});
    EXPECT_TRUE(runtime.ReloadAssembly(world, error, result->assembly.string())) << error;
    EXPECT_EQ(23u, world.GetGeneration());
}

TEST_F(ScriptBuildTest, WatchRequiresTwoStableSamplesAndIgnoresUnchangedBytes)
{
    Write("Gameplay/Gameplay.csproj", "<Project/>");
    Write("Gameplay/Test.cs", "class First {}");
    VL::ScriptBuildService service;
    service.Configure(VL_SCRIPT_DOTNET_EXECUTABLE, root / "Gameplay/Gameplay.csproj",
        root / "output", std::filesystem::path(VL_SCRIPT_OUTPUT_DIRECTORY) / "VulkanLearn.Runtime.dll", "Debug");
    EXPECT_FALSE(service.ObserveChanges(1));
    Write("Gameplay/Test.cs", "class Changed {}");
    EXPECT_FALSE(service.ObserveChanges(1));
    EXPECT_TRUE(service.ObserveChanges(1));
    EXPECT_FALSE(service.ObserveChanges(1));
    Write("Gameplay/obj/Generated.cs", "generated");
    EXPECT_FALSE(service.ObserveChanges(1));
}

TEST_F(ScriptBuildTest, BridgeSourceChangesRequireRestartBeforeLaunchingBuild)
{
    Write("Gameplay/Gameplay.csproj", "<Project/>");
    Write("VulkanLearn.Runtime/Api.cs", "class Api {}");
    VL::ScriptBuildService service;
    service.Configure(VL_SCRIPT_DOTNET_EXECUTABLE, root / "Gameplay/Gameplay.csproj",
        root / "output", std::filesystem::path(VL_SCRIPT_OUTPUT_DIRECTORY) / "VulkanLearn.Runtime.dll", "Debug");
    Write("VulkanLearn.Runtime/Api.cs", "class ChangedApi {}");
    std::string error;
    EXPECT_FALSE(service.Request(24, error));
    EXPECT_NE(error.find("restart"), std::string::npos);
    EXPECT_FALSE(service.Busy());
}

TEST_F(ScriptBuildTest, ShutdownCancelsOnlyOwnedCompilerAndJoinsWorker)
{
    Write("Gameplay/Gameplay.csproj", "<Project Sdk=\"Microsoft.NET.Sdk\"><PropertyGroup>"
        "<TargetFramework>net8.0</TargetFramework></PropertyGroup></Project>");
    Write("Gameplay/Test.cs", "public class Test {}");
    VL::ScriptBuildService service;
    service.Configure(VL_SCRIPT_DOTNET_EXECUTABLE, root / "Gameplay/Gameplay.csproj",
        root / "output", std::filesystem::path(VL_SCRIPT_OUTPUT_DIRECTORY) / "VulkanLearn.Runtime.dll", "Debug");
    std::string error;
    ASSERT_TRUE(service.Request(25, error)) << error;
    const auto started = std::chrono::steady_clock::now();
    service.Shutdown();
    EXPECT_LT(std::chrono::steady_clock::now() - started, std::chrono::seconds(5));
    EXPECT_FALSE(service.Busy());
    EXPECT_FALSE(service.Poll());
}

TEST_F(ScriptBuildTest, CompilerFailurePublishesSourceLocationAndPreservesBridge)
{
    Write("Gameplay/Gameplay.csproj", "<Project Sdk=\"Microsoft.NET.Sdk\"><PropertyGroup>"
        "<TargetFramework>net8.0</TargetFramework></PropertyGroup></Project>");
    Write("Gameplay/Broken.cs", "public class Broken { invalid syntax here; }");
    VL::ScriptBuildService service;
    service.Configure(VL_SCRIPT_DOTNET_EXECUTABLE, root / "Gameplay/Gameplay.csproj",
        root / "output", std::filesystem::path(VL_SCRIPT_OUTPUT_DIRECTORY) / "VulkanLearn.Runtime.dll", "Debug");
    std::string error;
    ASSERT_TRUE(service.Request(19, error)) << error;
    EXPECT_FALSE(service.Request(19, error));
    std::optional<VL::ScriptBuildResult> result;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(45);
    while (!result && std::chrono::steady_clock::now() < deadline)
    {
        result = service.Poll();
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    ASSERT_TRUE(result);
    EXPECT_FALSE(result->succeeded);
    EXPECT_EQ(19u, result->worldGeneration);
    EXPECT_NE(std::string::npos, result->log.find("Broken.cs("));
    EXPECT_NE(std::string::npos, result->log.find("error CS"));
}
#endif
