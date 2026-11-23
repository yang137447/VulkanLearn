#include "core/runtimeFileTrace.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#else
#include <fcntl.h>
#include <time.h>
#include <unistd.h>
#endif

namespace VL
{
namespace RuntimeFileTrace
{
namespace
{
constexpr size_t PathLimit = MaxPathCharacters;
constexpr size_t RecordCapacity = 16384;
constexpr uint64_t TerminalReserve = 2048;
constexpr const char* ScopeNames[] = {
    "package", "project", "sourceauthor", "cache", "system", "explicitoutput", "unknown"};
constexpr const char* EventNames[] = {
    "resolve", "nativeReadOpen", "streamReadOpen", "readerValidation", "resolveResult"};

template<typename String>
bool BoundedComponents(const String& path)
{
    size_t separators = 0;
    for (auto character : path)
        if (character == '/' || character == '\\')
            ++separators;
    return separators <= MaxPathComponents;
}

class ErrorState
{
public:
    ErrorState() : savedErrno(errno)
#if defined(_WIN32)
        , savedWindowsError(GetLastError())
#endif
    {}
    ~ErrorState()
    {
        errno = savedErrno;
#if defined(_WIN32)
        SetLastError(savedWindowsError);
#endif
    }
private:
    int savedErrno;
#if defined(_WIN32)
    DWORD savedWindowsError;
#endif
};

std::filesystem::path Absolute(const std::filesystem::path& path)
{
    if (path.native().size() > PathLimit || !BoundedComponents(path.native()))
        throw std::runtime_error("trace path exceeds limit");
    auto name = path.native();
#if defined(_WIN32)
    if (name.compare(0, 8, L"\\\\?\\UNC\\") == 0)
        name = L"\\\\" + name.substr(8);
    else if (name.compare(0, 4, L"\\\\?\\") == 0)
        name = name.substr(4);
#endif
    auto result = std::filesystem::absolute(std::filesystem::path(name)).lexically_normal();
    if (result.native().size() > PathLimit || !BoundedComponents(result.native()))
        throw std::runtime_error("trace absolute path exceeds limit");
    return result;
}

std::string PathKey(const std::filesystem::path& path)
{
    auto absolute = Absolute(path);
#if defined(_WIN32)
    auto native = absolute.native();
    std::wstring folded(native.size(), L'\0');
    if (LCMapStringEx(LOCALE_NAME_INVARIANT, LCMAP_LOWERCASE, native.data(),
        static_cast<int>(native.size()), folded.data(), static_cast<int>(folded.size()),
        nullptr, nullptr, 0) == 0)
        throw std::runtime_error("trace path case mapping failed");
    absolute = std::filesystem::path(folded);
#endif
    auto key = absolute.generic_u8string();
    if (key.size() > PathLimit)
        throw std::runtime_error("trace UTF-8 path exceeds limit");
    while (key.size() > 1 && key.back() == '/')
        key.pop_back();
    return key;
}

bool Within(const std::string& path, const std::string& root)
{
    return !root.empty() && (path == root ||
        (path.size() > root.size() && path.compare(0, root.size(), root) == 0 &&
            (root.back() == '/' || path[root.size()] == '/')));
}

uint64_t ThreadCpuNanoseconds()
{
#if defined(_WIN32)
    FILETIME created{}, exited{}, kernel{}, user{};
    if (!GetThreadTimes(GetCurrentThread(), &created, &exited, &kernel, &user))
        return 0;
    return (((uint64_t(kernel.dwHighDateTime) << 32) | kernel.dwLowDateTime) +
        ((uint64_t(user.dwHighDateTime) << 32) | user.dwLowDateTime)) * 100;
#else
    timespec value{};
    if (clock_gettime(CLOCK_THREAD_CPUTIME_ID, &value) != 0)
        return 0;
    return uint64_t(value.tv_sec) * 1000000000 + value.tv_nsec;
#endif
}

struct State
{
    std::mutex mutex;
    std::array<std::string, 6> roots;
    std::filesystem::path output;
    std::string outputKey;
    std::array<char, RecordCapacity> record{};
    std::array<char, 65536> fileBuffer{};
    FILE* file = nullptr;
    Statistics stats;
    bool attempted = false;
    std::atomic<bool> stopped{false};
    bool terminalWritten = false;

