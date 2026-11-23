#pragma once

// File responsibility: Provides same-directory temporary writes and atomic
// replacement for shader artifacts, manifests, and generated includes.

#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace VL
{

class PinnedFileDirectory
{
public:
    explicit PinnedFileDirectory(const std::filesystem::path& path);
    ~PinnedFileDirectory();
    PinnedFileDirectory(const PinnedFileDirectory&) = delete;
    PinnedFileDirectory& operator=(const PinnedFileDirectory&) = delete;
    const std::filesystem::path& Path() const;
    void ValidateIdentity() const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl;
};

// 从可信根逐级拒绝链接并持有祖先句柄；属性与读取使用同一文件句柄，
// 供资产读取边界执行流式摘要校验，不定义资产内容身份或 schema。
class RootedFileReader
{
public:
    RootedFileReader(const std::filesystem::path& root, const std::filesystem::path& relativePath);
    ~RootedFileReader();
    RootedFileReader(const RootedFileReader&) = delete;
    RootedFileReader& operator=(const RootedFileReader&) = delete;
    uint64_t Size() const;
    size_t Read(void* data, size_t size);
    void Seek(uint64_t offset);
    void ValidateIdentity() const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl;
};

struct AtomicFileWrite
{
    std::filesystem::path path;
    std::vector<uint8_t> bytes;
};

std::vector<uint8_t> ReadBinaryFile(const std::filesystem::path& path);

void WriteBinaryFileAtomically(
    const std::filesystem::path& path,
    const void* data,
    size_t size);

void WriteBinaryFileAtomically(
    const std::filesystem::path& path,
    const std::vector<uint8_t>& data);

void WriteTextFileAtomically(
    const std::filesystem::path& path,
    std::string_view text);

// Stages every file before replacing any destination. The caller commits its
// manifest only after this returns, so a process interruption leaves the old
// manifest as the authoritative validity marker.
void WriteFileBatchAtomically(const std::vector<AtomicFileWrite>& writes);

// Returns false without touching the destination when the bytes are identical.
bool WriteTextFileIfChangedAtomically(
    const std::filesystem::path& path,
    std::string_view text);

} // namespace VL
