#include "shader/build/atomicFile.h"
#include "core/runtimeFileTrace.h"

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <cstring>
#include <fstream>
#include <functional>
#include <memory>
#include <limits>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#else
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace VL
{
#if defined(VL_FILE_IO_TEST_HOOKS)
namespace AtomicFileTesting
{
thread_local std::function<void(const std::filesystem::path&)> BeforeCommit;
thread_local std::function<void(size_t)> AfterWrite;
}
#endif
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
    return target.parent_path() / (".vl-atomic.tmp." +
        std::to_string(GetProcessIdentity()) + "." +
        std::to_string(sequence));
}

[[noreturn]] void FileError(const std::string& operation)
{
#if defined(_WIN32)
    const auto error = GetLastError();
    throw std::runtime_error(operation + " with Windows error " + std::to_string(error));
#else
    const auto error = errno;
    throw std::runtime_error(operation + ": " + std::error_code(error, std::generic_category()).message());
#endif
}

#if defined(_WIN32)
using FileHandle = HANDLE;
const FileHandle InvalidFileHandle = INVALID_HANDLE_VALUE;
#else
using FileHandle = int;
constexpr FileHandle InvalidFileHandle = -1;

bool SameFileTimes(const struct stat& first, const struct stat& second)
{
#if defined(__APPLE__)
    return first.st_mtimespec.tv_sec == second.st_mtimespec.tv_sec &&
        first.st_mtimespec.tv_nsec == second.st_mtimespec.tv_nsec &&
        first.st_ctimespec.tv_sec == second.st_ctimespec.tv_sec &&
        first.st_ctimespec.tv_nsec == second.st_ctimespec.tv_nsec;
#else
    return first.st_mtim.tv_sec == second.st_mtim.tv_sec &&
        first.st_mtim.tv_nsec == second.st_mtim.tv_nsec &&
        first.st_ctim.tv_sec == second.st_ctim.tv_sec &&
        first.st_ctim.tv_nsec == second.st_ctim.tv_nsec;
#endif
}
#endif

class HeldHandle
{
public:
    explicit HeldHandle(FileHandle value) : value(value) {}
    HeldHandle(HeldHandle&& other) noexcept : value(std::exchange(other.value, InvalidFileHandle)) {}
    HeldHandle(const HeldHandle&) = delete;
    HeldHandle& operator=(const HeldHandle&) = delete;
    ~HeldHandle()
    {
        if (value != InvalidFileHandle)
        {
#if defined(_WIN32)
            CloseHandle(value);
#else
            close(value);
#endif
        }
    }
    FileHandle Get() const { return value; }
private:
    FileHandle value;
};

#if defined(_WIN32)
std::filesystem::path FinalHandlePath(HANDLE handle)
{
    const DWORD size = GetFinalPathNameByHandleW(handle, nullptr, 0, FILE_NAME_NORMALIZED | VOLUME_NAME_DOS);
    if (size == 0)
        FileError("Failed to measure final handle path");
    std::wstring path(size, L'\0');
    const DWORD written = GetFinalPathNameByHandleW(handle, path.data(), size,
        FILE_NAME_NORMALIZED | VOLUME_NAME_DOS);
    if (written == 0 || written >= size)
        FileError("Failed to query final handle path");
    path.resize(written);
    // MinGW 的 lexically_normal 会折叠 \\?\ 前缀，原生规范路径必须原样保留。
    return std::filesystem::path(path);
}

bool SameWindowsPath(const std::filesystem::path& first, const std::filesystem::path& second)
{
    auto firstName = first.native();
    auto secondName = second.native();
    while (!firstName.empty() && firstName.back() == L'\\')
        firstName.pop_back();
    while (!secondName.empty() && secondName.back() == L'\\')
        secondName.pop_back();
    return CompareStringOrdinal(firstName.c_str(), -1, secondName.c_str(), -1, TRUE) == CSTR_EQUAL;
}
#endif

std::filesystem::path AbsoluteFilePath(const std::filesystem::path& requested)
{
#if defined(_WIN32)
    auto name = requested.native();
    if (name.compare(0, 8, L"\\\\?\\UNC\\") == 0)
        name = L"\\\\" + name.substr(8);
    else if (name.compare(0, 4, L"\\\\?\\") == 0)
        name = name.substr(4);
    const DWORD size = GetFullPathNameW(name.c_str(), 0, nullptr, nullptr);
    if (size == 0)
        FileError("Failed to measure absolute file path");
    std::wstring absolute(size, L'\0');
    const DWORD written = GetFullPathNameW(name.c_str(), size, absolute.data(), nullptr);
    if (written == 0 || written >= size)
        FileError("Failed to resolve absolute file path");
    absolute.resize(written);
    return std::filesystem::path(absolute);
#else
    return std::filesystem::absolute(requested).lexically_normal();
#endif
}

