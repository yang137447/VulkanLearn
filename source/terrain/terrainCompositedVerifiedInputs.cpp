#include "terrain/terrainCompositedVerifiedInputs.h"

#include "terrain/terrainCompositedVTStore.h"
#include "terrain/terrainRvtBudget.h"

#include <algorithm>
#include <chrono>
#include <limits>
#include <mutex>
#include <stdexcept>
#include <utility>

namespace VL
{
namespace
{
uint64_t Now()
{
    return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count());
}

void Count(uint64_t& target, uint64_t amount)
{
    target += std::min(amount, std::numeric_limits<uint64_t>::max() - target);
}

TerrainRvtBudgetLease Reserve(TerrainRvtBudgetOwner& owner, uint64_t bytes)
{
    auto lease = owner.TryReserve({0, bytes, 0, 0},
        TerrainRvtCostClass::Workspace, TerrainRvtResourcePhase::Active);
    if (!lease)
        throw std::runtime_error("Verified input CPU reservation rejected before allocation");
    return std::move(*lease);
}

uint64_t IdentityScratchBytes(const TerrainCompositedProductionClosure& closure)
{
    uint64_t bytes = 128 * 1024;
    for (const auto& object : closure.objects)
        bytes += 512 + object.objectId.capacity() * 4 + object.relativePath.capacity() * 4;
    for (const auto& surface : closure.surfaces)
    {
        bytes += sizeof(surface) * 4 + surface.surfaceId.capacity() * 4;
        for (const auto& dependency : surface.authorDependencies)
        {
            bytes += sizeof(dependency) * 4 + 512 +
                (dependency.root.capacity() + dependency.path.capacity() +
                    dependency.mode.capacity() + dependency.digest.capacity()) * 4;
            for (const auto& id : dependency.productionObjectIds)
                bytes += sizeof(id) * 4 + id.capacity() * 4 + 256;
        }
    }
    for (const auto& id : closure.generator.includeSources)
        bytes += sizeof(id) * 4 + id.capacity() * 4 + 256;
    for (const auto& id : closure.generator.compiledArtifacts)
        bytes += sizeof(id) * 4 + id.capacity() * 4 + 256;
    for (const auto& link : closure.adjacency.Links())
        bytes += sizeof(link) * 4 +
            (link.sourceSurfaceId.capacity() + link.targetSurfaceId.capacity()) * 4 + 512;
    return bytes;
}

struct RetainedInput
{
    TerrainRvtBudgetLease charge;
    std::vector<uint8_t> bytes;

    RetainedInput(TerrainRvtBudgetLease lease, std::vector<uint8_t>&& input)
        : charge(std::move(lease)), bytes(std::move(input)) {}
};
}

struct TerrainCompositedVerifiedInputs::Impl
{
    struct Entry
    {
        bool verified = false;
        Bytes retained;
    };

    std::shared_ptr<const TerrainCompositedVTStore> store;
    std::shared_ptr<TerrainRvtBudgetOwner> owner;
    TerrainRvtBudgetLease ownerMetadataCharge;
    std::shared_ptr<TerrainRvtBudgetOwner> localOwner;
    uint64_t generation = 0;
    ContentDigest production;
    TerrainRvtBudgetLease metadataCharge;
    std::vector<Entry> entries;
    std::vector<uint8_t> scopes;
    std::vector<uint64_t> references;
    mutable std::mutex mutex;
    TerrainCompositedVerifiedInputStatistics stats;

    Impl(std::shared_ptr<const TerrainCompositedVTStore> openedStore,
        uint64_t worldGeneration, ContentDigest identity,
        std::shared_ptr<TerrainRvtBudgetOwner> budget,
        TerrainRvtBudgetLease ownerMetadata,
        std::shared_ptr<TerrainRvtBudgetOwner> localBudget,
        TerrainRvtBudgetLease metadata, uint64_t metadataBytes)
        : store(std::move(openedStore)), owner(std::move(budget)),
          ownerMetadataCharge(std::move(ownerMetadata)),
          localOwner(std::move(localBudget)), generation(worldGeneration),
          production(identity), metadataCharge(std::move(metadata)),
          entries(store->Production().objects.size()), scopes(store->Production().surfaces.size(), 0),
          references(scopes.size() * ((entries.size() + 63) / 64), 0)
    {
        stats.beginNanoseconds = Now();
        stats.metadataBytes = metadataBytes;
    }

