#pragma once

// File responsibility: Provides renderer-independent MI file snapshots and
// conflict-aware atomic persistence for editor services and command executors.

#include "shader/build/contentHash.h"

#include <cstddef>
#include <filesystem>
#include <string>
#include <string_view>

namespace VL::Editor::Persistence
{

enum class MaterialInstancePersistenceStatus
{
    SnapshotReady,
    Saved,
    InvalidPath,
    Missing,
    NotRegularFile,
    ReadFailed,
    SourceChanged,
    WriteFailed
};

struct MaterialInstanceFileSnapshot
{
    MaterialInstancePersistenceStatus status =
        MaterialInstancePersistenceStatus::ReadFailed;
    std::filesystem::path path;
    ContentDigest digest{};
    std::string errorMessage;

    bool Succeeded() const
    {
        return status == MaterialInstancePersistenceStatus::SnapshotReady;
    }
};

struct MaterialInstanceSaveResult
{
    MaterialInstancePersistenceStatus status =
        MaterialInstancePersistenceStatus::WriteFailed;
    std::filesystem::path path;
    ContentDigest expectedDigest{};
    ContentDigest observedDigest{};
    ContentDigest newDigest{};
    std::string errorMessage;

    bool Succeeded() const
    {
        return status == MaterialInstancePersistenceStatus::Saved;
    }

    bool HasSourceConflict() const
    {
        return status == MaterialInstancePersistenceStatus::SourceChanged;
    }
};

class MaterialInstancePersistence
{
public:
    // Reads the current source digest without requiring a live World or renderer.
    static MaterialInstanceFileSnapshot ReadSnapshot(
        const std::filesystem::path& path);

    // Saves text only when the source still has expectedDigest. The candidate is
    // staged beside the target and atomically replaces it after flush and close.
    static MaterialInstanceSaveResult SaveTextIfUnchanged(
        const std::filesystem::path& path,
        const ContentDigest& expectedDigest,
        std::string_view text);

    // Binary counterpart for callers that already own serialized candidate bytes.
    static MaterialInstanceSaveResult SaveBytesIfUnchanged(
        const std::filesystem::path& path,
        const ContentDigest& expectedDigest,
        const void* data,
        size_t size);

private:
    static std::filesystem::path NormalizePath(
        const std::filesystem::path& path);

    static MaterialInstanceSaveResult SaveBytesIfUnchangedInternal(
        const std::filesystem::path& path,
        const ContentDigest& expectedDigest,
        const void* data,
        size_t size);
};

const char* ToString(MaterialInstancePersistenceStatus status);

} // namespace VL::Editor::Persistence
