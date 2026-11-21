#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <mutex>
#include <string_view>

#include <nlohmann/json_fwd.hpp>

namespace VL
{
namespace RuntimeEvidence
{
enum class Status { Disabled, Recorded };

struct Limits
{
    uint64_t maxBytes = 128ull * 1024 * 1024;
    uint64_t maxRecords = 1000000;
    size_t maxLineBytes = 64 * 1024;
};

struct Cost
{
    uint64_t cpuBytes = 0;
    uint64_t gpuBytes = 0;
    uint64_t fixedMetadataBytes = 0;
    uint64_t workspaceBytes = 0;
};

struct Statistics
{
    bool enabled = false;
    bool incomplete = false;
    bool closed = false;
    uint64_t records = 0;
    uint64_t bytes = 0;
    uint64_t cpuNanoseconds = 0;
    uint64_t peakCallNanoseconds = 0;
    size_t fixedMetadataBytes = 0;
    size_t peakLineBytes = 0;
};

class Sink
{
public:
    static constexpr size_t LineCapacity = 64 * 1024;
    static constexpr size_t PathCapacity = 4096;
    static constexpr std::string_view OutputCategory = "explicitoutput";

    explicit Sink(const std::filesystem::path& packageRoot, Limits limits = {});
    ~Sink() noexcept;
    Sink(const Sink&) = delete;
    Sink& operator=(const Sink&) = delete;

    static Cost CostQuote() noexcept;
    bool IsEnabled() const noexcept;
    Status Record(std::string_view eventClass, const nlohmann::json& payload);
    Status Flush();
    Status Close(bool incomplete = false);
    Statistics GetStatistics() const;

private:
#if defined(VL_FILE_IO_TEST_HOOKS)
    friend struct TestAccess;
#endif
    void Open();
    void Write(std::string_view eventClass, const nlohmann::json& payload, bool terminal);
    [[noreturn]] void Fail(const char* reason, bool ioFailure = false);

    mutable std::mutex mutex;
    std::array<char, LineCapacity> line{};
    std::array<char, PathCapacity> output{};
    Limits limits;
    Statistics stats;
    FILE* file = nullptr;
    bool ioFailed = false;
};

Cost CostQuote() noexcept;
Status Initialize(const std::filesystem::path& packageRoot, Limits limits = {});
bool IsEnabled();
Status Record(std::string_view eventClass, const nlohmann::json& payload);
Status Flush();
Status Close(bool incomplete = false);
Statistics GetStatistics();
bool HasIncompleteSession() noexcept;
void MarkIncomplete() noexcept;

class ApplicationScope
{
public:
    explicit ApplicationScope(Sink* sink = nullptr) noexcept;
    ~ApplicationScope() noexcept;
    ApplicationScope(const ApplicationScope&) = delete;
    ApplicationScope& operator=(const ApplicationScope&) = delete;
    int Finish(int exitCode) noexcept;

private:
    Sink* sink;
    bool finished = false;
};
}
}
