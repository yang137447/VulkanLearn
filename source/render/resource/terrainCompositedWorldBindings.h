#pragma once

#include <cstdint>
#include <exception>
#include <new>
#include <memory>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>
#include <vulkan/vulkan.hpp>

#include "terrain/terrainRvtBudget.h"

class Texture;

namespace VL
{
class RendererResourceCache;

enum class WorldDescriptorRetryCause { HostAllocation, VulkanAllocation, CpuAdmission, SharedMetadataAdmission };

class WorldBindingDescriptorRetry final : public std::exception
{
public:
    explicit WorldBindingDescriptorRetry(
        WorldDescriptorRetryCause cause = WorldDescriptorRetryCause::HostAllocation, int32_t result = 0)
        : cause(cause), originalResult(result) {}
    const char* what() const noexcept override { return "World descriptor image update requires retry"; }
    WorldBindingDescriptorRetry WithContext(uint64_t world, uint64_t revision,
        uint32_t image, const char* operation) const
    {
        auto copy = *this;
        copy.worldGeneration = world;
        copy.bindingRevision = revision;
        copy.imageIndex = image;
        copy.operation = operation;
        return copy;
    }
    WorldDescriptorRetryCause cause;
    int32_t originalResult = 0;
    uint64_t worldGeneration = 0;
    uint64_t bindingRevision = 0;
    uint32_t imageIndex = 0;
    const char* operation = "unbound";
};

// 只转换可重试资源压力；DeviceLost、结构错误和未知异常保留原始类型并终止。
template<class Action>
void RunWorldDescriptorOperation(uint64_t world, uint64_t revision,
    uint32_t image, const char* operation, Action&& action)
{
    try { action(); }
    catch (const WorldBindingDescriptorRetry& error)
    {
        throw error.WithContext(world, revision, image, operation);
    }
    catch (const std::bad_alloc&)
    {
        throw WorldBindingDescriptorRetry().WithContext(world, revision, image, operation);
    }
    catch (const vk::SystemError& error)
    {
        const auto result = static_cast<vk::Result>(error.code().value());
        if (result != vk::Result::eErrorOutOfHostMemory && result != vk::Result::eErrorOutOfDeviceMemory &&
            result != vk::Result::eErrorOutOfPoolMemory && result != vk::Result::eErrorFragmentedPool)
            throw;
        throw WorldBindingDescriptorRetry(WorldDescriptorRetryCause::VulkanAllocation,
            static_cast<int32_t>(result)).WithContext(world, revision, image, operation);
    }
}

using WorldTextureMap = std::unordered_map<std::string, std::shared_ptr<Texture>>;

struct WorldDescriptorReserveState;
struct WorldDescriptorBudget
{
    std::shared_ptr<TerrainRvtBudgetOwner> owner;
    std::shared_ptr<const TerrainRvtBudgetLease> gpuMetadataReserve;
    std::shared_ptr<WorldDescriptorReserveState> registry;
};

WorldDescriptorBudget PrepareWorldDescriptorBudget(WorldDescriptorBudget budget);

struct DescriptorBindingVersion
{
    std::optional<TerrainRvtBudgetLease> metadata;
    WorldDescriptorBudget budget;
    uint64_t worldGeneration = 0;
    uint64_t revision = 0;
    WorldTextureMap textures;
    std::unordered_map<std::string, vk::DescriptorImageInfo> descriptors;
    uint64_t metadataCpuBytes = 0;
    uint64_t descriptorGpuReserveBytes = 0;
};

using DescriptorBindingSnapshot = std::shared_ptr<const DescriptorBindingVersion>;

struct WorldBindingRegistryLease
{
    ~WorldBindingRegistryLease();
    std::optional<TerrainRvtBudgetLease> metadata;
    WorldDescriptorBudget budget;
    uint64_t worldGeneration = 0;
    uint64_t cpuBytes = 0;
    uint64_t gpuReserveBytes = 0;
};

std::shared_ptr<const WorldBindingRegistryLease> PrepareWorldBindingRegistry(
    const DescriptorBindingSnapshot& snapshot, uint32_t imageCount, size_t bindingCount);

DescriptorBindingSnapshot PrepareDescriptorBindingVersion(
    uint64_t generation, uint64_t revision, const WorldTextureMap& textures,
    const WorldDescriptorBudget& budget);

class PreparedWorldTextureBindings
{
public:
    ~PreparedWorldTextureBindings();
    PreparedWorldTextureBindings(PreparedWorldTextureBindings&&) noexcept;
    PreparedWorldTextureBindings& operator=(PreparedWorldTextureBindings&&) noexcept;
    PreparedWorldTextureBindings(const PreparedWorldTextureBindings&) = delete;
    PreparedWorldTextureBindings& operator=(const PreparedWorldTextureBindings&) = delete;
    bool TryLockNoThrow() noexcept;
    void CommitNoThrow() noexcept;
    void CancelNoThrow() noexcept;
    DescriptorBindingSnapshot Snapshot() const noexcept;

private:
    friend class RendererResourceCache;
    struct Impl;
    explicit PreparedWorldTextureBindings(std::unique_ptr<Impl> implementation);
    std::unique_ptr<Impl> impl;
};

class WorldBindingImageOwner
{
public:
    const DescriptorBindingSnapshot& Snapshot() const noexcept { return snapshot; }
    void InstallAfterFence(DescriptorBindingSnapshot next,
        std::shared_ptr<const WorldBindingRegistryLease> nextRegistry = {}) noexcept
    {
        snapshot.swap(next);
        registry.swap(nextRegistry);
    }

private:
    std::shared_ptr<const WorldBindingRegistryLease> registry;
    DescriptorBindingSnapshot snapshot;
};
}
