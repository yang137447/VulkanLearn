#pragma once

// File responsibility: Polls the shader source-of-truth tree and publishes
// debounced, BLAKE3-256-confirmed change batches. It never touches Vulkan,
// live material/pipeline caches, or the build-cache manifest.

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <future>
#include <map>
#include <optional>
#include <string>
#include <vector>

namespace VL
{

class ShaderFileMonitor
{
public:
    struct ChangeBatch
    {
        // Content transitions first observed by this scan. These advance the
        // source epoch immediately, before debounce allows compilation.
        std::vector<std::string> observedSources;
        // Normalized source identities relative to shader/glsl with '/'
        // separators, sorted and unique. These have completed debounce.
        std::vector<std::string> changedSources;
    };

    void Initialize(
        const std::filesystem::path& shaderRoot,
        std::chrono::milliseconds pollInterval =
            std::chrono::milliseconds(250),
        size_t requiredStablePolls = 2);

    // Safe to call once per game frame. Performs real scanning only after the
    // configured poll interval elapses. Returns a batch when at least one
    // source has been stable across the configured number of polls.
    std::optional<ChangeBatch> Poll();

    // Advances the baseline for a set of normalized source identities without
    // emitting events. Used after a manual or committed reload so the monitor
    // does not re-report sources whose disk state is already authoritative.
    void RefreshBaselineForSources(
        const std::vector<std::string>& normalizedSources);

    size_t GetTrackedSourceCount() const;
    size_t GetScanCount() const { return scanCount; }
    size_t GetHashCount() const { return hashCount; }
    bool HasUnstableSourceChanges() const;
    void SetTestPollInterval(std::chrono::milliseconds interval)
    {
        pollInterval = interval;
        lastScanTime = {};
        backgroundScan = false;
        ResetPendingScan();
    }
    void SetTestScanSuspended(bool suspended)
    {
        testScanSuspended = suspended;
        backgroundScan = false;
        ResetPendingScan();
    }

    // 生产入口在 Initialize 后开启：目录遍历在后台线程执行，GT 只做比对。
    // 默认关闭以保留测试/运行时校验需要的“调用即扫描”同步语义。
    void SetBackgroundScanEnabled(bool enabled) { backgroundScan = enabled; }

    // Source-of-truth filter. Returns true for hand-written shader sources and
    // M_ material definitions, and false for generated includes, temporary
    // editor files, and everything outside the shader/glsl tree.
    static bool IsSourceOfTruthPath(
        const std::filesystem::path& relativePath);
    static bool IsTemporaryFileName(
        const std::filesystem::path& fileName);

private:
    struct TrackedSource
    {
        std::filesystem::path absolutePath;
        bool exists = false;
        std::uintmax_t size = 0;
        std::optional<std::filesystem::file_time_type> lastWriteTime;
        // Last emitted/confirmed digest; empty means "known to be missing".
        std::string baselineDigest;
        // Debounce state for the current candidate transition.
        std::string candidateDigest;
        size_t stablePollCount = 0;
        size_t nextFullHashScan = 0;
    };

    void ScanSources(
        std::map<std::string, TrackedSource>& scannedSources) const;
    std::string HashSourceOrDefer(
        const std::filesystem::path& path) const;
    // 切换回同步语义前丢弃 in-flight 后台扫描，避免恢复后消费挂起前的旧结果。
    void ResetPendingScan();

    std::filesystem::path glslRoot;
    std::chrono::milliseconds pollInterval{250};
    size_t requiredStablePolls = 2;
    std::chrono::steady_clock::time_point lastScanTime{};
    std::map<std::string, TrackedSource> sources;
    size_t fullHashEveryNPolls = 4;
    size_t scanCount = 0;
    mutable size_t hashCount = 0;
    bool testScanSuspended = false;
    // Windows 下逐文件 stat 220 个 shader 约 100ms，同步执行会形成每
    // 250ms 一次的可见卡顿；生产路径改为后台扫描。
    bool backgroundScan = false;
    std::future<std::map<std::string, TrackedSource>> pendingScan;
    bool initialized = false;
    bool baselineSeeded = false;
};

} // namespace VL
