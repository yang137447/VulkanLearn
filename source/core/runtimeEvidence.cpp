#include "core/runtimeEvidence.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <ostream>
#include <stdexcept>
#include <streambuf>
#include <string>

#include <nlohmann/json.hpp>

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <fcntl.h>
#include <sys/syscall.h>
#include <time.h>
#include <unistd.h>
#endif

namespace VL
{
namespace RuntimeEvidence
{
namespace
{
constexpr size_t TerminalReserve = 2048;
std::atomic<bool> incompleteSession{false};

struct ProcessState
{
    std::mutex initializeMutex;
    std::unique_ptr<Sink> owner;
    std::atomic<Sink*> published{nullptr};
};

ProcessState& Process()
{
    static ProcessState state;
    return state;
}

bool EnvironmentEnabled()
{
    const char* configured = std::getenv("VULKANLEARN_RUNTIME_EVIDENCE");
    return configured && *configured;
}

uint64_t SteadyNanoseconds()
{
    return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count());
}

uint64_t ThreadTag()
{
#if defined(_WIN32)
    return GetCurrentThreadId();
#else
    return static_cast<uint64_t>(syscall(SYS_gettid));
#endif
}

uint64_t ThreadCpuNanoseconds()
{
#if defined(_WIN32)
    FILETIME creation{}, exit{}, kernel{}, user{};
    if (!GetThreadTimes(GetCurrentThread(), &creation, &exit, &kernel, &user))
        throw std::runtime_error("runtimeEvidence: thread CPU clock unavailable");
    return (((uint64_t(kernel.dwHighDateTime) << 32) | kernel.dwLowDateTime) +
        ((uint64_t(user.dwHighDateTime) << 32) | user.dwLowDateTime)) * 100;
#else
    timespec value{};
    if (clock_gettime(CLOCK_THREAD_CPUTIME_ID, &value) != 0)
        throw std::runtime_error("runtimeEvidence: thread CPU clock unavailable");
    return uint64_t(value.tv_sec) * 1000000000 + value.tv_nsec;
#endif
}

class CallCost
{
public:
    CallCost(Statistics& stats, uint64_t start) :
        stats(stats), start(start), cpu(ThreadCpuNanoseconds()) {}
    ~CallCost() noexcept
    {
        try
        {
            stats.cpuNanoseconds += ThreadCpuNanoseconds() - cpu;
            stats.peakCallNanoseconds = std::max(stats.peakCallNanoseconds,
                SteadyNanoseconds() - start);
        }
        catch (...)
        {
            stats.incomplete = true;
            incompleteSession.store(true);
        }
    }
private:
    Statistics& stats;
    uint64_t start;
    uint64_t cpu;
};

class FixedBuffer : public std::streambuf
{
public:
    FixedBuffer(char* data, size_t capacity) { setp(data, data + capacity); }
    size_t Size() const { return static_cast<size_t>(pptr() - pbase()); }
protected:
    int_type overflow(int_type) override { return traits_type::eof(); }
};

void ValidatePayload(const nlohmann::json& value, size_t depth, size_t& remaining)
{
    if (depth > 32 || remaining == 0)
        throw std::runtime_error("runtimeEvidence: payload complexity limit");
    --remaining;
    if (value.is_discarded() || value.is_binary() ||
        (value.is_number_float() && !std::isfinite(value.get<double>())))
        throw std::runtime_error("runtimeEvidence: payload must be lossless JSON");
    if (value.is_string())
    {
        const auto size = value.get_ref<const std::string&>().size();
        if (size > remaining)
            throw std::runtime_error("runtimeEvidence: payload string limit");
        remaining -= size;
    }
    if (value.is_structured())
        for (auto iterator = value.begin(); iterator != value.end(); ++iterator)
        {
            if (value.is_object())
            {
                if (iterator.key().size() > remaining)
                    throw std::runtime_error("runtimeEvidence: payload key limit");
                remaining -= iterator.key().size();
            }
            ValidatePayload(iterator.value(), depth + 1, remaining);
        }
}

std::string PathKey(const std::filesystem::path& path)
{
    auto key = std::filesystem::weakly_canonical(path).generic_u8string();
#if defined(_WIN32)
    std::transform(key.begin(), key.end(), key.begin(), [](unsigned char character)
    {
        return character >= 'A' && character <= 'Z' ? character + ('a' - 'A') : character;
    });
#endif
    while (key.size() > 1 && key.back() == '/')
        key.pop_back();
    return key;
}

bool Within(const std::string& path, const std::string& root)
{
    return path == root || (path.size() > root.size() &&
        path.compare(0, root.size(), root) == 0 &&
        (root.back() == '/' || path[root.size()] == '/'));
}

Sink* GlobalSink()
{
    auto* sink = Process().published.load(std::memory_order_acquire);
    if (!sink && EnvironmentEnabled())
    {
        incompleteSession.store(true);
        throw std::runtime_error("runtimeEvidence: Initialize(packageRoot) required after root resolution");
    }
    return sink;
}
}

Sink::Sink(const std::filesystem::path& packageRoot, Limits requestedLimits) : limits(requestedLimits)
{
    stats.fixedMetadataBytes = static_cast<size_t>(CostQuote().fixedMetadataBytes);
    const char* configured = std::getenv("VULKANLEARN_RUNTIME_EVIDENCE");
    stats.enabled = configured && *configured;
    if (!stats.enabled)
        return;
    try
    {
        CallCost cost(stats, SteadyNanoseconds());
        size_t size = 0;
        while (size < output.size() && configured[size])
            ++size;
        if (size == output.size())
            Fail("output path exceeds limit");
        if (limits.maxLineBytes < TerminalReserve || limits.maxLineBytes > LineCapacity ||
            limits.maxBytes < TerminalReserve || limits.maxRecords < 2 ||
            limits.maxBytes > Limits{}.maxBytes || limits.maxRecords > Limits{}.maxRecords)
            Fail("invalid sink limits");
        const auto path = std::filesystem::u8path(configured);
        if (!path.is_absolute() || path.filename().empty() ||
            !std::filesystem::is_directory(path.parent_path()))
            Fail("output requires an absolute file path with an existing parent");
        const auto canonical = std::filesystem::canonical(path.parent_path()) / path.filename();
        const auto& root = packageRoot;
        if (!root.is_absolute() || root.native().size() >= PathCapacity ||
            Within(PathKey(canonical), PathKey(root)))
            Fail("output must be outside the package root");
        const auto name = canonical.u8string();
        if (name.size() >= output.size())
            Fail("canonical output path exceeds limit");
        std::memcpy(output.data(), name.c_str(), name.size() + 1);
    }
    catch (...)
    {
        stats.incomplete = true;
        incompleteSession.store(true);
        throw;
    }
}

Sink::~Sink() noexcept
{
    try { Close(); }
    catch (...) { incompleteSession.store(true); }
}

Cost Sink::CostQuote() noexcept
{
    return {sizeof(Sink) + 64 * 1024, 0, sizeof(Sink), 64 * 1024};
}

bool Sink::IsEnabled() const noexcept { return stats.enabled; }

[[noreturn]] void Sink::Fail(const char* reason, bool ioFailure)
{
    stats.incomplete = true;
    ioFailed = ioFailed || ioFailure;
    incompleteSession.store(true);
    throw std::runtime_error(std::string("runtimeEvidence: ") + reason);
}

void Sink::Open()
{
    if (file)
        return;
#if defined(_WIN32)
    const auto path = std::filesystem::u8path(output.data());
    file = _wfopen(path.c_str(), L"wbx");
#else
    const int descriptor = open(output.data(), O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0600);
    if (descriptor >= 0)
    {
        file = fdopen(descriptor, "wb");
        if (!file)
            close(descriptor);
    }
#endif
    if (!file)
        Fail("exclusive output open failed", true);
    if (std::setvbuf(file, nullptr, _IONBF, 0) != 0)
        Fail("unbuffered output setup failed", true);
}

void Sink::Write(std::string_view eventClass, const nlohmann::json& payload, bool terminal)
{
    if (eventClass.empty() || eventClass.size() > 256)
        Fail("event class limit");
    size_t remaining = LineCapacity;
    ValidatePayload(payload, 0, remaining);
    FixedBuffer buffer(line.data(), limits.maxLineBytes);
    std::ostream stream(&buffer);
    stream << "{\"sequence\":" << stats.records + 1
        << ",\"steady_ns\":" << SteadyNanoseconds()
        << ",\"threadTag\":" << ThreadTag()
        << ",\"outputCategory\":\"explicitoutput\",\"eventClass\":"
        << nlohmann::json(std::string(eventClass))
        << ",\"payload\":" << payload << "}\n";
    if (!stream)
        Fail("JSONL line limit exceeded; record rejected");
    const auto size = buffer.Size();
    const auto byteReserve = terminal ? 0 : TerminalReserve;
    const auto recordReserve = terminal ? 0 : 1;
    if (stats.records >= limits.maxRecords - recordReserve ||
        size + byteReserve > limits.maxBytes - stats.bytes)
        Fail("evidence capacity exceeded");
    Open();
    const auto written = std::fwrite(line.data(), 1, size, file);
    stats.bytes += written;
    if (written != size || std::fflush(file) != 0)
        Fail("record write or flush failed", true);
    ++stats.records;
    stats.peakLineBytes = std::max(stats.peakLineBytes, size);
}

Status Sink::Record(std::string_view eventClass, const nlohmann::json& payload)
{
    if (!IsEnabled())
        return Status::Disabled;
    const auto start = SteadyNanoseconds();
    std::lock_guard<std::mutex> lock(mutex);
    try
    {
        CallCost cost(stats, start);
        if (stats.closed || stats.incomplete)
            Fail("record after close or incomplete session");
        Write(eventClass, payload, false);
        return Status::Recorded;
    }
    catch (...)
    {
        stats.incomplete = true;
        incompleteSession.store(true);
        throw;
    }
}

Status Sink::Flush()
{
    if (!IsEnabled())
        return Status::Disabled;
    const auto start = SteadyNanoseconds();
    std::lock_guard<std::mutex> lock(mutex);
    CallCost cost(stats, start);
    if (stats.incomplete)
        Fail("flush of incomplete session");
    if (file && std::fflush(file) != 0)
        Fail("explicit flush failed", true);
    return Status::Recorded;
}

Status Sink::Close(bool incomplete)
{
    if (!IsEnabled())
        return Status::Disabled;
    const auto start = SteadyNanoseconds();
    std::lock_guard<std::mutex> lock(mutex);
    CallCost cost(stats, start);
    if (incomplete)
    {
        stats.incomplete = true;
        incompleteSession.store(true);
    }
    if (stats.closed)
    {
        if (stats.incomplete)
            Fail("closed incomplete session");
        return Status::Recorded;
    }
    try
    {
        if (!ioFailed)
            Write("runtimeEvidenceEnd", {
                {"incomplete", stats.incomplete}, {"recordsBeforeTerminal", stats.records},
                {"bytesBeforeTerminal", stats.bytes}, {"fixedMetadataBytes", stats.fixedMetadataBytes},
                {"cpuNanoseconds", stats.cpuNanoseconds},
                {"peakCallNanoseconds", stats.peakCallNanoseconds}}, true);
        const auto closing = file;
        file = nullptr;
        stats.closed = true;
        if (closing && std::fclose(closing) != 0)
            Fail("terminal close failed", true);
        if (stats.incomplete)
            Fail("session ended incomplete");
    }
    catch (...)
    {
        stats.closed = true;
        stats.incomplete = true;
        incompleteSession.store(true);
        if (file)
        {
            std::fclose(file);
            file = nullptr;
        }
        throw;
    }
    return Status::Recorded;
}

Statistics Sink::GetStatistics() const
{
    std::lock_guard<std::mutex> lock(mutex);
    return stats;
}

Cost CostQuote() noexcept
{
    auto cost = Sink::CostQuote();
    cost.fixedMetadataBytes += sizeof(ProcessState);
    cost.cpuBytes += sizeof(ProcessState);
    return cost;
}
Status Initialize(const std::filesystem::path& packageRoot, Limits limits)
{
    if (!EnvironmentEnabled())
        return Status::Disabled;
    auto& process = Process();
    std::lock_guard<std::mutex> lock(process.initializeMutex);
    if (process.owner)
        throw std::runtime_error("runtimeEvidence: process sink already initialized");
    process.owner = std::make_unique<Sink>(packageRoot, limits);
    process.published.store(process.owner.get(), std::memory_order_release);
    return Status::Recorded;
}
bool IsEnabled()
{
    const auto* sink = Process().published.load(std::memory_order_acquire);
    return sink && sink->IsEnabled();
}
Status Record(std::string_view eventClass, const nlohmann::json& payload)
{
    auto* sink = GlobalSink();
    return sink ? sink->Record(eventClass, payload) : Status::Disabled;
}
Status Flush()
{
    auto* sink = GlobalSink();
    return sink ? sink->Flush() : Status::Disabled;
}
Status Close(bool incomplete)
{
    auto* sink = Process().published.load(std::memory_order_acquire);
    return sink ? sink->Close(incomplete) : Status::Disabled;
}
Statistics GetStatistics()
{
    auto* sink = GlobalSink();
    return sink ? sink->GetStatistics() : Statistics{};
}
bool HasIncompleteSession() noexcept { return incompleteSession.load(); }
void MarkIncomplete() noexcept { incompleteSession.store(true); }

ApplicationScope::ApplicationScope(Sink* sink) noexcept : sink(sink) {}

ApplicationScope::~ApplicationScope() noexcept
{
    if (finished)
        return;
    try
    {
        if (sink)
            sink->Close(true);
        else
            Close(true);
    }
    catch (...) { incompleteSession.store(true); }
}

int ApplicationScope::Finish(int exitCode) noexcept
{
    finished = true;
    try
    {
        const bool incomplete = exitCode != 0 ||
            (sink ? sink->GetStatistics().incomplete : HasIncompleteSession());
        if (sink)
            sink->Close(incomplete);
        else
            Close(incomplete);
        return incomplete && exitCode == 0 ? 1 : exitCode;
    }
    catch (...) { return exitCode == 0 ? 1 : exitCode; }
}
}
}