class PinnedDirectory
{
public:
    explicit PinnedDirectory(const std::filesystem::path& requested)
    {
        const auto absolute = AbsoluteFilePath(requested);
        auto current = absolute.root_path();
#if defined(_WIN32)
        auto remaining = absolute.relative_path();
        const auto& native = absolute.native();
        if (native.compare(0, 2, L"\\\\") == 0)
        {
            const auto serverEnd = native.find(L'\\', 2);
            const auto shareEnd = serverEnd == std::wstring::npos ?
                std::wstring::npos : native.find(L'\\', serverEnd + 1);
            if (serverEnd == std::wstring::npos)
                throw std::runtime_error("UNC input must include a share");
            const auto rootEnd = shareEnd == std::wstring::npos ? native.size() : shareEnd;
            current = std::filesystem::path(L"\\\\?\\UNC\\" + native.substr(2, rootEnd - 2) + L"\\");
            remaining = shareEnd == std::wstring::npos ?
                std::filesystem::path() : std::filesystem::path(native.substr(shareEnd + 1));
        }
        else
            current = std::filesystem::path(L"\\\\?\\" + current.native());
        const auto retain = [&](const std::filesystem::path& path) {
            HeldHandle opened(CreateFileW(path.c_str(), FILE_READ_ATTRIBUTES | FILE_LIST_DIRECTORY,
                FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING,
                FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT, nullptr));
            if (opened.Get() == InvalidFileHandle)
                FileError("Failed to pin directory " + path.string());
            BY_HANDLE_FILE_INFORMATION info{};
            if (!GetFileInformationByHandle(opened.Get(), &info) ||
                !(info.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) ||
                (info.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT))
                throw std::runtime_error("Linked/non-directory ancestor rejected: " + path.string());
            const auto actual = FinalHandlePath(opened.Get());
            if (!handles.empty() && !SameWindowsPath(actual.parent_path(), this->path))
                throw std::runtime_error("Directory handle escaped pinned ancestor: " +
                    actual.parent_path().string() + " != " + this->path.string());
            handles.push_back(std::move(opened));
            paths.push_back(actual);
            this->path = actual;
        };
        retain(current);
        for (const auto& component : remaining)
        {
            current /= component;
            retain(current);
        }
#else
        HeldHandle base(open(current.c_str(), O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC));
        if (base.Get() < 0)
            FileError("Failed to open root directory");
        handles.push_back(std::move(base));
        for (const auto& component : absolute.relative_path())
        {
            HeldHandle opened(openat(Get(), component.c_str(),
                O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC));
            if (opened.Get() < 0)
                FileError("Failed to open no-follow directory " + component.string());
            handles.push_back(std::move(opened));
        }
        path = absolute;
#endif
    }
    FileHandle Get() const { return handles.back().Get(); }
    const std::filesystem::path& Path() const { return path; }
    void Validate() const
    {
#if defined(_WIN32)
        for (size_t index = 0; index < handles.size(); ++index)
        {
            BY_HANDLE_FILE_INFORMATION info{};
            if (!GetFileInformationByHandle(handles[index].Get(), &info) ||
                !(info.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) ||
                (info.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) ||
                !SameWindowsPath(FinalHandlePath(handles[index].Get()), paths[index]))
                throw std::runtime_error("Pinned directory identity/path changed");
        }
#endif
    }
private:
    // 不共享 DELETE 防止祖先被换名；逐层核对最终路径，拒绝中途的 reparse 重定向。
    std::vector<HeldHandle> handles;
    std::filesystem::path path;
#if defined(_WIN32)
    std::vector<std::filesystem::path> paths;
#endif
};