    bool Registered(std::size_t scope, std::size_t object) const
    {
        const auto word = scope * ((entries.size() + 63) / 64) + object / 64;
        return (references[word] & (uint64_t{1} << (object % 64))) != 0;
    }

    void Register(std::size_t scope, std::size_t object)
    {
        const auto word = scope * ((entries.size() + 63) / 64) + object / 64;
        references[word] |= uint64_t{1} << (object % 64);
        scopes[scope] = 1;
    }

    std::size_t Scope(const Binding& binding) const
    {
        if (!binding.store || binding.store.get() != store.get() ||
            binding.worldGeneration != generation || binding.productionIdentity != production)
            throw std::invalid_argument("Verified input Store/World/publication mismatch");
        store->Surface(binding.surfaceId);
        const auto& surfaces = store->Production().surfaces;
        for (std::size_t index = 0; index < surfaces.size(); ++index)
            if (surfaces[index].surfaceId == binding.surfaceId)
                return index;
        throw std::invalid_argument("Verified input unknown Production surface");
    }

    std::size_t Object(std::size_t scope, const std::string& objectId) const
    {
        const auto& dependencies = store->Production().surfaces[scope].authorDependencies;
        const bool bound = std::any_of(dependencies.begin(), dependencies.end(),
            [&](const TerrainCompositedAuthorDependency& dependency)
            {
                return std::find(dependency.productionObjectIds.begin(),
                    dependency.productionObjectIds.end(), objectId) !=
                    dependency.productionObjectIds.end();
            });
        if (!bound)
            throw std::invalid_argument("Verified input is not a surface author dependency");
        const auto& object = store->Production().Object(objectId);
        return static_cast<std::size_t>(&object - store->Production().objects.data());
    }
};

std::shared_ptr<TerrainCompositedVerifiedInputs> TerrainCompositedVerifiedInputs::Begin(
    std::shared_ptr<const TerrainCompositedVTStore> openedStore,
    uint64_t worldGeneration, ContentDigest productionIdentity,
    std::shared_ptr<TerrainRvtBudgetOwner> cpuOwner, std::size_t maxObjects)
{
    if (!openedStore || !cpuOwner || maxObjects == 0 || maxObjects > MaxObjects ||
        openedStore->Production().objects.size() > maxObjects ||
        openedStore->Production().surfaces.size() > 256)
        throw std::invalid_argument("Invalid verified input batch identity/owner/bound");
    // 独立 512 MiB 上限并 parent 到调用者总账；跨 World 的 geometry/runtime 成本不可绕过。
    TerrainRvtBudgetCaps caps;
    caps.totalCpuBytes = MaxCpuBytes;
    caps.workspaceCpuBytes = MaxCpuBytes;
    const uint64_t metadataBytes = sizeof(Impl) + sizeof(TerrainCompositedVerifiedInputs) +
        openedStore->Production().objects.size() * sizeof(Impl::Entry) +
        openedStore->Production().surfaces.size() *
            (1 + ((openedStore->Production().objects.size() + 63) / 64) * sizeof(uint64_t)) + 512;
    auto parentMetadata = Reserve(*cpuOwner,
        sizeof(TerrainRvtBudgetOwner) + TerrainRvtBudgetOwner::MetadataBytesPerLease);
    auto localOwner = std::make_shared<TerrainRvtBudgetOwner>(caps, cpuOwner);
    {
        // ContentHash/Validate 的排序副本与集合也先收费，不能把批次 metadata 隐藏在峰值外。
        auto identityScratch = Reserve(*localOwner, IdentityScratchBytes(openedStore->Production()));
        if (openedStore->Production().ContentHash() != productionIdentity)
            throw std::invalid_argument("Invalid verified input publication identity");
    }
    auto metadata = Reserve(*localOwner, metadataBytes);
    auto implementation = std::make_unique<Impl>(std::move(openedStore),
        worldGeneration, productionIdentity, std::move(cpuOwner),
        std::move(parentMetadata),
        std::move(localOwner), std::move(metadata), metadataBytes);
    return std::shared_ptr<TerrainCompositedVerifiedInputs>(
        new TerrainCompositedVerifiedInputs(std::move(implementation)));
}

TerrainCompositedVerifiedInputs::TerrainCompositedVerifiedInputs(
    std::unique_ptr<Impl> implementation) : impl(std::move(implementation)) {}

TerrainCompositedVerifiedInputs::~TerrainCompositedVerifiedInputs() = default;