    State()
    {
        stats.fixedMetadataBytes = sizeof(State);
        const char* value = std::getenv("VULKANLEARN_RUNTIME_FILE_TRACE");
        stats.enabled = value && *value;
        if (!stats.enabled)
            return;
        try
        {
            if (std::strlen(value) > PathLimit || !BoundedComponents(std::string_view(value)))
                throw std::runtime_error("trace output exceeds path limit");
            output = std::filesystem::u8path(value);
            if (!output.is_absolute() || output.native().size() > PathLimit)
                throw std::runtime_error("trace requires a bounded absolute output path");
            const char* variables[] = {
                "VULKANLEARN_RUNTIME_FILE_TRACE_PACKAGE_ROOT",
                "VULKANLEARN_RUNTIME_FILE_TRACE_PROJECT_ROOT",
                "VULKANLEARN_RUNTIME_FILE_TRACE_SOURCEAUTHOR_ROOT",
                "VULKANLEARN_RUNTIME_FILE_TRACE_CACHE_ROOT",
                "VULKANLEARN_RUNTIME_FILE_TRACE_SYSTEM_ROOT",
                "VULKANLEARN_RUNTIME_FILE_TRACE_EXPLICITOUTPUT_ROOT"};
            for (size_t index = 0; index < roots.size(); ++index)
                if (const char* root = std::getenv(variables[index]); root && *root)
                {
                    if (std::strlen(root) > PathLimit || !BoundedComponents(std::string_view(root)))
                        throw std::runtime_error("trace root exceeds path limit");
                    roots[index] = PathKey(std::filesystem::weakly_canonical(std::filesystem::u8path(root)));
                    if (roots[index].size() > PathLimit)
                        throw std::runtime_error("trace root exceeds UTF-8 path limit");
                }
        }
        catch (...)
        {
            stopped = true;
            stats.incomplete = true;
            std::fputs("runtimeFileTrace incomplete: invalid environment configuration\n", stderr);
        }
    }

    ~State()
    {
        if (file)
        {
            if (!terminalWritten)
                Terminal("traceEnd", "observedAPIOnly");
            std::fclose(file);
        }
    }

    RootScope ClassifyKey(const std::string& path) const
    {
        size_t longest = 0;
        RootScope result = RootScope::Unknown;
        for (size_t index = 0; index < roots.size(); ++index)
            if (Within(path, roots[index]) && roots[index].size() > longest)
            {
                longest = roots[index].size();
                result = static_cast<RootScope>(index);
            }
        return result;
    }

    size_t KnownStringHeapCapacityBytes() const
    {
        size_t bytes = 0;
        for (const auto& root : roots)
            if (root.capacity() > std::string().capacity())
                bytes += root.capacity() + 1;
        if (outputKey.capacity() > std::string().capacity())
            bytes += outputKey.capacity() + 1;
        if (output.native().capacity() > std::filesystem::path::string_type().capacity())
            bytes += (output.native().capacity() + 1) * sizeof(std::filesystem::path::value_type);
        return bytes;
    }

    bool Write(const char* data, size_t size)
    {
        if (!file || size > MaxBytes - stats.bytes)
            return false;
        const auto written = std::fwrite(data, 1, size, file);
        stats.bytes += written;
        if (written != size)
        {
            stats.incomplete = true;
            stopped = true;
            std::fputs("runtimeFileTrace incomplete: evidence write failed\n", stderr);
            return false;
        }
        ++stats.records;
        if (stats.records % 128 == 0 && std::fflush(file) != 0)
        {
            stats.incomplete = true;
            stopped = true;
            std::fputs("runtimeFileTrace incomplete: evidence flush failed\n", stderr);
        }
        return !stopped;
    }

