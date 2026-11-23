#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <optional>

namespace VL
{
namespace RuntimeFileTrace
{
enum class RootScope { Package, Project, SourceAuthor, Cache, System, ExplicitOutput, Unknown };
enum class Event { Resolve, NativeReadOpen, StreamReadOpen, ReaderValidation, ResolveResult };
enum class Access { Read, Write };

struct Statistics
{
    uint64_t records = 0;
    uint64_t bytes = 0;
    uint64_t traceCpuNanoseconds = 0;
    uint64_t peakCallNanoseconds = 0;
    size_t fixedMetadataBytes = 0;
    size_t peakRecordBytes = 0;
    size_t knownStringHeapCapacityBytes = 0;
    bool enabled = false;
    bool incomplete = false;
};

constexpr uint64_t MaxRecords = 100000;
constexpr uint64_t MaxBytes = 16 * 1024 * 1024;
constexpr size_t MaxPathCharacters = 2048;
constexpr size_t MaxPathComponents = 64;
constexpr uint32_t MaxReaderObservers = 16;

struct Cost
{
    uint64_t cpuBytes = 0;
    uint64_t gpuBytes = 0;
    uint64_t fixedMetadataBytes = 0;
    uint64_t workspaceBytes = 0;
};

bool IsEnabled() noexcept;
Cost CostQuote() noexcept;
void SetRoot(RootScope scope, const std::filesystem::path& root) noexcept;
RootScope Classify(const std::filesystem::path& absolutePath) noexcept;
void Record(Event event, const std::filesystem::path& requestedPath, bool success,
    const char* owner, const std::filesystem::path* finalHandlePath = nullptr,
    std::optional<uint64_t> generation = std::nullopt, Access access = Access::Read,
    std::optional<int64_t> errorCode = std::nullopt, const char* errorDomain = nullptr) noexcept;
void Flush() noexcept;
void MarkIncomplete() noexcept;
Statistics GetStatistics() noexcept;
}
}
