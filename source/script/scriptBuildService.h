#pragma once

#include <atomic>
#include <filesystem>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

namespace VL
{
// 编译 worker 只生成独立目录及诊断；GT 取结果后决定是否重载，不持有 CLR/World/UI。
struct ScriptBuildResult
{
    bool succeeded = false;
    bool cancelled = false;
    uint64_t worldGeneration = 0;
    std::filesystem::path assembly;
    std::string sourceDigest;
    std::string log;
};

// 所有配置与投递在 GT，单个 worker 运行 dotnet；Shutdown 终止自有进程树再 join。
class ScriptBuildService
{
public:
    ~ScriptBuildService() { Shutdown(); }
    void Configure(std::filesystem::path dotnet, std::filesystem::path project,
        std::filesystem::path outputRoot, std::filesystem::path bridge, std::string configuration,
        std::filesystem::path resourceRoot = {}, std::filesystem::path runtimeSources = {});
    bool Available() const;
    bool Busy() const { return running.load(); }
    bool Request(uint64_t generation, std::string& error);
    std::optional<ScriptBuildResult> Poll();
    bool ObserveChanges(float dt);
    void Shutdown() noexcept;
    static std::string SourceDigest(const std::filesystem::path& sourceRoot);
    static std::wstring QuoteArgument(const std::wstring& argument);
private:
    std::string CurrentSourceDigest() const;
    void Compile(uint64_t generation, std::filesystem::path output, std::string digest);
    std::filesystem::path dotnet, project, outputRoot, bridge, resourceRoot, runtimeSources;
    std::string configuration, bridgeDigest, bridgeSourceDigest, observedDigest, pendingDigest;
    float pollTime = 0;
    std::atomic<bool> running{false}, stopping{false};
    std::thread worker;
    std::mutex mutex;
    std::optional<ScriptBuildResult> completed;
    uint64_t sequence = 0;
};
}