    void Terminal(const char* event, const char* reason)
    {
        if (terminalWritten || !file)
            return;
        terminalWritten = true;
        std::array<char, TerminalReserve> line{};
        const int size = std::snprintf(line.data(), line.size(),
            "{\"event\":\"%s\",\"reason\":\"%s\",\"coverage\":\"observedAPIOnly\","
            "\"osKernelTrace\":false,\"incomplete\":%s,\"recordsBeforeTerminal\":%llu,"
            "\"bytesBeforeTerminal\":%llu,\"fixedMetadataBytes\":%zu,\"peakRecordBytes\":%zu,"
            "\"knownStringHeapCapacityBytes\":%zu,\"costQuoteCpuBytes\":%llu,"
            "\"traceCpuNanoseconds\":%llu,\"peakCallNanoseconds\":%llu}\n",
            event, reason, stats.incomplete ? "true" : "false",
            static_cast<unsigned long long>(stats.records),
            static_cast<unsigned long long>(stats.bytes), stats.fixedMetadataBytes, stats.peakRecordBytes,
            KnownStringHeapCapacityBytes(),
            static_cast<unsigned long long>(CostQuote().cpuBytes),
            static_cast<unsigned long long>(stats.traceCpuNanoseconds),
            static_cast<unsigned long long>(stats.peakCallNanoseconds));
        if (size > 0 && size_t(size) < line.size())
            Write(line.data(), size_t(size));
        if (std::fflush(file) != 0)
        {
            stats.incomplete = true;
            std::fputs("runtimeFileTrace incomplete: terminal flush failed\n", stderr);
        }
    }

    void Incomplete(const char* reason)
    {
        stats.incomplete = true;
        Terminal("traceIncomplete", reason);
        stopped = true;
        std::fputs("runtimeFileTrace incomplete: see evidence terminal/status\n", stderr);
    }

