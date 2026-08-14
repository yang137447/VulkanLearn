#include "shader/build/atomicFile.h"

#include <atomic>
#include <fstream>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#else
#include <unistd.h>
#endif

namespace VL
{
namespace
{

std::atomic<uint64_t> TemporaryFileCounter{0};

uint64_t GetProcessIdentity()
{
#if defined(_WIN32)
    return static_cast<uint64_t>(GetCurrentProcessId());
#else
    return static_cast<uint64_t>(getpid());
#endif
}

std::filesystem::path BuildTemporaryPath(const std::filesystem::path& target)
{
    const uint64_t sequence = TemporaryFileCounter.fetch_add(1);
    return target.string() + ".tmp." +
        std::to_string(GetProcessIdentity()) + "." +
        std::to_string(sequence);
}

void ReplaceFile(const std::filesystem::path& temporaryPath, const std::filesystem::path& targetPath)
{
#if defined(_WIN32)
    if (!MoveFileExW(
            temporaryPath.c_str(),
            targetPath.c_str(),
            MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH))
    {
        const DWORD error = GetLastError();
        std::error_code ignoredError;
        std::filesystem::remove(temporaryPath, ignoredError);
        throw std::runtime_error(
            "Atomic file replacement failed for '" + targetPath.string() +
            "' with Windows error " + std::to_string(error));
    }
#else
    std::error_code renameError;
    std::filesystem::rename(temporaryPath, targetPath, renameError);
    if (renameError)
    {
        std::error_code ignoredError;
        std::filesystem::remove(temporaryPath, ignoredError);
        throw std::runtime_error(
            "Atomic file replacement failed for '" + targetPath.string() +
            "': " + renameError.message());
    }
#endif
}

} // namespace

std::vector<uint8_t> ReadBinaryFile(const std::filesystem::path& path)
{
    std::ifstream input(path, std::ios::binary | std::ios::ate);
    if (!input.is_open())
    {
        throw std::runtime_error("Failed to open file: " + path.string());
    }

    const std::streamoff endPosition = input.tellg();
    if (endPosition < 0)
    {
        throw std::runtime_error("Failed to measure file: " + path.string());
    }
    input.seekg(0, std::ios::beg);

    std::vector<uint8_t> bytes(static_cast<size_t>(endPosition));
    if (!bytes.empty())
    {
        input.read(
            reinterpret_cast<char*>(bytes.data()),
            static_cast<std::streamsize>(bytes.size()));
        if (!input)
        {
            throw std::runtime_error("Failed to read file: " + path.string());
        }
    }
    return bytes;
}

void WriteBinaryFileAtomically(
    const std::filesystem::path& path,
    const void* data,
    size_t size)
{
    if (size > 0 && data == nullptr)
    {
        throw std::runtime_error("Cannot write a null byte range");
    }

    if (path.has_parent_path())
    {
        std::filesystem::create_directories(path.parent_path());
    }

    const std::filesystem::path temporaryPath = BuildTemporaryPath(path);
    {
        std::ofstream output(temporaryPath, std::ios::binary | std::ios::trunc);
        if (!output.is_open())
        {
            throw std::runtime_error(
                "Failed to open temporary output file: " + temporaryPath.string());
        }
        if (size > 0)
        {
            output.write(
                static_cast<const char*>(data),
                static_cast<std::streamsize>(size));
        }
        output.flush();
        if (!output)
        {
            output.close();
            std::error_code ignoredError;
            std::filesystem::remove(temporaryPath, ignoredError);
            throw std::runtime_error(
                "Failed to write temporary output file: " + temporaryPath.string());
        }
    }

    ReplaceFile(temporaryPath, path);
}

void WriteBinaryFileAtomically(
    const std::filesystem::path& path,
    const std::vector<uint8_t>& data)
{
    WriteBinaryFileAtomically(path, data.data(), data.size());
}

void WriteTextFileAtomically(
    const std::filesystem::path& path,
    std::string_view text)
{
    WriteBinaryFileAtomically(path, text.data(), text.size());
}

void WriteFileBatchAtomically(const std::vector<AtomicFileWrite>& writes)
{
    struct StagedWrite
    {
        std::filesystem::path targetPath;
        std::filesystem::path temporaryPath;
    };

    std::vector<StagedWrite> stagedWrites;
    stagedWrites.reserve(writes.size());
    try
    {
        for (const AtomicFileWrite& write : writes)
        {
            if (write.path.has_parent_path())
            {
                std::filesystem::create_directories(write.path.parent_path());
            }

            StagedWrite stagedWrite;
            stagedWrite.targetPath = write.path;
            stagedWrite.temporaryPath = BuildTemporaryPath(write.path);

            std::ofstream output(
                stagedWrite.temporaryPath,
                std::ios::binary | std::ios::trunc);
            if (!output.is_open())
            {
                throw std::runtime_error(
                    "Failed to open temporary output file: " +
                    stagedWrite.temporaryPath.string());
            }
            if (!write.bytes.empty())
            {
                output.write(
                    reinterpret_cast<const char*>(write.bytes.data()),
                    static_cast<std::streamsize>(write.bytes.size()));
            }
            output.flush();
            if (!output)
            {
                throw std::runtime_error(
                    "Failed to write temporary output file: " +
                    stagedWrite.temporaryPath.string());
            }
            output.close();
            stagedWrites.push_back(std::move(stagedWrite));
        }

        for (const StagedWrite& stagedWrite : stagedWrites)
        {
            ReplaceFile(stagedWrite.temporaryPath, stagedWrite.targetPath);
        }
    }
    catch (...)
    {
        for (const StagedWrite& stagedWrite : stagedWrites)
        {
            std::error_code ignoredError;
            std::filesystem::remove(stagedWrite.temporaryPath, ignoredError);
        }
        throw;
    }
}

bool WriteTextFileIfChangedAtomically(
    const std::filesystem::path& path,
    std::string_view text)
{
    if (std::filesystem::is_regular_file(path))
    {
        const std::vector<uint8_t> currentBytes = ReadBinaryFile(path);
        if (currentBytes.size() == text.size() &&
            std::equal(currentBytes.begin(), currentBytes.end(), text.begin()))
        {
            return false;
        }
    }

    WriteTextFileAtomically(path, text);
    return true;
}

} // namespace VL
