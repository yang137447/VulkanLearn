#pragma once

// File responsibility: Polls the shader source-of-truth tree and publishes
// debounced, BLAKE3-256-confirmed change batches. It never touches Vulkan,
// live material/pipeline caches, or the build-cache manifest.

#include <chrono>
#include <cstdint>
#include <filesystem>
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
    bool HasUnstableSourceChanges() const;
    void SetTestPollInterval(std::chrono::milliseconds interval)
    {
        pollInterval = interval;
        lastScanTime = {};
    }
    void SetTestScanSuspended(bool suspended)
    {
        testScanSuspended = suspended;
    }

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
    };

    void ScanSources(
        std::map<std::string, TrackedSource>& scannedSources) const;
    std::string HashSourceOrDefer(
        const std::filesystem::path& path) const;

    std::filesystem::path glslRoot;
    std::chrono::milliseconds pollInterval{250};
    size_t requiredStablePolls = 2;
    std::chrono::steady_clock::time_point lastScanTime{};
    std::map<std::string, TrackedSource> sources;
    size_t pollsSinceFullHash = 0;
    size_t fullHashEveryNPolls = 4;
    size_t scanCount = 0;
    bool testScanSuspended = false;
    bool initialized = false;
    bool baselineSeeded = false;
};

} // namespace VL