    bool Open()
    {
        if (attempted)
            return file != nullptr;
        attempted = true;
        if (roots[size_t(RootScope::Package)].empty() && roots[size_t(RootScope::Project)].empty())
        {
            Incomplete("packageOrProjectRootRequired");
            return false;
        }
        const auto parent = std::filesystem::weakly_canonical(output.parent_path());
        if (parent.native().size() > PathLimit || !BoundedComponents(parent.native()))
            throw std::runtime_error("trace evidence parent exceeds path metadata limit");
        output = parent / output.filename();
        outputKey = PathKey(output);
        if (Within(outputKey, roots[size_t(RootScope::Package)]) ||
            Within(outputKey, roots[size_t(RootScope::Project)]))
        {
            Incomplete("outputInsidePackageOrProject");
            return false;
        }
#if defined(_WIN32)
        file = _wfopen(output.c_str(), L"wbx");
#else
        const int descriptor = open(output.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0600);
        if (descriptor >= 0)
        {
            file = fdopen(descriptor, "wb");
            if (!file)
                close(descriptor);
        }
#endif
        if (!file)
        {
            Incomplete("exclusiveEvidenceOpenFailed");
            return false;
        }
        std::setvbuf(file, fileBuffer.data(), _IOFBF, fileBuffer.size());
        constexpr char header[] =
            "{\"event\":\"traceStart\",\"coverage\":\"observedAPIOnly\",\"osKernelTrace\":false,"
            "\"observedAPIs\":[\"FileSystem.ResolveRuntimePath\",\"FileSystem.ReadJsonFile\","
            "\"RootedFileReader\",\"CommonFunction.Path\",\"CommonFunction.ReadFile\","
            "\"CommonFunction.Config\",\"TracingIfstream\",\"stb_image.fopen\","
            "\"Assimp.DefaultIOSystem.Open\"],\"maxRecords\":100000,\"maxBytes\":16777216,"
            "\"streamActualPathAvailable\":false,\"rootClassification\":\"registeredLongestPrefix\","
            "\"readHooksOnly\":true,\"flushEveryRecords\":128,\"pathLimit\":2048,"
            "\"costMetric\":\"callingThreadCPU; peakCallWallIncludesMutexWait\","
            "\"pendingHooks\":[\"uiOn.RmlUiFonts\",\"otherSceneReaders\","
            "\"editorOfflineReaders\"],\"closureValidationRequired\":true}\n";
        return Write(header, sizeof(header) - 1);
    }
};

State& GetState()
{
    static State state;
    return state;
}

class Line
{
public:
    explicit Line(State& state) : state(state) {}
    void Text(const char* text)
    {
        const auto count = std::strlen(text);
        if (count > state.record.size() - used)
            throw std::runtime_error("trace record overflow");
        std::memcpy(state.record.data() + used, text, count);
        used += count;
    }
    void String(const std::string& text)
    {
        Text("\"");
        for (unsigned char character : text)
        {
            if (character == '"' || character == '\\')
            {
                char escaped[] = {'\\', char(character), '\0'};
                Text(escaped);
            }
            else if (character < 32)
            {
                char escaped[7]{};
                std::snprintf(escaped, sizeof(escaped), "\\u%04x", character);
                Text(escaped);
            }
            else
            {
                char plain[] = {char(character), '\0'};
                Text(plain);
            }
        }
        Text("\"");
    }
    void Number(uint64_t value)
    {
        char number[32]{};
        std::snprintf(number, sizeof(number), "%llu", static_cast<unsigned long long>(value));
        Text(number);
    }
    size_t Size() const { return used; }
private:
    State& state;
    size_t used = 0;
};
}

bool IsEnabled() noexcept
{
    return GetState().stats.enabled;
}

Cost CostQuote() noexcept
{
    const uint64_t fixed = sizeof(State) + 2 * sizeof(std::atomic<uint32_t>);
    constexpr uint64_t workspace = 2 * 1024 * 1024;
    return {fixed + workspace, 0, fixed, workspace};
}

void SetRoot(RootScope scope, const std::filesystem::path& root) noexcept
{
    if (!IsEnabled())
        return;
    auto& state = GetState();
    std::lock_guard<std::mutex> lock(state.mutex);
    try
    {
        if (scope == RootScope::Unknown || root.empty() || state.stopped)
            return;
        if (root.native().size() > PathLimit)
            throw std::runtime_error("trace root overflow");
        const auto key = PathKey(root);
        if (key.size() > PathLimit)
            throw std::runtime_error("trace root overflow");
        if (state.roots[size_t(scope)] == key)
            return;
        if (state.file && (scope == RootScope::Package || scope == RootScope::Project) &&
            Within(state.outputKey, key))
        {
            state.Incomplete("lateRootContainsEvidence");
            return;
        }
        state.roots[size_t(scope)] = key;
    }
    catch (...)
    {
        state.Incomplete("rootRegistrationFailed");
    }
}

RootScope Classify(const std::filesystem::path& absolutePath) noexcept
{
    if (!IsEnabled())
        return RootScope::Unknown;
    auto& state = GetState();
    std::lock_guard<std::mutex> lock(state.mutex);
    try
    {
        if (absolutePath.native().size() <= PathLimit)
            return state.ClassifyKey(PathKey(absolutePath));
    }
    catch (...) {}
    return RootScope::Unknown;
}

void Record(Event event, const std::filesystem::path& requestedPath, bool success,
    const char* owner, const std::filesystem::path* finalHandlePath,
    std::optional<uint64_t> generation, Access access, std::optional<int64_t> errorCode,
    const char* errorDomain) noexcept
{
    ErrorState preserveErrors;
    if (!IsEnabled())
        return;
    auto& state = GetState();
    if (state.stopped.load(std::memory_order_relaxed))
        return;
    const auto start = std::chrono::steady_clock::now();
    const auto cpu = ThreadCpuNanoseconds();
    std::lock_guard<std::mutex> lock(state.mutex);
    if (state.stopped)
        return;
    try
    {
        if (requestedPath.native().size() > PathLimit ||
            (finalHandlePath && finalHandlePath->native().size() > PathLimit) ||
            (owner && std::strlen(owner) > 256) ||
            (errorDomain && std::strlen(errorDomain) > 64))
        {
            state.Open();
            state.Incomplete("pathOrOwnerLimit");
            return;
        }
        if (!state.Open())
            return;
        const auto requested = Absolute(requestedPath).generic_u8string();
        const auto actual = finalHandlePath ? Absolute(*finalHandlePath).generic_u8string() : std::string();
        if (requested.size() > PathLimit || actual.size() > PathLimit)
            throw std::runtime_error("trace UTF-8 event path exceeds limit");
        const auto key = PathKey(finalHandlePath ? *finalHandlePath : requestedPath);
        if (key == state.outputKey)
            return;
        Line line(state);
        line.Text("{\"event\":"); line.String(EventNames[size_t(event)]);
        line.Text(",\"operation\":"); line.String(access == Access::Read ? "read" : "write");
        line.Text(",\"success\":"); line.Text(success ? "true" : "false");
        line.Text(",\"requestedAbsolutePath\":"); line.String(requested);
        line.Text(",\"actualAbsolutePath\":");
        if (finalHandlePath && event != Event::ResolveResult) line.String(actual); else line.Text("null");
        line.Text(",\"resolvedAbsolutePath\":");
        if (finalHandlePath && event == Event::ResolveResult) line.String(actual); else line.Text("null");
        line.Text(",\"pathBasis\":");
        line.String(finalHandlePath ?
            (event == Event::ResolveResult ? "resolvedCandidate" : "finalHandle") : "requestedOnly");
        const auto scope = state.ClassifyKey(key);
        line.Text(",\"rootScope\":"); line.String(ScopeNames[size_t(scope)]);
        line.Text(",\"rootAbsolutePath\":");
        if (scope != RootScope::Unknown) line.String(state.roots[size_t(scope)]); else line.Text("null");
        line.Text(",\"owner\":"); if (owner) line.String(owner); else line.Text("null");
        line.Text(",\"generation\":"); if (generation) line.Number(*generation); else line.Text("null");
        line.Text(",\"errorCode\":");
        if (errorCode)
        {
            if (*errorCode < 0)
            {
                line.Text("-");
                line.Number(uint64_t(-(*errorCode + 1)) + 1);
            }
            else
                line.Number(uint64_t(*errorCode));
        }
        else line.Text("null");
        line.Text(",\"errorDomain\":");
        if (errorCode && errorDomain) line.String(errorDomain); else line.Text("null");
        line.Text(",\"thread\":");
#if defined(_WIN32)
        line.Number(GetCurrentThreadId());
#else
        line.Number(std::hash<std::thread::id>{}(std::this_thread::get_id()));
#endif
        line.Text(",\"unixTimeNanoseconds\":");
        line.Number(uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::system_clock::now().time_since_epoch()).count()));
        line.Text("}\n");
        if (state.stats.records >= MaxRecords - 1 ||
            line.Size() > MaxBytes - TerminalReserve - state.stats.bytes)
        {
            state.Incomplete("recordOrByteHardCap");
            return;
        }
        state.stats.peakRecordBytes = std::max(state.stats.peakRecordBytes, line.Size());
        state.Write(state.record.data(), line.Size());
    }
    catch (...)
    {
        state.Incomplete("recordSerializationOrInitializationFailed");
    }
    const auto cpuEnd = ThreadCpuNanoseconds();
    if (cpuEnd >= cpu)
        state.stats.traceCpuNanoseconds += cpuEnd - cpu;
    state.stats.peakCallNanoseconds = std::max(state.stats.peakCallNanoseconds,
        uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::steady_clock::now() - start).count()));
}

void Flush() noexcept
{
    if (!IsEnabled())
        return;
    auto& state = GetState();
    std::lock_guard<std::mutex> lock(state.mutex);
    if (state.file && std::fflush(state.file) != 0)
        state.Incomplete("explicitFlushFailed");
}

Statistics GetStatistics() noexcept
{
    auto& state = GetState();
    std::lock_guard<std::mutex> lock(state.mutex);
    auto stats = state.stats;
    stats.knownStringHeapCapacityBytes = state.KnownStringHeapCapacityBytes();
    return stats;
}

void MarkIncomplete() noexcept
{
    ErrorState preserveErrors;
    if (!IsEnabled())
        return;
    auto& state = GetState();
    std::lock_guard<std::mutex> lock(state.mutex);
    state.Incomplete("readerHookObservationFailed");
}
}
}