TerrainCompositedVerifiedInputs::Bytes TerrainCompositedVerifiedInputs::Request(
    const Binding& binding, const std::string& objectId, bool retain)
{
    std::lock_guard<std::mutex> lock(impl->mutex);
    Count(impl->stats.requests, 1);
    try
    {
        const auto scope = impl->Scope(binding);
        const auto index = impl->Object(scope, objectId);
        const auto& object = impl->store->Production().objects[index];
        Count(impl->stats.requestedBytes, object.bytes);
        auto& entry = impl->entries[index];
        if (impl->stats.closed)
        {
            // 关闭后只消费初始化批次实际登记过的引用；不能借其他 surface 的 proof 开新 scope/ref。
            if (!retain && entry.verified && impl->Registered(scope, index))
            {
                Count(impl->stats.hits, 1);
                return {};
            }
            throw std::runtime_error("Verified input batch is closed to new references/payload");
        }
        if (entry.verified)
        {
            if (retain && !entry.retained)
                throw std::runtime_error("Proof-only input has no retained immutable payload");
            impl->Register(scope, index);
            Count(impl->stats.hits, 1);
            return entry.retained;
        }
        if (object.bytes > std::numeric_limits<std::size_t>::max() ||
            object.bytes > MaxCpuBytes - 512)
            throw std::runtime_error("Verified input exceeds bounded CPU capacity");
        // Store.ReadInput 内部分配 whole vector 之前预留；串行读限制临时峰值为一个对象。
        auto scratch = Reserve(*impl->localOwner, ReadScratchBytes);
        auto charge = Reserve(*impl->localOwner, object.bytes + sizeof(RetainedInput) + 256);
        Count(impl->stats.readCalls, 1);
        Count(impl->stats.readAttemptBytes, object.bytes);
        auto bytes = impl->store->ReadInput(objectId);
        Count(impl->stats.actualReadBytes, bytes.size());
        // ReadInput 已用对象 digest 做 rooted-handle hash/payload 验证；不再 hash 整个大对象。
        if (bytes.size() != object.bytes || bytes.capacity() > object.bytes)
            throw std::runtime_error("Verified input vector size/capacity mismatch");
        Bytes snapshot;
        if (retain)
        {
            auto owned = std::make_shared<RetainedInput>(std::move(charge), std::move(bytes));
            snapshot = Bytes(owned, &owned->bytes);
        }
        // 仅在读取、hash 与 snapshot allocation 全部成功后发布 proof/scope；失败不缓存。
        entry.retained = snapshot;
        entry.verified = true;
        impl->Register(scope, index);
        Count(impl->stats.verifiedObjects, 1);
        Count(impl->stats.validatedBytes, object.bytes);
        if (retain)
            Count(impl->stats.retainedBytes, object.bytes);
        return snapshot;
    }
    catch (...)
    {
        Count(impl->stats.failures, 1);
        throw;
    }
}

void TerrainCompositedVerifiedInputs::VerifyObject(
    const Binding& binding, const std::string& objectId)
{
    Request(binding, objectId, false);
}

TerrainCompositedVerifiedInputs::Bytes TerrainCompositedVerifiedInputs::ReadVerified(
    const Binding& binding, const std::string& objectId)
{
    return Request(binding, objectId, true);
}

bool TerrainCompositedVerifiedInputs::HasVerified(
    const Binding& binding, const std::string& objectId) const
{
    std::lock_guard<std::mutex> lock(impl->mutex);
    try
    {
        const auto scope = impl->Scope(binding);
        const auto index = impl->Object(scope, objectId);
        return impl->Registered(scope, index) && impl->entries[index].verified;
    }
    catch (const std::exception&)
    {
        return false;
    }
}

bool TerrainCompositedVerifiedInputs::Matches(const Binding& binding) const
{
    std::lock_guard<std::mutex> lock(impl->mutex);
    try
    {
        const auto scope = impl->Scope(binding);
        return !impl->stats.closed || impl->scopes[scope] != 0;
    }
    catch (const std::exception&)
    {
        return false;
    }
}

void TerrainCompositedVerifiedInputs::Close()
{
    std::lock_guard<std::mutex> lock(impl->mutex);
    if (impl->stats.closed)
        return;
    impl->stats.closed = true;
    impl->stats.endNanoseconds = Now();
    for (auto& entry : impl->entries)
        entry.retained.reset();
    impl->stats.retainedBytes = 0;
}

TerrainCompositedVerifiedInputStatistics TerrainCompositedVerifiedInputs::Statistics() const
{
    std::lock_guard<std::mutex> lock(impl->mutex);
    return impl->stats;
}
}
