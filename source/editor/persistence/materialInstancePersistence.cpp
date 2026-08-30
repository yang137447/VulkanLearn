#include "editor/persistence/materialInstancePersistence.h"

#include "shader/build/atomicFile.h"

#include <cstring>
#include <filesystem>
#include <stdexcept>
#include <system_error>
#include <vector>

namespace VL::Editor::Persistence
{
namespace
{

MaterialInstanceFileSnapshot ReadSnapshotNormalized(
    const std::filesystem::path& normalizedPath)
{
    MaterialInstanceFileSnapshot result;
    result.path = normalizedPath;

    std::error_code statusError;
    const std::filesystem::file_status fileStatus =
        std::filesystem::status(normalizedPath, statusError);
    if (statusError)
    {
        if (statusError == std::errc::no_such_file_or_directory)
        {
            result.status = MaterialInstancePersistenceStatus::Missing;
            result.errorMessage =
                "MI file does not exist: " + normalizedPath.string();
            return result;
        }
        result.status = MaterialInstancePersistenceStatus::ReadFailed;
        result.errorMessage =
            "Failed to inspect MI file '" + normalizedPath.string() + "': " +
            statusError.message();
        return result;
    }

    if (!std::filesystem::exists(fileStatus))
    {
        result.status = MaterialInstancePersistenceStatus::Missing;
        result.errorMessage =
            "MI file does not exist: " + normalizedPath.string();
        return result;
    }

    if (!std::filesystem::is_regular_file(fileStatus))
    {
        result.status = MaterialInstancePersistenceStatus::NotRegularFile;
        result.errorMessage =
            "MI path is not a regular file: " + normalizedPath.string();
        return result;
    }

    try
    {
        result.digest = ContentHasher::HashFile(normalizedPath);
        result.status = MaterialInstancePersistenceStatus::SnapshotReady;
    }
    catch (const std::exception& exception)
    {
        result.status = MaterialInstancePersistenceStatus::ReadFailed;
        result.errorMessage =
            "Failed to hash MI file '" + normalizedPath.string() + "': " +
            exception.what();
    }
    return result;
}

MaterialInstanceSaveResult MakeSaveResult(
    const std::filesystem::path& path,
    const ContentDigest& expectedDigest)
{
    MaterialInstanceSaveResult result;
    result.path = path;
    result.expectedDigest = expectedDigest;
    return result;
}

MaterialInstanceSaveResult FromSnapshotFailure(
    const MaterialInstanceFileSnapshot& snapshot,
    const ContentDigest& expectedDigest)
{
    MaterialInstanceSaveResult result =
        MakeSaveResult(snapshot.path, expectedDigest);
    result.status = snapshot.status;
    result.observedDigest = snapshot.digest;
    result.errorMessage = snapshot.errorMessage;
    return result;
}

} // namespace

const char* ToString(MaterialInstancePersistenceStatus status)
{
    switch (status)
    {
    case MaterialInstancePersistenceStatus::SnapshotReady:
        return "SnapshotReady";
    case MaterialInstancePersistenceStatus::Saved:
        return "Saved";
    case MaterialInstancePersistenceStatus::InvalidPath:
        return "InvalidPath";
    case MaterialInstancePersistenceStatus::Missing:
        return "Missing";
    case MaterialInstancePersistenceStatus::NotRegularFile:
        return "NotRegularFile";
    case MaterialInstancePersistenceStatus::ReadFailed:
        return "ReadFailed";
    case MaterialInstancePersistenceStatus::SourceChanged:
        return "SourceChanged";
    case MaterialInstancePersistenceStatus::WriteFailed:
        return "WriteFailed";
    }
    return "Unknown";
}

std::filesystem::path MaterialInstancePersistence::NormalizePath(
    const std::filesystem::path& path)
{
    if (path.empty())
    {
        throw std::invalid_argument("MI persistence path must not be empty");
    }

    std::error_code absoluteError;
    const std::filesystem::path absolutePath =
        std::filesystem::absolute(path, absoluteError);
    if (absoluteError)
    {
        throw std::runtime_error(
            "Failed to normalize MI path '" + path.string() + "': " +
            absoluteError.message());
    }
    return absolutePath.lexically_normal();
}

MaterialInstanceFileSnapshot MaterialInstancePersistence::ReadSnapshot(
    const std::filesystem::path& path)
{
    MaterialInstanceFileSnapshot result;
    result.path = path;
    try
    {
        result.path = NormalizePath(path);
    }
    catch (const std::exception& exception)
    {
        result.status = MaterialInstancePersistenceStatus::InvalidPath;
        result.errorMessage = exception.what();
        return result;
    }
    return ReadSnapshotNormalized(result.path);
}

MaterialInstanceSaveResult MaterialInstancePersistence::SaveTextIfUnchanged(
    const std::filesystem::path& path,
    const ContentDigest& expectedDigest,
    std::string_view text)
{
    return SaveBytesIfUnchanged(
        path,
        expectedDigest,
        text.data(),
        text.size());
}

MaterialInstanceSaveResult MaterialInstancePersistence::SaveBytesIfUnchanged(
    const std::filesystem::path& path,
    const ContentDigest& expectedDigest,
    const void* data,
    size_t size)
{
    MaterialInstanceSaveResult result;
    result.path = path;
    result.expectedDigest = expectedDigest;
    try
    {
        result.path = NormalizePath(path);
    }
    catch (const std::exception& exception)
    {
        result.status = MaterialInstancePersistenceStatus::InvalidPath;
        result.errorMessage = exception.what();
        return result;
    }
    return SaveBytesIfUnchangedInternal(
        result.path,
        expectedDigest,
        data,
        size);
}

MaterialInstanceSaveResult
MaterialInstancePersistence::SaveBytesIfUnchangedInternal(
    const std::filesystem::path& path,
    const ContentDigest& expectedDigest,
    const void* data,
    size_t size)
{
    MaterialInstanceSaveResult result = MakeSaveResult(path, expectedDigest);
    if (size > 0 && data == nullptr)
    {
        result.status = MaterialInstancePersistenceStatus::WriteFailed;
        result.errorMessage = "Cannot save a null MI byte range";
        return result;
    }

    const MaterialInstanceFileSnapshot initialSnapshot =
        ReadSnapshotNormalized(path);
    if (!initialSnapshot.Succeeded())
    {
        return FromSnapshotFailure(initialSnapshot, expectedDigest);
    }

    result.observedDigest = initialSnapshot.digest;
    if (initialSnapshot.digest != expectedDigest)
    {
        result.status = MaterialInstancePersistenceStatus::SourceChanged;
        result.errorMessage =
            "MI source changed before save: '" + path.string() + "'";
        return result;
    }

    const ContentDigest candidateDigest = ContentHasher::HashBytes(data, size);

    // 二次摘要复核缩短检查与替换之间的窗口；原子 helper 负责 sibling 写入及关闭后替换。
    const MaterialInstanceFileSnapshot confirmedSnapshot =
        ReadSnapshotNormalized(path);
    if (!confirmedSnapshot.Succeeded())
    {
        return FromSnapshotFailure(confirmedSnapshot, expectedDigest);
    }

    result.observedDigest = confirmedSnapshot.digest;
    if (confirmedSnapshot.digest != expectedDigest)
    {
        result.status = MaterialInstancePersistenceStatus::SourceChanged;
        result.errorMessage =
            "MI source changed during save preparation: '" + path.string() +
            "'";
        return result;
    }

    try
    {
        std::vector<uint8_t> candidateBytes(size);
        if (size > 0)
        {
            std::memcpy(candidateBytes.data(), data, size);
        }
        WriteFileBatchAtomically({AtomicFileWrite{path, std::move(candidateBytes)}});
    }
    catch (const std::exception& exception)
    {
        result.status = MaterialInstancePersistenceStatus::WriteFailed;
        result.errorMessage =
            "Failed to atomically save MI file '" + path.string() + "': " +
            exception.what();
        return result;
    }

    result.status = MaterialInstancePersistenceStatus::Saved;
    result.newDigest = candidateDigest;
    result.observedDigest = candidateDigest;
    return result;
}

} // namespace VL::Editor::Persistence