class TemporaryFile
{
public:
    explicit TemporaryFile(const std::filesystem::path& target)
        : parent(target.has_parent_path() ? target.parent_path() : std::filesystem::path(".")),
          path(parent.Path() / BuildTemporaryPath(target).filename())
    {
        ValidateTarget(target);
#if defined(_WIN32)
        handle = CreateFileW(path.c_str(), GENERIC_WRITE | DELETE, 0, nullptr,
            CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (handle == INVALID_HANDLE_VALUE)
        {
            const DWORD error = GetLastError();
            throw std::runtime_error("Failed to create exclusive temporary output file: " +
                path.string() + " with Windows error " + std::to_string(error));
        }
#else
        descriptor = openat(parent.Get(), path.filename().c_str(),
            O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0666);
        if (descriptor < 0)
        {
            const int error = errno;
            throw std::runtime_error("Failed to create exclusive temporary output file: " +
                path.string() + ": " + std::error_code(error, std::generic_category()).message());
        }
#endif
        // 只有独占创建成功才拥有路径；碰撞时不得截断、跟随链接或清理已有文件。
        owned = true;
    }

    TemporaryFile(const TemporaryFile&) = delete;
    TemporaryFile& operator=(const TemporaryFile&) = delete;

    ~TemporaryFile()
    {
        if (owned)
        {
#if defined(_WIN32)
            FILE_DISPOSITION_INFO disposition{TRUE};
            SetFileInformationByHandle(handle, FileDispositionInfo, &disposition, sizeof(disposition));
#else
            if (OwnsEntry())
                unlinkat(parent.Get(), path.filename().c_str(), 0);
#endif
        }
        Close();
    }

    void Write(const void* data, size_t size)
    {
        const auto* bytes = static_cast<const uint8_t*>(data);
        size_t offset = 0;
        while (offset < size)
        {
            const size_t chunk = std::min(size - offset, size_t{1024 * 1024});
#if defined(_WIN32)
            DWORD written = 0;
            if (!WriteFile(handle, bytes + offset, static_cast<DWORD>(chunk), &written, nullptr) ||
                written == 0)
#else
            const ssize_t written = write(descriptor, bytes + offset, chunk);
            if (written < 0 && errno == EINTR)
                continue;
            if (written <= 0)
#endif
            {
                throw std::runtime_error("Failed to write temporary output file: " + path.string());
            }
            offset += static_cast<size_t>(written);
#if defined(VL_FILE_IO_TEST_HOOKS)
            if (AtomicFileTesting::AfterWrite)
                AtomicFileTesting::AfterWrite(offset);
#endif
        }
#if defined(_WIN32)
        const bool flushed = FlushFileBuffers(handle) != 0;
#else
        const bool flushed = fsync(descriptor) == 0;
#endif
        if (!flushed)
        {
            throw std::runtime_error("Failed to flush temporary output file: " + path.string());
        }
    }

    void Commit(const std::filesystem::path& target)
    {
#if defined(VL_FILE_IO_TEST_HOOKS)
        if (AtomicFileTesting::BeforeCommit)
            AtomicFileTesting::BeforeCommit(path);
#endif
        parent.Validate();
        ValidateTarget(target);
#if defined(_WIN32)
        const auto filename = (parent.Path() / target.filename()).native();
        std::vector<uint8_t> buffer(sizeof(FILE_RENAME_INFO) + filename.size() * sizeof(wchar_t));
        auto* rename = reinterpret_cast<FILE_RENAME_INFO*>(buffer.data());
        rename->ReplaceIfExists = targetExists ? TRUE : FALSE;
        // Win32 FileRenameInfo 要求 RootDirectory 为空；父路径由保留的祖先句柄锁定。
        rename->RootDirectory = nullptr;
        rename->FileNameLength = static_cast<DWORD>(filename.size() * sizeof(wchar_t));
        std::memcpy(rename->FileName, filename.data(), rename->FileNameLength);
        if (!SetFileInformationByHandle(handle, FileRenameInfo, rename, static_cast<DWORD>(buffer.size())))
            FileError("Atomic file handle replacement failed for " + target.string());
#else
        if (!OwnsEntry())
            throw std::runtime_error("Atomic temporary entry was replaced");
        if (renameat(parent.Get(), path.filename().c_str(), parent.Get(), target.filename().c_str()) != 0)
            FileError("Atomic directory-relative replacement failed for " + target.string());
#endif
        // 发布后旧临时名可被其他调用复用，析构不得再删除它。
        owned = false;
        if (!Close())
            FileError("Failed to close published file handle");
    }

private:
    void ValidateTarget(const std::filesystem::path& target)
    {
#if defined(_WIN32)
        HeldHandle existing(CreateFileW((parent.Path() / target.filename()).c_str(),
            FILE_READ_ATTRIBUTES, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
            nullptr, OPEN_EXISTING, FILE_FLAG_OPEN_REPARSE_POINT | FILE_FLAG_BACKUP_SEMANTICS, nullptr));
        if (existing.Get() == InvalidFileHandle)
        {
            if (GetLastError() == ERROR_FILE_NOT_FOUND)
            {
                if (targetInspected && targetExists)
                    throw std::runtime_error("Atomic destination disappeared during staging");
                targetInspected = true;
                return;
            }
            FileError("Failed to inspect atomic destination");
        }
        BY_HANDLE_FILE_INFORMATION info{};
        if (GetFileType(existing.Get()) != FILE_TYPE_DISK ||
            !GetFileInformationByHandle(existing.Get(), &info) ||
            (info.dwFileAttributes & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT)))
            throw std::runtime_error("Atomic destination is not a regular disk file");
        if (targetInspected && (!targetExists ||
            info.dwVolumeSerialNumber != targetIdentity.dwVolumeSerialNumber ||
            info.nFileIndexHigh != targetIdentity.nFileIndexHigh ||
            info.nFileIndexLow != targetIdentity.nFileIndexLow ||
            info.nFileSizeHigh != targetIdentity.nFileSizeHigh ||
            info.nFileSizeLow != targetIdentity.nFileSizeLow ||
            CompareFileTime(&info.ftLastWriteTime, &targetIdentity.ftLastWriteTime) != 0))
            throw std::runtime_error("Atomic destination changed during staging");
        targetIdentity = info;
#else
        struct stat info{};
        if (fstatat(parent.Get(), target.filename().c_str(), &info, AT_SYMLINK_NOFOLLOW) != 0)
        {
            if (errno == ENOENT)
            {
                if (targetInspected && targetExists)
                    throw std::runtime_error("Atomic destination disappeared during staging");
                targetInspected = true;
                return;
            }
            FileError("Failed to inspect atomic destination");
        }
        if (!S_ISREG(info.st_mode))
            throw std::runtime_error("Atomic destination is not a regular file");
        if (targetInspected && (!targetExists || info.st_dev != targetIdentity.st_dev ||
            info.st_ino != targetIdentity.st_ino || info.st_size != targetIdentity.st_size ||
            !SameFileTimes(info, targetIdentity)))
            throw std::runtime_error("Atomic destination changed during staging");
        targetIdentity = info;
#endif
        targetExists = true;
        targetInspected = true;
    }

#if !defined(_WIN32)
    bool OwnsEntry() const
    {
        struct stat ownedInfo{}, entryInfo{};
        return fstat(descriptor, &ownedInfo) == 0 &&
            fstatat(parent.Get(), path.filename().c_str(), &entryInfo, AT_SYMLINK_NOFOLLOW) == 0 &&
            ownedInfo.st_dev == entryInfo.st_dev && ownedInfo.st_ino == entryInfo.st_ino;
    }
#endif
    bool Close()
    {
#if defined(_WIN32)
        if (handle == INVALID_HANDLE_VALUE)
            return true;
        const HANDLE closing = std::exchange(handle, INVALID_HANDLE_VALUE);
        return CloseHandle(closing) != 0;
#else
        if (descriptor < 0)
            return true;
        return close(std::exchange(descriptor, -1)) == 0;
#endif
    }

