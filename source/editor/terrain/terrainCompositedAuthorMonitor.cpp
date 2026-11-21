#include "editor/terrain/terrainCompositedAuthorMonitor.h"

#include <future>
#include <fstream>
#include <iterator>
#include <map>
#include <set>
#include <stdexcept>
#include <utility>
#include <algorithm>
#include <iostream>
#include <mutex>

#include "terrain/terrainCompositedDependencies.h"
#include "terrain/terrainCompositedVTStore.h"
#include "terrain/terrainCompositedVerifiedInputs.h"

namespace VL::Editor
{

struct TerrainCompositedAuthorMonitor::BaselineLoadSession::Impl
{
    mutable std::mutex mutex;
    std::shared_ptr<const TerrainCompositedVTStore> store;
    uint64_t worldGeneration = 0;
    ContentDigest publicationIdentity;
    std::vector<std::shared_ptr<TerrainCompositedVerifiedInputs>> owners;
    Statistics statistics;
    Clock::time_point started = Clock::now();
};

TerrainCompositedAuthorMonitor::BaselineLoadSession::BaselineLoadSession()
    : impl(std::make_unique<Impl>()) {}

TerrainCompositedAuthorMonitor::BaselineLoadSession::~BaselineLoadSession()
{
    Close();
}

void TerrainCompositedAuthorMonitor::BaselineLoadSession::VerifyObject(
    const TerrainCompositedAuthorBaseline& baseline, const std::string& objectId)
{
    std::lock_guard<std::mutex> lock(impl->mutex);
    if (impl->statistics.closed || !baseline.store || !baseline.worldGeneration ||
        !baseline.verificationOwner)
        throw std::invalid_argument("Baseline object verification requires an open scoped Store session");
    if (impl->store != baseline.store || impl->worldGeneration != baseline.worldGeneration)
    {
        const auto& production = baseline.store->Production();
        production.Validate();
        const auto identity = production.ContentHash();
        impl->store = baseline.store;
        impl->worldGeneration = baseline.worldGeneration;
        impl->publicationIdentity = identity;
        ++impl->statistics.bindings;
    }
    const auto& object = baseline.store->Production().Object(objectId);
    ++impl->statistics.objectRequests;
    impl->statistics.requestedBytes += object.bytes;
    const auto& owner = baseline.verificationOwner;
    const TerrainCompositedVerifiedInputBinding binding{
        baseline.store, baseline.worldGeneration, impl->publicationIdentity, baseline.surfaceId};
    if (!owner->Matches(binding))
        throw std::invalid_argument("Baseline verification owner belongs to another Store/World/publication");
    if (std::find(impl->owners.begin(), impl->owners.end(), owner) == impl->owners.end())
        impl->owners.push_back(owner);
    const auto before = owner->Statistics();
    if (before.closed)
    {
        if (!owner->HasVerified(binding, objectId))
            throw std::invalid_argument("Closed baseline owner has no registered immutable object proof");
        ++impl->statistics.cacheHits;
        return;
    }
    const auto consumeMetrics = [&] {
        const auto after = owner->Statistics();
        impl->statistics.objectReads += after.readCalls - before.readCalls;
        impl->statistics.readBytes += after.actualReadBytes - before.actualReadBytes;
        impl->statistics.cacheHits += after.hits - before.hits;
        impl->statistics.failedReads += after.failures - before.failures;
    };
    try
    {
        // 唯一 hash/proof 由 terrain owner 建立；Editor 不保存另一份 payload 或验证缓存。
        owner->VerifyObject(binding, objectId);
        consumeMetrics();
    }
    catch (...)
    {
        consumeMetrics();
        throw;
    }
}

TerrainCompositedAuthorMonitor::BaselineLoadSession::Statistics
TerrainCompositedAuthorMonitor::BaselineLoadSession::Stats() const
{
    std::lock_guard<std::mutex> lock(impl->mutex);
    return impl->statistics;
}

void TerrainCompositedAuthorMonitor::BaselineLoadSession::Close()
{
    std::lock_guard<std::mutex> lock(impl->mutex);
    if (impl->statistics.closed)
        return;
    impl->statistics.closed = true;
    for (const auto& owner : impl->owners)
        owner->Close();
    const auto ended = Clock::now();
    std::cout << "[RVT_EDITOR_BASELINE_SCOPE] " << nlohmann::json{
        {"worldGeneration", impl->worldGeneration},
        {"storeHandle", static_cast<uint64_t>(reinterpret_cast<uintptr_t>(impl->store.get()))},
        {"publicationIdentity", impl->publicationIdentity.ToHex()},
        {"beginMonotonicMs", std::chrono::duration<double, std::milli>(
            impl->started.time_since_epoch()).count()},
        {"endMonotonicMs", std::chrono::duration<double, std::milli>(
            ended.time_since_epoch()).count()},
        {"objectRequests", impl->statistics.objectRequests},
        {"requestedBytes", impl->statistics.requestedBytes},
        {"objectReads", impl->statistics.objectReads},
        {"readBytes", impl->statistics.readBytes},
        {"cacheHits", impl->statistics.cacheHits},
        {"failedReads", impl->statistics.failedReads},
        {"bindings", impl->statistics.bindings}}.dump() << std::endl;
    impl->owners.clear();
    impl->store.reset();
}

struct TerrainCompositedAuthorMonitor::Impl
{
    struct Surface
    {
        SurfaceStatus status;
        TerrainCompositedDependencyRoots roots;
        nlohmann::json manifest;
        std::shared_ptr<const TerrainCompositedVTStore> productionStore;
    };

