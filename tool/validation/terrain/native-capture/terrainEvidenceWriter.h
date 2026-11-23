#pragma once
#include <windows.h>
#include <algorithm>
#include <filesystem>
#include <functional>
#include <ostream>
#include <stdexcept>
#include <streambuf>

namespace VL
{
struct TerrainEvidenceIo
{
    decltype(&WriteFile) write = &WriteFile;
    decltype(&FlushFileBuffers) flush = &FlushFileBuffers;
    decltype(&CloseHandle) close = &CloseHandle;
};

class TerrainEvidenceBuffer final : public std::streambuf
{
    HANDLE handle = INVALID_HANDLE_VALUE;
    TerrainEvidenceIo operations;
protected:
    std::streamsize xsputn(const char* data, std::streamsize count) override
    {
        std::streamsize total = 0;
        while (total < count)
        {
            const auto requested = static_cast<DWORD>((std::min)(
                count-total, static_cast<std::streamsize>(0x40000000)));
            DWORD written = 0;
            if (!operations.write(handle, data+total, requested, &written, nullptr) ||
                written == 0 || written > requested) break;
            total += written;
        }
        return total;
    }
    int_type overflow(int_type value) override
    {
        if (traits_type::eq_int_type(value, traits_type::eof()))
            return traits_type::not_eof(value);
        const char character = traits_type::to_char_type(value);
        return xsputn(&character, 1) == 1 ? value : traits_type::eof();
    }
    int sync() override { return operations.flush(handle) ? 0 : -1; }
public:
    explicit TerrainEvidenceBuffer(const std::filesystem::path& path, TerrainEvidenceIo io)
        : operations(io)
    {
        handle = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW,
            FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
        if (handle == INVALID_HANDLE_VALUE)
            throw std::runtime_error("Evidence exclusive open failed: "+path.u8string()+
                " Win32="+std::to_string(GetLastError()));
    }
    ~TerrainEvidenceBuffer() override
    {
        if (handle != INVALID_HANDLE_VALUE) CloseHandle(handle);
    }
    void Close()
    {
        if (!operations.close(handle))
            throw std::runtime_error("Evidence close failed Win32="+std::to_string(GetLastError()));
        handle = INVALID_HANDLE_VALUE;
    }
};

inline void WriteTerrainEvidenceFile(const std::filesystem::path& path,
    const std::function<void(std::ostream&)>& write, TerrainEvidenceIo operations = {})
{
    TerrainEvidenceBuffer buffer(path, operations);
    std::ostream stream(&buffer);
    stream.exceptions(std::ios::badbit | std::ios::failbit);
    try
    {
        write(stream);
        if (!stream) throw std::runtime_error("Evidence write failed");
        stream.flush();
        if (!stream) throw std::runtime_error("Evidence flush failed");
        buffer.Close();
    }
    catch (const std::exception& error)
    {
        throw std::runtime_error("Evidence write/flush/close failed: "+path.u8string()+": "+error.what());
    }
}
}