    PinnedDirectory parent;
    std::filesystem::path path;
    bool owned = false;
    bool targetInspected = false;
    bool targetExists = false;
#if defined(_WIN32)
    BY_HANDLE_FILE_INFORMATION targetIdentity{};
    HANDLE handle = INVALID_HANDLE_VALUE;
#else
    struct stat targetIdentity{};
    int descriptor = -1;
#endif
};

} // namespace

struct PinnedFileDirectory::Impl
{
    explicit Impl(const std::filesystem::path& path) : directory(path) {}
    PinnedDirectory directory;
};

PinnedFileDirectory::PinnedFileDirectory(const std::filesystem::path& path)
    : impl(std::make_unique<Impl>(path))
{
}

PinnedFileDirectory::~PinnedFileDirectory() = default;

const std::filesystem::path& PinnedFileDirectory::Path() const
{
    return impl->directory.Path();
}

void PinnedFileDirectory::ValidateIdentity() const
{
    impl->directory.Validate();
}

namespace
{
std::filesystem::path RootedPath(
    const std::filesystem::path& root, const std::filesystem::path& relative)
{
    if (relative.empty() || relative.has_root_path())
        throw std::runtime_error("Rooted input path must be relative");
    for (const auto& component : relative)
        if (component == "." || component == ".." || component.empty())
            throw std::runtime_error("Rooted input path contains traversal");
    return AbsoluteFilePath(root) / relative;
}
}