    struct CheckResult
    {
        std::string surfaceId;
        uint64_t generation = 0;
        std::string diagnostic;
        std::vector<ChangedDependency> changedDependencies;
    };

    struct Signature
    {
        std::filesystem::path resolved;
        bool exists = false;
        bool regular = false;
        uintmax_t size = 0;
        std::filesystem::file_time_type modified{};

        bool operator==(const Signature& other) const
        {
            return resolved == other.resolved && exists == other.exists &&
                regular == other.regular && size == other.size && modified == other.modified;
        }
    };

    struct CachedDependency
    {
        Signature signature;
        std::string digest;
        Clock::time_point lastHash{};
        bool verified = false;
    };

    struct ScanBatch
    {
        std::vector<CheckResult> results;
        std::map<std::string, CachedDependency> cache;
        uint64_t cacheRevision = 0;
        uint64_t metadataChecks = 0;
        uint64_t metadataCacheHits = 0;
        uint64_t fullReadCount = 0;
        uint64_t fullReadBytes = 0;
    };

    std::map<std::string, Surface> surfaces;
    std::map<std::string, CachedDependency> cache;
    std::future<ScanBatch> pending;
    std::chrono::milliseconds interval;
    std::chrono::milliseconds fullHashInterval;
    Clock::time_point lastScan{};
    uint64_t generation = 0;
    uint64_t completedChecks = 0;
    uint64_t failedChecks = 0;
    uint64_t discardedChecks = 0;
    uint64_t cacheRevision = 0;
    uint64_t metadataChecks = 0;
    uint64_t metadataCacheHits = 0;
    uint64_t fullReadCount = 0;
    uint64_t fullReadBytes = 0;
    bool scanRequested = true;
    std::shared_ptr<BaselineLoadSession> baselineSession;
    std::vector<std::weak_ptr<BaselineLoadSession>> baselineScopes;
    uint64_t baselineLoadCalls = 0;
    uint64_t baselineObjectRequests = 0;
    uint64_t baselineRequestedBytes = 0;
    uint64_t baselineObjectReads = 0;
    uint64_t baselineReadBytes = 0;
    uint64_t baselineCacheHits = 0;
    uint64_t baselineFailedReads = 0;
    uint64_t baselineBindings = 0;
    double baselineLoadMilliseconds = 0;

    Impl(std::chrono::milliseconds interval, std::chrono::milliseconds fullHashInterval)
        : interval(interval), fullHashInterval(fullHashInterval) {}

