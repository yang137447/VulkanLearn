#include "shader/reload/shaderFileMonitor.h"

#include <algorithm>
#include <set>
#include <stdexcept>
#include <system_error>

#include "shader/build/contentHash.h"

namespace VL
{
namespace
{

bool IsGenerateOrSpvDirectory(const std::filesystem::path& component)
{
    return component == "generate" || component == "spv";
}

} // namespace

bool ShaderFileMonitor::IsTemporaryFileName(
    const std::filesystem::path& fileName)
{
    const std::string name = fileName.string();
    if (name.empty())
    {
        return false;
    }
    if (name.front() == '.' || name.front() == '~' || name.back() == '~')
    {
        return true;
    }
    static constexpr const char* TemporaryExtensions[] = {
        ".tmp", ".swp", ".swo", ".bak", ".old"};
    for (const char* extension : TemporaryExtensions)
    {
        if (name.size() >= std::char_traits<char>::length(extension) &&
            name.compare(
                name.size() - std::char_traits<char>::length(extension),
                std::char_traits<char>::length(extension),
                extension) == 0)
        {
            return true;
        }
    }
    return false;
}

bool ShaderFileMonitor::IsSourceOfTruthPath(
    const std::filesystem::path& relativePath)
{
    if (relativePath.empty() || relativePath.is_absolute())
    {
        return false;
    }
    for (const std::filesystem::path& component : relativePath)
    {
        if (IsGenerateOrSpvDirectory(component))
        {
            return false;
        }
    }
    if (IsTemporaryFileName(relativePath.filename()))
    {
        return false;
    }

    const std::string extension = relativePath.extension().string();
    if (extension == ".vert" || extension == ".frag" ||
        extension == ".comp" || extension == ".glsl")
    {
        return true;
    }

    const std::string fileName = relativePath.filename().string();
    if (extension == ".json" &&
        fileName.rfind("M_", 0) == 0)
    {
        return true;
    }
    return false;
}

void ShaderFileMonitor::Initialize(
    const std::filesystem::path& shaderRoot,
    std::chrono::milliseconds interval,
    size_t stablePolls)
{
    glslRoot = std::filesystem::weakly_canonical(
        shaderRoot / "glsl");
    if (!std::filesystem::is_directory(glslRoot))
    {
        throw std::runtime_error(
            "Shader GLSL root does not exist for file monitoring: " +
            glslRoot.string());
    }
    pollInterval = interval;
    requiredStablePolls = stablePolls == 0 ? 1 : stablePolls;
    sources.clear();
    lastScanTime = {};
    scanCount = 0;
    testScanSuspended = false;
    baselineSeeded = false;
    initialized = true;
}

std::optional<ShaderFileMonitor::ChangeBatch> ShaderFileMonitor::Poll()
{
    if (!initialized)
    {
        return std::nullopt;
    }

    const auto now = std::chrono::steady_clock::now();
    if (testScanSuspended)
    {
        return std::nullopt;
    }
    if (now - lastScanTime < pollInterval)
    {
        return std::nullopt;
    }
    lastScanTime = now;
    ++pollsSinceFullHash;
    const bool forceFullHash =
        pollsSinceFullHash >= fullHashEveryNPolls;
    if (forceFullHash)
    {
        // mtime/size is only a fast candidate gate. Periodic full re-hashing
        // catches edits that preserve both fields (coarse timestamp
        // granularity, tools that restore timestamps) so the BLAKE3-256
        // digest remains the authoritative change signal over time.
        pollsSinceFullHash = 0;
    }

    std::map<std::string, TrackedSource> scannedSources;
    ++scanCount;
    ScanSources(scannedSources);

    ChangeBatch batch;
    const bool firstScan = !baselineSeeded;
    for (auto& [identity, next] : scannedSources)
    {
        const auto existingIt = sources.find(identity);
        if (existingIt == sources.end())
        {
            // New source discovered after the initial seed. Its digest must be
            // stable for the configured number of polls before it is reported.
            next.baselineDigest = HashSourceOrDefer(next.absolutePath);
            if (firstScan)
            {
                continue;
            }
            next.candidateDigest = next.baselineDigest;
            next.stablePollCount = 1;
            batch.observedSources.push_back(identity);
            if (next.stablePollCount >= requiredStablePolls)
            {
                next.stablePollCount = 0;
                batch.changedSources.push_back(identity);
            }
            continue;
        }

        const TrackedSource& previous = existingIt->second;
        if (!forceFullHash &&
            next.size == previous.size &&
            next.lastWriteTime == previous.lastWriteTime &&
            !previous.baselineDigest.empty() &&
            previous.stablePollCount == 0)
        {
            // mtime/size fast path. Any candidate that does pass the gate is
            // still confirmed by its BLAKE3-256 digest below.
            next = previous;
            continue;
        }

        next.baselineDigest = previous.baselineDigest;
        next.candidateDigest = previous.candidateDigest;
        next.stablePollCount = previous.stablePollCount;

        const std::string currentDigest =
            HashSourceOrDefer(next.absolutePath);
        if (currentDigest.empty())
        {
            // Transiently unreadable (still being written). Defer without
            // committing a half-written state and retry next poll.
            next.stablePollCount = 0;
            continue;
        }
        if (currentDigest == previous.baselineDigest)
        {
            // Content is unchanged or settled back to the baseline.
            if (previous.stablePollCount > 0)
            {
                batch.observedSources.push_back(identity);
            }
            next.candidateDigest.clear();
            next.stablePollCount = 0;
            continue;
        }

        if (currentDigest == previous.candidateDigest &&
            previous.stablePollCount > 0)
        {
            next.stablePollCount = previous.stablePollCount + 1;
        }
        else
        {
            next.candidateDigest = currentDigest;
            next.stablePollCount = 1;
            batch.observedSources.push_back(identity);
        }

        if (next.stablePollCount >= requiredStablePolls)
        {
            next.baselineDigest = currentDigest;
            next.candidateDigest.clear();
            next.stablePollCount = 0;
            batch.changedSources.push_back(identity);
        }
    }

    // Deleted or renamed-away sources get an explicit debounced event.
    for (const auto& [identity, previous] : sources)
    {
        if (scannedSources.count(identity) != 0)
        {
            continue;
        }

        TrackedSource& missing = scannedSources[identity];
        missing = previous;
        missing.exists = false;
        missing.size = 0;
        missing.lastWriteTime.reset();
        if (missing.baselineDigest.empty())
        {
            // Already confirmed missing; keep the entry for rename-back.
            continue;
        }

        if (missing.candidateDigest.empty() && missing.stablePollCount > 0)
        {
            ++missing.stablePollCount;
        }
        else
        {
            missing.candidateDigest.clear();
            missing.stablePollCount = 1;
            batch.observedSources.push_back(identity);
        }

        if (missing.stablePollCount >= requiredStablePolls)
        {
            missing.baselineDigest.clear();
            missing.stablePollCount = 0;
            batch.changedSources.push_back(identity);
        }
    }

    sources = std::move(scannedSources);
    baselineSeeded = true;
    std::sort(
        batch.observedSources.begin(),
        batch.observedSources.end());
    batch.observedSources.erase(
        std::unique(
            batch.observedSources.begin(),
            batch.observedSources.end()),
        batch.observedSources.end());
    std::sort(
        batch.changedSources.begin(),
        batch.changedSources.end());
    batch.changedSources.erase(
        std::unique(
            batch.changedSources.begin(),
            batch.changedSources.end()),
        batch.changedSources.end());
    return batch.observedSources.empty() &&
            batch.changedSources.empty()
        ? std::nullopt
        : std::optional<ChangeBatch>(std::move(batch));
}

void ShaderFileMonitor::RefreshBaselineForSources(
    const std::vector<std::string>& normalizedSources)
{
    if (!initialized)
    {
        return;
    }
    for (const std::string& identity : normalizedSources)
    {
        auto sourceIt = sources.find(identity);
        if (sourceIt == sources.end())
        {
            continue;
        }
        TrackedSource& source = sourceIt->second;
        if (!source.exists)
        {
            continue;
        }
        try
        {
            source.baselineDigest =
                ContentHasher::HashFile(source.absolutePath).ToHex();
            source.candidateDigest = source.baselineDigest;
            source.stablePollCount = 0;
        }
        catch (const std::exception&)
        {
            // Leave the old baseline; the next poll retries the hash.
        }
    }
}

size_t ShaderFileMonitor::GetTrackedSourceCount() const
{
    return sources.size();
}

bool ShaderFileMonitor::HasUnstableSourceChanges() const
{
    for (const auto& [identity, source] : sources)
    {
        (void)identity;
        if (source.stablePollCount > 0)
        {
            return true;
        }
    }
    return false;
}

void ShaderFileMonitor::ScanSources(
    std::map<std::string, TrackedSource>& scannedSources) const
{
    std::error_code iterationError;
    std::filesystem::recursive_directory_iterator iterator(
        glslRoot,
        std::filesystem::directory_options::skip_permission_denied,
        iterationError);
    const std::filesystem::recursive_directory_iterator end;
    while (iterator != end)
    {
        if (iterationError)
        {
            break;
        }

        const std::filesystem::directory_entry& entry = *iterator;
        if (entry.is_regular_file(iterationError) && !iterationError)
        {
            const std::filesystem::path relative =
                std::filesystem::relative(
                    entry.path(),
                    glslRoot,
                    iterationError);
            if (!iterationError && IsSourceOfTruthPath(relative))
            {
                const std::string identity =
                    relative.generic_string();
                TrackedSource source;
                source.absolutePath = entry.path();
                source.exists = true;
                source.size = entry.file_size(iterationError);
                source.lastWriteTime =
                    iterationError
                    ? std::optional<std::filesystem::file_time_type>()
                    : std::optional<std::filesystem::file_time_type>(
                          entry.last_write_time(iterationError));
                scannedSources.emplace(
                    std::move(identity),
                    std::move(source));
            }
        }
        iterationError.clear();
        iterator.increment(iterationError);
    }
}

std::string ShaderFileMonitor::HashSourceOrDefer(
    const std::filesystem::path& path) const
{
    try
    {
        return ContentHasher::HashFile(path).ToHex();
    }
    catch (const std::exception&)
    {
        // The file is still being written or is transiently unreadable. The
        // next poll re-hashes it; no half-written content is ever committed.
        return {};
    }
}

} // namespace VL
