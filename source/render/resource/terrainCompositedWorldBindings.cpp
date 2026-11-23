#include "render/resource/terrainCompositedWorldBindings.h"

#include <limits>
#include <atomic>
#include <mutex>
#include <stdexcept>
#include <utility>

#include "render/resource/rendererResourceCache.h"
#include "texture.h"

namespace VL
{
struct WorldDescriptorReserveState
{
    std::optional<TerrainRvtBudgetLease> metadata;
    std::atomic<uint64_t> registryGpuBytes{0};
};

WorldDescriptorBudget PrepareWorldDescriptorBudget(WorldDescriptorBudget budget)
{
    if (!budget.owner || !budget.gpuMetadataReserve)
        throw std::runtime_error("World descriptor budget is incomplete");
    if (!budget.registry)
    {
        auto metadata = budget.owner->TryReserve(
            {0, sizeof(WorldDescriptorReserveState) + 256, 0, 0},
            TerrainRvtCostClass::Workspace, TerrainRvtResourcePhase::Candidate);
        if (!metadata)
            throw std::runtime_error("World descriptor reserve registry exceeds CPU workspace");
        budget.registry = std::make_shared<WorldDescriptorReserveState>();
        budget.registry->metadata = std::move(metadata);
    }
    return budget;
}

WorldBindingRegistryLease::~WorldBindingRegistryLease()
{
    if (budget.registry)
        budget.registry->registryGpuBytes.fetch_sub(gpuReserveBytes);
}

std::shared_ptr<const WorldBindingRegistryLease> PrepareWorldBindingRegistry(
    const DescriptorBindingSnapshot& snapshot, uint32_t imageCount, size_t bindingCount)
{
    if (!snapshot)
        throw std::invalid_argument("World descriptor registry requires a snapshot");
    const uint64_t cpuBytes = sizeof(WorldBindingRegistryLease) + 256 +
        imageCount * (sizeof(WorldBindingImageOwner) +
            sizeof(std::vector<vk::WriteDescriptorSet>) +
            bindingCount * (sizeof(vk::WriteDescriptorSet) + sizeof(vk::DescriptorImageInfo)));
    const uint64_t gpuBytes = imageCount * bindingCount * 256;
    std::optional<TerrainRvtBudgetLease> metadata;
    if (snapshot->budget.owner)
    {
        if (!snapshot->budget.registry || !snapshot->budget.gpuMetadataReserve)
            throw std::runtime_error("World descriptor registry has an incomplete admitted budget");
        if (!snapshot->budget.registry ||
            snapshot->budget.gpuMetadataReserve->Cost().gpuBytes < gpuBytes)
            throw std::runtime_error("World Set3 registry exceeds P0 GPU descriptor reserve");
        metadata = snapshot->budget.owner->TryReserve({0, cpuBytes, 0, 0},
            TerrainRvtCostClass::Workspace, TerrainRvtResourcePhase::Candidate);
        if (!metadata)
            throw WorldBindingDescriptorRetry(WorldDescriptorRetryCause::CpuAdmission);
    }
    auto result = std::make_shared<WorldBindingRegistryLease>();
    result->metadata = std::move(metadata);
    result->budget = snapshot->budget;
    result->worldGeneration = snapshot->worldGeneration;
    result->cpuBytes = cpuBytes;
    if (result->budget.registry)
    {
        auto& used = result->budget.registry->registryGpuBytes;
        auto previous = used.load();
        const auto limit = result->budget.gpuMetadataReserve->Cost().gpuBytes;
        do
        {
            if (previous > limit || gpuBytes > limit - previous)
                throw WorldBindingDescriptorRetry(WorldDescriptorRetryCause::SharedMetadataAdmission);
        } while (!used.compare_exchange_weak(previous, previous + gpuBytes));
    }
    result->gpuReserveBytes = gpuBytes;
    return result;
}

DescriptorBindingSnapshot PrepareDescriptorBindingVersion(
    uint64_t generation, uint64_t revision, const WorldTextureMap& textures,
    const WorldDescriptorBudget& budget)
{
    uint64_t bytes = sizeof(DescriptorBindingVersion) + 256;
    for (const auto& entry : textures)
    {
        if (entry.first.empty() || !entry.second)
            throw std::runtime_error("Invalid World descriptor texture");
        bytes += 512 + entry.first.size() * 3;
    }
    const uint64_t gpuBytes = textures.size() * 256;
    std::optional<TerrainRvtBudgetLease> lease;
    if (budget.owner)
    {
        if (!budget.gpuMetadataReserve ||
            budget.gpuMetadataReserve->Cost().gpuBytes < gpuBytes)
            throw std::runtime_error("World descriptors require the admitted P0 GPU metadata reserve");
        lease = budget.owner->TryReserve({0, bytes, 0, 0},
            TerrainRvtCostClass::Workspace, TerrainRvtResourcePhase::Candidate);
        if (!lease)
            throw std::runtime_error("World descriptor metadata exceeds frozen workspace");
    }
    auto result = std::make_shared<DescriptorBindingVersion>();
    result->metadata = std::move(lease);
    result->budget = budget;
    result->worldGeneration = generation;
    result->revision = revision;
    result->metadataCpuBytes = bytes;
    result->descriptorGpuReserveBytes = gpuBytes;
    result->textures = textures;
    result->descriptors.reserve(textures.size());
    for (const auto& entry : textures)
    {
        const auto descriptor = entry.second->GetDescriptorInfo();
        if (!descriptor.imageView || !descriptor.sampler ||
            descriptor.imageLayout == vk::ImageLayout::eUndefined)
            throw std::runtime_error("World descriptor texture is not prepared: " + entry.first);
        result->descriptors.emplace(entry.first, descriptor);
    }
    return result;
}

struct PreparedWorldTextureBindings::Impl
{
    std::optional<TerrainRvtBudgetLease> metadata;
    RendererResourceCache* cache = nullptr;
    RendererResourceCache::ImmutableWorldLocalResourceRefs expectedWorld;
    DescriptorBindingSnapshot expected;
    DescriptorBindingSnapshot prepared;
    DescriptorBindingSnapshot published;
    WorldTextureMap replacement;
    RendererResourceCache::RecursiveLock lock;
    bool finished = false;
};

PreparedWorldTextureBindings::PreparedWorldTextureBindings(std::unique_ptr<Impl> implementation)
    : impl(std::move(implementation)) {}
PreparedWorldTextureBindings::~PreparedWorldTextureBindings() { CancelNoThrow(); }
PreparedWorldTextureBindings::PreparedWorldTextureBindings(PreparedWorldTextureBindings&&) noexcept = default;
PreparedWorldTextureBindings& PreparedWorldTextureBindings::operator=(PreparedWorldTextureBindings&&) noexcept = default;

PreparedWorldTextureBindings RendererResourceCache::PrepareWorldTextureBindings(
    uint64_t generation, const DescriptorBindingSnapshot& oldIdentity,
    const WorldTextureMap& replacements, const WorldDescriptorBudget& budget)
{
    RecursiveLock guard(mutex, std::try_to_lock);
    if (!guard.owns_lock())
        throw std::runtime_error("World descriptor preparation cache is busy");
    if (!oldIdentity || !worldLocalResources ||
        worldLocalResources->ownerGeneration != generation ||
        oldIdentity->worldGeneration != generation ||
        worldLocalResources->descriptorBindings != oldIdentity ||
        worldLocalResources->worldTextureRevision != oldIdentity->revision ||
        oldIdentity->revision == std::numeric_limits<uint64_t>::max() ||
        replacements.empty() || !budget.owner ||
        oldIdentity->budget.owner != budget.owner ||
        oldIdentity->budget.gpuMetadataReserve != budget.gpuMetadataReserve)
        throw std::runtime_error("Stale World descriptor preparation identity/budget");
    uint64_t bytes = sizeof(PreparedWorldTextureBindings::Impl) + 256;
    for (const auto& entry : worldLocalResources->worldTextures)
        bytes += 256 + entry.first.size();
    auto metadata = budget.owner->TryReserve({0, bytes, 0, 0},
        TerrainRvtCostClass::Workspace, TerrainRvtResourcePhase::Candidate);
    if (!metadata)
        throw std::runtime_error("World binding preparation exceeds frozen workspace");
    auto prepared = std::make_unique<PreparedWorldTextureBindings::Impl>();
    prepared->metadata = std::move(metadata);
    prepared->cache = this;
    prepared->expectedWorld = worldLocalResources;
    prepared->expected = oldIdentity;
    prepared->replacement = worldLocalResources->worldTextures;
    for (const auto& entry : replacements)
    {
        auto found = prepared->replacement.find(entry.first);
        if (found == prepared->replacement.end() || !entry.second)
            throw std::runtime_error("Unknown/null World texture replacement: " + entry.first);
        found->second = entry.second;
    }
    prepared->prepared = PrepareDescriptorBindingVersion(generation,
        oldIdentity->revision + 1, prepared->replacement, oldIdentity->budget);
    prepared->published = prepared->prepared;
    return PreparedWorldTextureBindings(std::move(prepared));
}

bool PreparedWorldTextureBindings::TryLockNoThrow() noexcept
{
    if (!impl || impl->finished || impl->lock.owns_lock())
        return false;
    try
    {
        RendererResourceCache::RecursiveLock acquired(impl->cache->mutex, std::try_to_lock);
        if (!acquired.owns_lock() ||
            impl->cache->worldLocalResources != impl->expectedWorld ||
            impl->cache->worldLocalResources->descriptorBindings != impl->expected ||
            impl->cache->worldLocalResources->worldTextureRevision != impl->expected->revision)
            return false;
        impl->lock = std::move(acquired);
        return true;
    }
    catch (...) { return false; }
}

void PreparedWorldTextureBindings::CommitNoThrow() noexcept
{
    if (!impl || impl->finished || !impl->lock.owns_lock())
        return;
    auto& world = *impl->cache->worldLocalResources;
    world.worldTextures.swap(impl->replacement);
    world.descriptorBindings.swap(impl->prepared);
    world.worldTextureRevision = world.descriptorBindings->revision;
    impl->finished = true;
    impl->lock.unlock();
}

void PreparedWorldTextureBindings::CancelNoThrow() noexcept
{
    if (!impl)
        return;
    if (impl->lock.owns_lock())
        impl->lock.unlock();
    impl.reset();
}

DescriptorBindingSnapshot PreparedWorldTextureBindings::Snapshot() const noexcept
{
    return impl ? impl->published : nullptr;
}
}
