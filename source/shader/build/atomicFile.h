#pragma once

// File responsibility: Provides same-directory temporary writes and atomic
// replacement for shader artifacts, manifests, and generated includes.

#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

namespace VL
{

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