    static Signature Metadata(
        const std::filesystem::path& root, const std::string& relative)
    {
        Signature result;
        result.resolved = std::filesystem::weakly_canonical(root / relative);
        const auto withinRoot = result.resolved.lexically_relative(root);
        if (withinRoot.empty() || withinRoot.is_absolute())
            throw std::runtime_error("canonical dependency path escapes root");
        for (const auto& component : withinRoot)
            if (component == "..")
                throw std::runtime_error("canonical dependency path escapes root");
        const auto status = std::filesystem::status(result.resolved);
        result.exists = std::filesystem::exists(status);
        result.regular = std::filesystem::is_regular_file(status);
        if (result.regular)
        {
            result.size = std::filesystem::file_size(result.resolved);
            result.modified = std::filesystem::last_write_time(result.resolved);
        }
        return result;
    }

    static std::string Digest(const Signature& signature, const std::string& mode)
    {
        if (mode == "bytes")
            return ContentHasher::HashFile(signature.resolved).ToHex();
        std::ifstream input(signature.resolved, std::ios::binary);
        if (!input)
            throw std::runtime_error("cannot open required dependency");
        const std::string bytes(std::istreambuf_iterator<char>(input), {});
        if (input.bad())
            throw std::runtime_error("cannot read required dependency");
        return TerrainCompositedDependencyEntryHash(
            bytes, mode, signature.resolved.generic_string()).ToHex();
    }

    static ScanBatch Scan(
        const std::vector<Surface>& snapshot,
        std::map<std::string, CachedDependency> previous,
        Clock::time_point nowTime, std::chrono::milliseconds fullInterval,
        uint64_t revision, bool forceFull = false)
    {
        struct Observation
        {
            Signature signature;
            bool read = false;
            std::string digest;
            std::string diagnostic;
        };
        ScanBatch batch;
        batch.cacheRevision = revision;
        std::map<std::string, Observation> round;
        std::set<std::string> fullKeys;
        for (const auto& surface : snapshot)
        {
            if (!surface.status.checked)
            {
                for (const auto& entry : surface.manifest.at("dependencies"))
                {
                    const auto& root = entry.at("root") == "resource" ?
                        surface.roots.resourceRoot : surface.roots.projectRoot;
                    fullKeys.insert((root / entry.at("path").get<std::string>()).generic_string() +
                        "|" + entry.at("mode").get<std::string>());
                }
            }
        }
        for (const auto& surface : snapshot)
        {
            CheckResult check{surface.status.surfaceId, surface.status.generation, {}};
            for (const auto& entry : surface.manifest.at("dependencies"))
            {
                const auto name = entry.at("path").get<std::string>();
                const auto mode = entry.at("mode").get<std::string>();
                const auto& root = entry.at("root") == "resource" ?
                    surface.roots.resourceRoot : surface.roots.projectRoot;
                const std::string key = (root / name).generic_string() + "|" + mode;
                auto observation = round.find(key);
                ChangedDependency dependency{entry.at("root").get<std::string>(), name, mode,
                    entry.at("digest").get<std::string>(), {}, {}};
                try
                {
                    if (observation == round.end())
                    {
                        ++batch.metadataChecks;
                        Observation observed;
                        observed.signature = Metadata(root, name);
                        observation = round.emplace(key, std::move(observed)).first;
                    }
                    auto& observed = observation->second;
                    if (!observed.diagnostic.empty())
                        throw std::runtime_error(observed.diagnostic);
                    if (mode == "absent")
                    {
                        if (observed.signature.exists)
                            throw std::runtime_error("previously absent optional dependency now exists");
                        continue;
                    }
                    if (!observed.signature.regular)
                        throw std::runtime_error("required author dependency is missing");
                    const auto cached = previous.find(key);
                    const bool cachedValid = cached != previous.end() &&
                        cached->second.verified && cached->second.signature == observed.signature;
                    const bool fullDue = !cachedValid ||
                        nowTime < cached->second.lastHash ||
                        nowTime - cached->second.lastHash >= fullInterval;
                    const bool needsRead = forceFull || fullKeys.count(key) != 0 || fullDue;
                    if (!observed.read && needsRead)
                    {
                        ++batch.fullReadCount;
                        batch.fullReadBytes += observed.signature.size;
                        observed.digest = Digest(observed.signature, mode);
                        if (!(observed.signature == Metadata(root, name)))
                        {
                            observed.digest.clear();
                            throw std::runtime_error("dependency changed while hashing");
                        }
                        observed.read = true;
                        batch.cache[key] = {observed.signature, observed.digest, nowTime, true};
                    }
                    if (!observed.read)
                    {
                        ++batch.metadataCacheHits;
                        observed.digest = cached->second.digest;
                        batch.cache[key] = cached->second;
                    }
                    dependency.observedDigest = observed.digest;
                    if (observed.digest != entry.at("digest").get<std::string>())
                        throw std::runtime_error("author digest changed; Cook is stale");
                }
                catch (const std::exception& exception)
                {
                    dependency.diagnostic = exception.what();
                    check.changedDependencies.push_back(std::move(dependency));
                    if (observation != round.end() && observation->second.digest.empty())
                        observation->second.diagnostic = exception.what();
                    if (check.diagnostic.empty())
                        check.diagnostic = "Terrain composited dependency [" + name + "]: " +
                            exception.what();
                }
            }
            batch.results.push_back(std::move(check));
        }
        return batch;
    }

