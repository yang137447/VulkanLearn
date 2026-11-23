#pragma once

#include <cerrno>
#include <cstdio>
#include <filesystem>
#include <fstream>

#include "core/runtimeFileTrace.h"

namespace VL::RuntimeFileTrace
{
void ObserveStreamOpen(const std::filesystem::path& path, bool opened,
    const char* owner, int previousErrno) noexcept;
void ObserveStreamOpen(const char* path, bool opened,
    const char* owner, int previousErrno) noexcept;
void ObserveStreamOpen(const std::string& path, bool opened,
    const char* owner, int previousErrno) noexcept;
void ObserveCFileOpen(const char* path, FILE* file, const char* owner) noexcept;
FILE* OpenCFile(const char* path, const char* mode);
bool TraceExists(const std::filesystem::path& path, const char* owner);
uint32_t PeakReaderObservers() noexcept;

class TracingIfstream : public std::ifstream
{
public:
    template<typename Path>
    explicit TracingIfstream(const Path& path,
        std::ios_base::openmode mode = std::ios_base::in,
        const char* owner = "TracingIfstream")
        : TracingIfstream(path, mode, owner, errno)
    {}

private:
    template<typename Path>
    TracingIfstream(const Path& path, std::ios_base::openmode mode,
        const char* owner, int previousErrno)
        : std::ifstream(path, mode)
    {
        ObserveStreamOpen(path, is_open(), owner, previousErrno);
    }
};
}
