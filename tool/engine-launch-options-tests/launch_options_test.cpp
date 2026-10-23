#include <gtest/gtest.h>

#include <string>
#include <vector>

#include "engine/engineLoop.h"
#include "engine/launchOptions.h"

namespace VL
{

// launchOptions.cpp 也负责把选项翻译成 EngineLoop 上的启动命令；解析测试
// 不启动引擎，因此只需要三个入口的空实现来闭合链接。
void EngineLoop::QueueRuntimeCommand(RuntimeCommand) {}
void EngineLoop::SetExitAfterRuntimeTests(bool) {}
void EngineLoop::QueueLaunchConsoleCommands(const std::vector<std::string>&, int) {}

namespace
{

LaunchOptions Parse(const std::vector<std::string>& arguments)
{
    std::vector<std::string> owned = arguments;
    std::vector<char*> argv;
    argv.reserve(owned.size());
    for (std::string& argument : owned)
    {
        argv.push_back(argument.data());
    }
    return ParseLaunchOptions(static_cast<int>(argv.size()), argv.data());
}

} // namespace

TEST(LaunchOptions, GpuStatsIsOffByDefault)
{
    const LaunchOptions options = Parse({"main.exe"});
    EXPECT_TRUE(options.errorMessage.empty());
    EXPECT_FALSE(options.gpuStats);
    EXPECT_FALSE(options.perfJsonlPath.has_value());
}

TEST(LaunchOptions, PerfJsonlRequiresAPath)
{
    const LaunchOptions missing = Parse({"main.exe", "--perf-jsonl"});
    EXPECT_FALSE(missing.errorMessage.empty());
    EXPECT_NE(missing.errorMessage.find("--perf-jsonl requires a file path"),
        std::string::npos);

    const LaunchOptions nextFlag = Parse({"main.exe", "--perf-jsonl", "--gpu-stats"});
    EXPECT_FALSE(nextFlag.errorMessage.empty());
    EXPECT_NE(nextFlag.errorMessage.find("--perf-jsonl requires a file path"),
        std::string::npos);
    EXPECT_FALSE(nextFlag.perfJsonlPath.has_value());
}

TEST(LaunchOptions, PerfJsonlAcceptsAPathAndCombinesWithGpuStats)
{
    const LaunchOptions options = Parse(
        {"main.exe", "--gpu-stats", "--perf-jsonl", "artifacts/perf/run.jsonl"});
    EXPECT_TRUE(options.errorMessage.empty()) << options.errorMessage;
    EXPECT_TRUE(options.gpuStats);
    ASSERT_TRUE(options.perfJsonlPath.has_value());
    EXPECT_EQ(*options.perfJsonlPath, "artifacts/perf/run.jsonl");
}

TEST(LaunchOptions, GpuStatsFlagIsAccepted)
{
    const LaunchOptions options = Parse({"main.exe", "--gpu-stats"});
    EXPECT_TRUE(options.errorMessage.empty());
    EXPECT_TRUE(options.gpuStats);
}

TEST(LaunchOptions, GpuStatsCombinesWithExistingFlags)
{
    const LaunchOptions options = Parse(
        {"main.exe", "--gpu-stats", "--frame-limit", "60", "--initial-scene", "Maps/SC_x.json"});
    EXPECT_TRUE(options.errorMessage.empty());
    EXPECT_TRUE(options.gpuStats);
    EXPECT_EQ(options.frameLimitHz, 60);
    EXPECT_EQ(options.initialSceneOverride, "Maps/SC_x.json");
}

TEST(LaunchOptions, PrerenderSkinIsOnByDefault)
{
    const LaunchOptions options = Parse({"main.exe"});
    EXPECT_TRUE(options.errorMessage.empty());
    EXPECT_TRUE(options.prerenderSkin);
}

TEST(LaunchOptions, PrerenderSkinFlagSelectsThePreSkinnedVariant)
{
    const LaunchOptions options = Parse(
        {"main.exe", "--prerender-skin", "--gpu-stats", "--frame-limit", "60"});
    EXPECT_TRUE(options.errorMessage.empty()) << options.errorMessage;
    EXPECT_TRUE(options.prerenderSkin);
    EXPECT_TRUE(options.gpuStats);
    EXPECT_EQ(options.frameLimitHz, 60);
}

TEST(LaunchOptions, NoPrerenderSkinFlagFallsBackToPerPassSkinning)
{
    // 关闭项必须覆盖默认值，否则 A/B 与回退路径无从验证。
    const LaunchOptions options = Parse({"main.exe", "--no-prerender-skin"});
    EXPECT_TRUE(options.errorMessage.empty()) << options.errorMessage;
    EXPECT_FALSE(options.prerenderSkin);
}

TEST(LaunchOptions, UnknownFlagsAreStillRejected)
{
    const LaunchOptions options = Parse({"main.exe", "--gpu-stat"});
    EXPECT_FALSE(options.errorMessage.empty());
    EXPECT_NE(options.errorMessage.find("Unknown launch option"), std::string::npos);
    EXPECT_FALSE(options.gpuStats);
}

} // namespace VL
