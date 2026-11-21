#include "core/tracingIfstream.h"

#include <cerrno>
#include <atomic>
#include <cstdint>
#include <string>

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <io.h>
#endif

namespace VL::RuntimeFileTrace
{
namespace
{
std::atomic<uint32_t> observers{0};
std::atomic<uint32_t> peakObservers{0};

class ObservationSlot
{
public:
    ObservationSlot()
    {
        auto count = observers.load(std::memory_order_relaxed);
        while (count < MaxReaderObservers)
        {
            if (observers.compare_exchange_weak(count, count + 1, std::memory_order_relaxed))
            {
                acquired = true;
                auto peak = peakObservers.load(std::memory_order_relaxed);
                while (peak < count + 1 && !peakObservers.compare_exchange_weak(
                    peak, count + 1, std::memory_order_relaxed)) {}
                return;
            }
        }
        MarkIncomplete();
    }
    ~ObservationSlot()
    {
        if (acquired)
            observers.fetch_sub(1, std::memory_order_relaxed);
    }
    bool Acquired() const { return acquired; }
private:
    bool acquired = false;
};

bool BoundedName(const char* path)
{
    size_t characters = 0, separators = 0;
    for (; path[characters] != '\0' && characters <= MaxPathCharacters; ++characters)
        if (path[characters] == '/' || path[characters] == '\\')
            ++separators;
    return characters <= MaxPathCharacters && separators <= MaxPathComponents;
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
}

void ObserveStreamOpen(const std::filesystem::path& path, bool opened,
    const char* owner, int previousErrno) noexcept
{
    ErrorState preserveErrors;
    const auto error = !opened && errno != 0 && errno != previousErrno ?
        std::optional<int64_t>(errno) : std::nullopt;
    Record(Event::StreamReadOpen, path, opened, owner, nullptr,
        std::nullopt, Access::Read, error, error ? "errno" : nullptr);
}

void ObserveStreamOpen(const char* path, bool opened,
    const char* owner, int previousErrno) noexcept
{
    ErrorState preserveErrors;
    if (!IsEnabled())
        return;
    ObservationSlot slot;
    if (!slot.Acquired())
        return;
    try
    {
        if (!BoundedName(path))
        {
            MarkIncomplete();
            return;
        }
        ObserveStreamOpen(std::filesystem::path(path), opened, owner, previousErrno);
    }
    catch (...)
    {
        MarkIncomplete();
    }
}

void ObserveStreamOpen(const std::string& path, bool opened,
    const char* owner, int previousErrno) noexcept
{
    ObserveStreamOpen(path.c_str(), opened, owner, previousErrno);
}

void ObserveCFileOpen(const char* path, FILE* file, const char* owner) noexcept
{
    ErrorState preserveErrors;
    if (!IsEnabled())
        return;
    ObservationSlot slot;
    if (!slot.Acquired())
        return;
    try
    {
        if (!BoundedName(path))
        {
            MarkIncomplete();
            return;
        }
        const auto error = !file && errno != 0 ?
            std::optional<int64_t>(errno) : std::nullopt;
        const std::filesystem::path requested(path);
#if defined(_WIN32)
        if (file)
        {
            const auto native = _get_osfhandle(_fileno(file));
            if (native != -1)
            {
                const auto handle = reinterpret_cast<HANDLE>(native);
                const DWORD size = GetFinalPathNameByHandleW(handle, nullptr, 0,
                    FILE_NAME_NORMALIZED | VOLUME_NAME_DOS);
                if (size > 0 && size <= 2048)
                {
                    std::wstring name(size, L'\0');
                    const DWORD written = GetFinalPathNameByHandleW(handle,
                        name.data(), size, FILE_NAME_NORMALIZED | VOLUME_NAME_DOS);
                    if (written > 0 && written < size)
                    {
                        name.resize(written);
                        const std::filesystem::path actual(name);
                        Record(Event::NativeReadOpen, requested, true, owner, &actual);
                        return;
                    }
                }
                else if (size > 2048)
                {
                    MarkIncomplete();
                    return;
                }
            }
        }
#endif
        Record(Event::StreamReadOpen, requested, file != nullptr, owner, nullptr,
            std::nullopt, Access::Read, error, error ? "errno" : nullptr);
    }
    catch (...)
    {
        MarkIncomplete();
    }
}

FILE* OpenCFile(const char* path, const char* mode)
{
    FILE* file = std::fopen(path, mode);
    ObserveCFileOpen(path, file, "stb_image.fopen");
    return file;
}

bool TraceExists(const std::filesystem::path& path, const char* owner)
{
    bool found;
    try
    {
        found = std::filesystem::exists(path);
    }
    catch (const std::filesystem::filesystem_error& error)
    {
        Record(Event::Resolve, path, false, owner, nullptr, std::nullopt,
            Access::Read, error.code().value(), error.code().category().name());
        throw;
    }
    catch (...)
    {
        Record(Event::Resolve, path, false, owner);
        throw;
    }
    Record(Event::Resolve, path, found, owner);
    return found;
}

uint32_t PeakReaderObservers() noexcept
{
    return peakObservers.load(std::memory_order_relaxed);
}
}