struct RootedFileReader::Impl
{
    explicit Impl(const std::filesystem::path& requested)
        : parent(requested.parent_path()), filename(requested.filename()),
#if defined(_WIN32)
          handle(CreateFileW((parent.Path() / filename).c_str(), GENERIC_READ,
              FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_FLAG_OPEN_REPARSE_POINT, nullptr))
#else
          handle(openat(parent.Get(), filename.c_str(), O_RDONLY | O_NONBLOCK | O_NOFOLLOW | O_CLOEXEC))
#endif
    {
        if (handle.Get() == InvalidFileHandle)
        {
            const auto savedError =
#if defined(_WIN32)
                GetLastError();
#else
                errno;
#endif
            RuntimeFileTrace::Record(RuntimeFileTrace::Event::NativeReadOpen,
                requested, false, "RootedFileReader", nullptr, std::nullopt,
                RuntimeFileTrace::Access::Read, savedError,
#if defined(_WIN32)
                "win32"
#else
                "errno"
#endif
            );
#if defined(_WIN32)
            SetLastError(savedError);
#else
            errno = savedError;
#endif
            FileError("Failed to open rooted input");
        }
        RuntimeFileTrace::Record(RuntimeFileTrace::Event::NativeReadOpen,
            requested, true, "RootedFileReader");
#if defined(_WIN32)
        if (GetFileType(handle.Get()) != FILE_TYPE_DISK ||
            !GetFileInformationByHandle(handle.Get(), &identity) ||
            (identity.dwFileAttributes & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT)))
            throw std::runtime_error("Rooted input is not a regular no-reparse disk file");
        size = (uint64_t(identity.nFileSizeHigh) << 32) | identity.nFileSizeLow;
        expectedPath = parent.Path() / filename;
#else
        if (fstat(handle.Get(), &identity) != 0 || !S_ISREG(identity.st_mode) || identity.st_size < 0)
            throw std::runtime_error("Rooted input is not a regular file");
        size = static_cast<uint64_t>(identity.st_size);
#endif
        Validate();
    }

    void Validate() const
    {
        parent.Validate();
#if defined(_WIN32)
        BY_HANDLE_FILE_INFORMATION current{};
        if (!GetFileInformationByHandle(handle.Get(), &current) ||
            current.dwVolumeSerialNumber != identity.dwVolumeSerialNumber ||
            current.nFileIndexHigh != identity.nFileIndexHigh ||
            current.nFileIndexLow != identity.nFileIndexLow ||
            current.nFileSizeHigh != identity.nFileSizeHigh ||
            current.nFileSizeLow != identity.nFileSizeLow ||
            CompareFileTime(&current.ftLastWriteTime, &identity.ftLastWriteTime) != 0 ||
            (current.dwFileAttributes & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT)) ||
            !SameWindowsPath(TraceFinalHandlePath(), expectedPath))
            throw std::runtime_error("Rooted input handle identity/path mismatch");
#else
        struct stat current{}, entry{};
        if (fstat(handle.Get(), &current) != 0 ||
            fstatat(parent.Get(), filename.c_str(), &entry, AT_SYMLINK_NOFOLLOW) != 0 ||
            !S_ISREG(current.st_mode) || !S_ISREG(entry.st_mode) ||
            current.st_dev != identity.st_dev || current.st_ino != identity.st_ino ||
            current.st_size != identity.st_size ||
            !SameFileTimes(current, identity) ||
            entry.st_dev != identity.st_dev || entry.st_ino != identity.st_ino)
            throw std::runtime_error("Rooted input descriptor identity mismatch");
#endif
    }

#if defined(_WIN32)
    std::filesystem::path TraceFinalHandlePath() const
    {
        const auto actual = FinalHandlePath(handle.Get());
        if (RuntimeFileTrace::IsEnabled() && !tracedFinalPath.exchange(true, std::memory_order_relaxed))
        {
            RuntimeFileTrace::Record(RuntimeFileTrace::Event::ReaderValidation,
                expectedPath, SameWindowsPath(actual, expectedPath), "RootedFileReader", &actual);
        }
        return actual;
    }