    void ConsumeMetrics(ScanBatch& batch, bool mergeCache = false)
    {
        metadataChecks += batch.metadataChecks;
        metadataCacheHits += batch.metadataCacheHits;
        fullReadCount += batch.fullReadCount;
        fullReadBytes += batch.fullReadBytes;
        if (batch.cacheRevision == cacheRevision)
        {
            if (mergeCache)
            {
                for (auto& entry : batch.cache)
                    cache.insert_or_assign(entry.first, std::move(entry.second));
            }
            else
                cache = std::move(batch.cache);
        }
    }

    bool Apply(const CheckResult& result)
    {
        const auto found = surfaces.find(result.surfaceId);
        if (found == surfaces.end() ||
            found->second.status.generation != result.generation)
        {
            ++discardedChecks;
            return false;
        }
        ++completedChecks;
        auto& status = found->second.status;
        const bool previousChecked = status.checked;
        const bool previousExternal = status.externalChange;
        const bool previousConflict = status.conflict;
        const std::string previousDiagnostic = status.diagnostic;
        bool dependenciesChanged = false;
        status.checked = true;
        if (!result.diagnostic.empty())
        {
            ++failedChecks;
            status.externalChange = true;
            status.conflict = status.conflict || status.dirty;
            status.diagnostic = result.diagnostic;
            for (const auto& dependency : result.changedDependencies)
            {
                const auto existing = std::find_if(status.changedDependencies.begin(),
                    status.changedDependencies.end(), [&dependency](const auto& current) {
                        return current.root == dependency.root && current.path == dependency.path;
                    });
                if (existing == status.changedDependencies.end())
                {
                    status.changedDependencies.push_back(dependency);
                    dependenciesChanged = true;
                }
                else
                {
                    dependenciesChanged |= existing->observedDigest != dependency.observedDigest ||
                        existing->diagnostic != dependency.diagnostic;
                    *existing = dependency;
                }
            }
        }
        return previousChecked != status.checked ||
            previousExternal != status.externalChange ||
            previousConflict != status.conflict ||
            previousDiagnostic != status.diagnostic || dependenciesChanged;
    }
};

TerrainCompositedAuthorMonitor::TerrainCompositedAuthorMonitor(
    std::chrono::milliseconds interval, std::chrono::milliseconds fullHashInterval)
    : impl(std::make_unique<Impl>(interval, fullHashInterval))
{
    if (interval.count() < 0 || fullHashInterval.count() <= 0)
        throw std::invalid_argument("Terrain author monitor intervals must be nonnegative/positive");
}

TerrainCompositedAuthorMonitor::~TerrainCompositedAuthorMonitor() = default;

bool TerrainCompositedAuthorMonitor::LoadBaseline(
    const TerrainCompositedAuthorBaseline& baseline,
    const TerrainCompositedDependencyRoots& roots, std::string& error)
{
    if (!impl->baselineSession)
        impl->baselineSession = std::make_shared<BaselineLoadSession>();
    return LoadBaseline(baseline, roots, impl->baselineSession, error);
}

bool TerrainCompositedAuthorMonitor::LoadBaseline(
    const TerrainCompositedAuthorBaseline& baseline,
    const TerrainCompositedDependencyRoots& roots,
    const std::shared_ptr<BaselineLoadSession>& session, std::string& error)
{
    error.clear();
    const auto started = Clock::now();
    const auto before = session ? session->Stats() : BaselineLoadSession::Statistics{};
    const auto finish = [&](bool success) {
        const auto ended = Clock::now();
        const auto after = session ? session->Stats() : before;
        const double elapsed = std::chrono::duration<double, std::milli>(ended - started).count();
        ++impl->baselineLoadCalls;
        impl->baselineObjectRequests += after.objectRequests - before.objectRequests;
        impl->baselineRequestedBytes += after.requestedBytes - before.requestedBytes;
        impl->baselineObjectReads += after.objectReads - before.objectReads;
        impl->baselineReadBytes += after.readBytes - before.readBytes;
        impl->baselineCacheHits += after.cacheHits - before.cacheHits;
        impl->baselineFailedReads += after.failedReads - before.failedReads;
        impl->baselineBindings += after.bindings - before.bindings;
        impl->baselineLoadMilliseconds += elapsed;
        std::cout << "[RVT_EDITOR_BASELINE_LOAD] " << nlohmann::json{
            {"surface", baseline.surfaceId}, {"worldGeneration", baseline.worldGeneration},
            {"storeHandle", static_cast<uint64_t>(reinterpret_cast<uintptr_t>(baseline.store.get()))},
            {"productionIdentity", baseline.productionIdentity.ToHex()}, {"success", success},
            {"beginMonotonicMs", std::chrono::duration<double, std::milli>(
                started.time_since_epoch()).count()},
            {"endMonotonicMs", std::chrono::duration<double, std::milli>(
                ended.time_since_epoch()).count()}, {"elapsedMs", elapsed},
            {"objectRequests", after.objectRequests - before.objectRequests},
            {"requestedBytes", after.requestedBytes - before.requestedBytes},
            {"objectReads", after.objectReads - before.objectReads},
            {"readBytes", after.readBytes - before.readBytes},
            {"cacheHits", after.cacheHits - before.cacheHits},
            {"failedReads", after.failedReads - before.failedReads}, {"error", error}}.dump() << std::endl;
    };
    try
    {
        if (session)
        {
            const auto existing = std::find_if(impl->baselineScopes.begin(), impl->baselineScopes.end(),
                [&](const auto& value) { return value.lock() == session; });
            if (existing == impl->baselineScopes.end())
                impl->baselineScopes.push_back(session);
        }
        const auto previous = impl->surfaces.find(baseline.surfaceId);
        if (!session || baseline.surfaceId.empty() || !baseline.store || baseline.dependencies.empty() ||
            (previous != impl->surfaces.end() && previous->second.status.dirty))
            throw std::invalid_argument("Terrain baseline requires a verified Store and no unsaved edits");
        const auto& production = baseline.store->Production();
        production.Validate();
        if (production.SurfaceContentHash(baseline.surfaceId) != baseline.productionIdentity ||
            baseline.store->Surface(baseline.surfaceId).dependencyHash != baseline.productionIdentity)
            throw std::invalid_argument("Terrain baseline production identity mismatch");
        const auto surface = std::find_if(production.surfaces.begin(), production.surfaces.end(),
            [&baseline](const auto& value) { return value.surfaceId == baseline.surfaceId; });
        if (surface == production.surfaces.end() ||
            surface->authorDependencies.size() != baseline.dependencies.size())
            throw std::invalid_argument("Terrain baseline requires the publication's captured author dependencies");
        for (const auto& dependency : baseline.dependencies)
        {
            const auto stored = std::find_if(surface->authorDependencies.begin(), surface->authorDependencies.end(),
                [&dependency](const auto& value) {
                    return value.root == dependency.root && value.path == dependency.path &&
                        value.mode == dependency.mode;
                });
            if (stored == surface->authorDependencies.end() || stored->digest != dependency.digest)
                throw std::invalid_argument("Terrain author baseline digest is not bound to the publication");
            auto expected = stored->productionObjectIds;
            auto observed = dependency.productionObjectIds;
            std::sort(expected.begin(), expected.end());
            std::sort(observed.begin(), observed.end());
            if (expected != observed)
                throw std::invalid_argument("Terrain author baseline object binding differs from the publication");
        }
        Impl::Surface candidate;
        candidate.status.surfaceId = baseline.surfaceId;
        candidate.status.manifestHash = baseline.productionIdentity.ToHex();
        candidate.status.worldGeneration = baseline.worldGeneration;
        candidate.roots.resourceRoot = std::filesystem::canonical(roots.resourceRoot);
        candidate.roots.projectRoot = std::filesystem::canonical(roots.projectRoot);
        candidate.productionStore = baseline.store;
        candidate.manifest["dependencies"] = nlohmann::json::array();
        std::set<std::string> dependencies;
        for (const auto& dependency : baseline.dependencies)
        {
            if ((dependency.root != "resource" && dependency.root != "project") ||
                dependency.path.empty() || dependency.productionObjectIds.empty() ||
                !dependencies.insert(dependency.root + "|" + dependency.path + "|" + dependency.mode).second)
                throw std::invalid_argument("Terrain baseline has invalid or duplicate author dependency");
            if (dependency.mode != "bytes" && dependency.mode != "absent" &&
                dependency.mode != "terrain-json-without-package-path-v1" &&
                dependency.mode != "terrain-material-instance-static-json-v2" &&
                dependency.mode != "terrain-material-static-json-v2")
                throw std::invalid_argument("Terrain baseline author dependency mode is unsupported");
            ContentDigest::FromHex(dependency.digest);
            const auto& root = dependency.root == "resource" ?
                candidate.roots.resourceRoot : candidate.roots.projectRoot;
            Impl::Metadata(root, dependency.path);
            for (const auto& objectId : dependency.productionObjectIds)
                session->VerifyObject(baseline, objectId);
            candidate.manifest["dependencies"].push_back({
                {"root", dependency.root}, {"path", dependency.path},
                {"mode", dependency.mode}, {"digest", dependency.digest}});
        }
        candidate.status.generation = ++impl->generation;
        impl->surfaces.insert_or_assign(baseline.surfaceId, std::move(candidate));
        impl->scanRequested = true;
        finish(true);
        return true;
    }
    catch (const std::exception& exception)
    {
        error = exception.what();
        finish(false);
        return false;
    }
}

bool TerrainCompositedAuthorMonitor::RegisterSurface(
    const std::string& surfaceId,
    const TerrainCompositedDependencyRoots& roots,
    const nlohmann::json& dependencyManifest,
    std::string& error)
{
    error.clear();
    const auto previous = impl->surfaces.find(surfaceId);
    if (surfaceId.empty() ||
        (previous != impl->surfaces.end() && previous->second.status.dirty))
    {
        error = surfaceId.empty() ? "Terrain surface identity is required" :
            "Cannot replace terrain author baseline while Paint/Sculpt or material edits are unsaved";
        return false;
    }
    try
    {
        Impl::Surface candidate;
        candidate.status.surfaceId = surfaceId;
        candidate.status.manifestHash =
            TerrainCompositedDependencyHash(dependencyManifest).ToHex();
        candidate.manifest = dependencyManifest;
        candidate.roots.resourceRoot = std::filesystem::canonical(roots.resourceRoot);
        candidate.roots.projectRoot = std::filesystem::canonical(roots.projectRoot);
        candidate.status.generation = ++impl->generation;
        impl->surfaces.insert_or_assign(surfaceId, std::move(candidate));
        impl->scanRequested = true;
        return true;
    }
    catch (const std::exception& exception)
    {
        error = exception.what();
        return false;
    }
}

void TerrainCompositedAuthorMonitor::UnregisterSurface(const std::string& surfaceId)
{
    impl->surfaces.erase(surfaceId);
}

std::vector<TerrainCompositedAuthorMonitor::SurfaceStatus>
TerrainCompositedAuthorMonitor::Poll(Clock::time_point nowTime)
{
    // 默认会话仅覆盖本批初始化；进入作者文件扫描前释放凭据，后续外改不能命中旧验证。
    for (const auto& reference : impl->baselineScopes)
        if (const auto session = reference.lock())
            session->Close();
    impl->baselineScopes.clear();
    impl->baselineSession.reset();
    std::vector<SurfaceStatus> changed;
    if (impl->pending.valid() &&
        impl->pending.wait_for(std::chrono::milliseconds(0)) == std::future_status::ready)
    {
        try
        {
            auto batch = impl->pending.get();
            impl->ConsumeMetrics(batch);
            for (const auto& result : batch.results)
            {
                if (impl->Apply(result))
                    changed.push_back(impl->surfaces.at(result.surfaceId).status);
            }
        }
        catch (const std::exception& exception)
        {
            for (auto& entry : impl->surfaces)
            {
                const Impl::CheckResult result{
                    entry.first, entry.second.status.generation, exception.what()};
                if (impl->Apply(result))
                    changed.push_back(entry.second.status);
            }
        }
    }
    if (impl->pending.valid() || impl->surfaces.empty() ||
        (!impl->scanRequested && nowTime >= impl->lastScan &&
         nowTime - impl->lastScan < impl->interval))
        return changed;
    try
    {
        std::vector<Impl::Surface> snapshot;
        snapshot.reserve(impl->surfaces.size());
        for (const auto& entry : impl->surfaces)
            snapshot.push_back(entry.second);
        impl->pending = std::async(std::launch::async,
            [snapshot = std::move(snapshot), cache = impl->cache,
             nowTime, fullInterval = impl->fullHashInterval, revision = impl->cacheRevision] {
                return Impl::Scan(snapshot, std::move(cache), nowTime, fullInterval, revision);
            });
        impl->lastScan = nowTime;
        impl->scanRequested = false;
    }
    catch (const std::exception& exception)
    {
        for (auto& entry : impl->surfaces)
        {
            const Impl::CheckResult result{
                entry.first, entry.second.status.generation, exception.what()};
            if (impl->Apply(result))
                changed.push_back(entry.second.status);
        }
    }
    return changed;
}

bool TerrainCompositedAuthorMonitor::MarkEdited(const std::string& surfaceId)
{
    return MarkEdited(surfaceId, TerrainCompositedDependencyChange::BaseMaterial);
}

bool TerrainCompositedAuthorMonitor::MarkEdited(
    const std::string& surfaceId, TerrainCompositedDependencyChange kind)
{
    ClassifyTerrainCompositedDependencyChange(kind);
    const auto found = impl->surfaces.find(surfaceId);
    if (found == impl->surfaces.end())
        return false;
    auto& status = found->second.status;
    ++status.editRevision;
    status.dirty = true;
    status.conflict = status.conflict || status.externalChange;
    if (std::find(status.localChanges.begin(), status.localChanges.end(), kind) == status.localChanges.end())
        status.localChanges.push_back(kind);
    return true;
}

bool TerrainCompositedAuthorMonitor::MarkSaved(const std::string& surfaceId)
{
    const auto found = impl->surfaces.find(surfaceId);
    if (found == impl->surfaces.end())
        return false;
    auto& status = found->second.status;
    status.dirty = false;
    status.localChanges.clear();
    status.checked = false;
    status.generation = ++impl->generation;
    impl->scanRequested = true;
    return true;
}

bool TerrainCompositedAuthorMonitor::MarkSaved(
    const std::string& surfaceId, TerrainCompositedDependencyChange kind,
    uint64_t editRevision, uint64_t generation)
{
    const auto found = impl->surfaces.find(surfaceId);
    if (found == impl->surfaces.end() ||
        found->second.status.editRevision != editRevision ||
        found->second.status.generation != generation)
        return false;
    auto& status = found->second.status;
    const auto change = std::find(status.localChanges.begin(), status.localChanges.end(), kind);
    if (change == status.localChanges.end())
        return false;
    status.localChanges.erase(change);
    status.dirty = !status.localChanges.empty();
    status.checked = false;
    status.generation = ++impl->generation;
    impl->scanRequested = true;
    return true;
}

bool TerrainCompositedAuthorMonitor::CanRecook(const std::string& surfaceId) const
{
    const auto found = impl->surfaces.find(surfaceId);
    if (found == impl->surfaces.end())
        return false;
    const auto& status = found->second.status;
    return status.checked && !status.dirty && !status.externalChange && !status.conflict;
}

bool TerrainCompositedAuthorMonitor::CheckBeforeRecook(
    const std::string& surfaceId, std::string& error)
{
    error.clear();
    const auto found = impl->surfaces.find(surfaceId);
    if (found == impl->surfaces.end())
    {
        error = "Terrain surface is not registered";
        return false;
    }
    auto& surface = found->second;
    if (surface.status.dirty || surface.status.externalChange || surface.status.conflict)
    {
        error = surface.status.dirty ?
            "Save or explicitly resolve unsaved terrain/material edits before Cook" :
            "Reload or resolve external terrain dependencies before Cook";
        return false;
    }
    surface.status.generation = ++impl->generation;
    ++impl->cacheRevision;
    auto batch = Impl::Scan({surface}, impl->cache, Clock::now(),
        impl->fullHashInterval, impl->cacheRevision, true);
    impl->ConsumeMetrics(batch, true);
    impl->Apply(batch.results.front());
    if (!CanRecook(surfaceId))
    {
        error = surface.status.diagnostic;
        return false;
    }
    return true;
}

bool TerrainCompositedAuthorMonitor::GetStatus(
    const std::string& surfaceId, SurfaceStatus& result) const
{
    const auto found = impl->surfaces.find(surfaceId);
    if (found == impl->surfaces.end())
        return false;
    result = found->second.status;
    return true;
}

TerrainCompositedAuthorMonitor::Statistics TerrainCompositedAuthorMonitor::Stats() const
{
    Statistics result;
    result.surfaces = impl->surfaces.size();
    result.completedChecks = impl->completedChecks;
    result.failedChecks = impl->failedChecks;
    result.discardedChecks = impl->discardedChecks;
    result.metadataChecks = impl->metadataChecks;
    result.metadataCacheHits = impl->metadataCacheHits;
    result.fullReadCount = impl->fullReadCount;
    result.fullReadBytes = impl->fullReadBytes;
    result.scanPending = impl->pending.valid();
    for (const auto& entry : impl->surfaces)
    {
        const auto& status = entry.second.status;
        result.dirtySurfaces += status.dirty ? 1 : 0;
        result.changedSurfaces += status.externalChange ? 1 : 0;
        result.conflictSurfaces += status.conflict ? 1 : 0;
        result.uncheckedSurfaces += status.checked ? 0 : 1;
    }
    return result;
}

TerrainCompositedAuthorMonitor::BaselineLoadSession::Statistics
TerrainCompositedAuthorMonitor::BaselineStats() const
{
    BaselineLoadSession::Statistics result;
    result.objectRequests = impl->baselineObjectRequests;
    result.requestedBytes = impl->baselineRequestedBytes;
    result.objectReads = impl->baselineObjectReads;
    result.readBytes = impl->baselineReadBytes;
    result.cacheHits = impl->baselineCacheHits;
    result.failedReads = impl->baselineFailedReads;
    result.bindings = impl->baselineBindings;
    return result;
}

}