#endif

    PinnedDirectory parent;
    std::filesystem::path filename;
    HeldHandle handle;
    uint64_t size = 0;
#if defined(_WIN32)
    BY_HANDLE_FILE_INFORMATION identity{};
    std::filesystem::path expectedPath;
    mutable std::atomic<bool> tracedFinalPath{false};
#else
    struct stat identity{};
#endif
};

RootedFileReader::RootedFileReader(
    const std::filesystem::path& root, const std::filesystem::path& relativePath)
    try : impl(std::make_unique<Impl>(RootedPath(root, relativePath)))
{
}
catch (...)
{
    if (RuntimeFileTrace::IsEnabled())
    {
        try
        {
            RuntimeFileTrace::Record(RuntimeFileTrace::Event::ReaderValidation,
                root / relativePath, false, "RootedFileReader");
        }
        catch (...) {}
    }
    throw;
}

RootedFileReader::~RootedFileReader() = default;

uint64_t RootedFileReader::Size() const
{
    impl->Validate();
    return impl->size;
}

void RootedFileReader::ValidateIdentity() const
{
    impl->Validate();
}

void RootedFileReader::Seek(uint64_t offset)
{
    impl->Validate();
    if (offset > impl->size || offset > uint64_t(std::numeric_limits<int64_t>::max()))
        throw std::runtime_error("Rooted input seek out of range");
#if defined(_WIN32)
    LARGE_INTEGER position{};
    position.QuadPart = static_cast<LONGLONG>(offset);
    if (!SetFilePointerEx(impl->handle.Get(), position, nullptr, FILE_BEGIN))
        FileError("Failed to seek rooted input");
#else
    if (offset > uint64_t(std::numeric_limits<off_t>::max()) ||
        lseek(impl->handle.Get(), static_cast<off_t>(offset), SEEK_SET) < 0)
        FileError("Failed to seek rooted input");
#endif
}

size_t RootedFileReader::Read(void* data, size_t size)
{
    if (size == 0)
        return 0;
    if (!data)
        throw std::runtime_error("Cannot read into a null byte range");
    const size_t count = std::min(size, size_t{1024 * 1024});
#if defined(_WIN32)
    DWORD readBytes = 0;
    if (!ReadFile(impl->handle.Get(), data, static_cast<DWORD>(count), &readBytes, nullptr))
        FileError("Failed to read rooted input handle");
#else
    ssize_t readBytes;
    do
    {
        readBytes = read(impl->handle.Get(), data, count);
    } while (readBytes < 0 && errno == EINTR);
    if (readBytes < 0)
        FileError("Failed to read rooted input descriptor");
#endif
    return static_cast<size_t>(readBytes);
}

std::vector<uint8_t> ReadBinaryFile(const std::filesystem::path& path)
{
    RootedFileReader input(path.has_parent_path() ? path.parent_path() : std::filesystem::path("."),
        path.filename());
    const auto size = input.Size();
    if (size > std::numeric_limits<size_t>::max())
        throw std::runtime_error("File exceeds addressable read size");
    std::vector<uint8_t> bytes(static_cast<size_t>(size));
    size_t offset = 0;
    while (offset < bytes.size())
    {
        const auto count = input.Read(bytes.data() + offset, bytes.size() - offset);
        if (count == 0)
            throw std::runtime_error("Truncated file");
        offset += count;
    }
    uint8_t extra{};
    if (input.Read(&extra, 1) != 0)
        throw std::runtime_error("File grew during read");
    input.ValidateIdentity();
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

    TemporaryFile temporary(path);
    temporary.Write(data, size);
    temporary.Commit(path);
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
    std::vector<std::unique_ptr<TemporaryFile>> stagedWrites;
    stagedWrites.reserve(writes.size());
    for (const AtomicFileWrite& write : writes)
    {
        if (write.path.has_parent_path())
        {
            std::filesystem::create_directories(write.path.parent_path());
        }
        auto temporary = std::make_unique<TemporaryFile>(write.path);
        temporary->Write(write.bytes.data(), write.bytes.size());
        stagedWrites.push_back(std::move(temporary));
    }

    for (size_t index = 0; index < writes.size(); ++index)
        stagedWrites[index]->Commit(writes[index].path);
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
