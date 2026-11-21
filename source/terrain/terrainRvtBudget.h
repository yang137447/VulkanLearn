#pragma once

#include <array>
#include <cstdint>
#include <cstddef>
#include <memory>
#include <optional>
#include <vector>
#if defined(VL_FILE_IO_TEST_HOOKS)
#include <functional>
#endif

namespace VL
{
enum class TerrainRvtCostClass { PhysicalPool, Input, Workspace, Count };
enum class TerrainRvtResourcePhase { Candidate, Active, Retiring, Count };

struct TerrainRvtCost
{
    uint64_t gpuBytes = 0;
    uint64_t cpuBytes = 0;
    uint64_t physicalSlots = 0;
    uint64_t productionTasks = 0;

    bool operator==(const TerrainRvtCost& other) const;
};

struct TerrainRvtBudgetCaps
{
    uint64_t physicalPoolGpuBytes = 0;
    uint64_t inputGpuBytes = 0;
    uint64_t workspaceGpuBytes = 0;
    uint64_t totalGpuBytes = 0;
    uint64_t totalCpuBytes = 0;
    uint64_t physicalSlots = 0;
    uint64_t productionTasks = 0;
    uint64_t physicalPoolCpuBytes = 0;
    uint64_t inputCpuBytes = 0;
    uint64_t workspaceCpuBytes = 0;
};

struct TerrainRvtBudgetStats
{
    TerrainRvtCost current;
    TerrainRvtCost peak;
    std::array<TerrainRvtCost, 3> byClass{};
    std::array<TerrainRvtCost, 3> byPhase{};
    uint64_t allocations = 0;
    uint64_t rejected = 0;
    uint64_t metadataBytes = 0;
    uint64_t peakMetadataBytes = 0;
};

struct TerrainRvtBudgetState;
struct TerrainRvtBudgetCharge;
class TerrainRvtBudgetOwner;
class TerrainRvtBudgetPhaseBatch;

// 租约由实际资源和在途任务共享持有，最后一个引用释放才返还跨 World 成本。
class TerrainRvtBudgetLease
{
public:
    TerrainRvtCost Cost() const;
    TerrainRvtResourcePhase Phase() const;
    void SetPhase(TerrainRvtResourcePhase phase);

private:
    friend class TerrainRvtBudgetOwner;
    friend struct TerrainRvtBudgetCharge;
    friend class TerrainRvtBudgetPhaseBatch;
    explicit TerrainRvtBudgetLease(std::shared_ptr<TerrainRvtBudgetCharge> charge);
    std::shared_ptr<TerrainRvtBudgetCharge> charge;
};

struct TerrainRvtBudgetPhaseChange
{
    TerrainRvtBudgetLease lease;
    TerrainRvtResourcePhase phase;
};

class TerrainRvtBudgetPhaseBatch
{
public:
    static constexpr std::size_t MaxCharges = 4096;
    static std::optional<TerrainRvtBudgetPhaseBatch> Prepare(
        const std::vector<TerrainRvtBudgetPhaseChange>& changes,
        TerrainRvtBudgetOwner& metadataOwner);
    ~TerrainRvtBudgetPhaseBatch();
    TerrainRvtBudgetPhaseBatch(TerrainRvtBudgetPhaseBatch&&) noexcept;
    TerrainRvtBudgetPhaseBatch& operator=(TerrainRvtBudgetPhaseBatch&&) noexcept;
    TerrainRvtBudgetPhaseBatch(const TerrainRvtBudgetPhaseBatch&) = delete;
    TerrainRvtBudgetPhaseBatch& operator=(const TerrainRvtBudgetPhaseBatch&) = delete;
    bool TryCommitNoThrow() noexcept;
    uint64_t MetadataBytes() const noexcept;
    std::size_t ChargeCount() const noexcept;

private:
    struct Impl;
    explicit TerrainRvtBudgetPhaseBatch(std::unique_ptr<Impl> implementation);
    std::unique_ptr<Impl> impl;
};

// Renderer 持有总账；候选、活动 World、输入快照和 GPU 退休资源使用同一实例。
// 物理池一次计入全部槽，root/reserved/retiring 均不可从已分配容量中扣除。
class TerrainRvtBudgetOwner
{
public:
    static constexpr uint64_t MetadataBytesPerLease = 256;
    static constexpr uint64_t MaxRegistryEntries = 4096;

    explicit TerrainRvtBudgetOwner(TerrainRvtBudgetCaps frozenCaps,
        std::shared_ptr<TerrainRvtBudgetOwner> parent = {},
        std::shared_ptr<TerrainRvtBudgetOwner> secondParent = {});
    TerrainRvtBudgetOwner(const TerrainRvtBudgetOwner&) = delete;
    TerrainRvtBudgetOwner& operator=(const TerrainRvtBudgetOwner&) = delete;

    std::optional<TerrainRvtBudgetLease> TryReserve(
        TerrainRvtCost cost, TerrainRvtCostClass costClass,
        TerrainRvtResourcePhase phase);
    TerrainRvtBudgetStats Statistics() const;
    TerrainRvtBudgetCaps Caps() const;
    std::shared_ptr<const void> IdentityToken() const;
    void SetPhase(TerrainRvtResourcePhase phase);

private:
    friend struct TerrainRvtBudgetTestAccess;
    std::shared_ptr<TerrainRvtBudgetState> state;
    std::shared_ptr<TerrainRvtBudgetOwner> parent;
    std::shared_ptr<TerrainRvtBudgetOwner> secondParent;
};
#if defined(VL_FILE_IO_TEST_HOOKS)
struct TerrainRvtBudgetTestAccess
{
    static void WithStateLock(const TerrainRvtBudgetOwner& owner,
        const std::function<void()>& callback);
    static void FailTryLockAfter(std::size_t acquired);
};
#endif
}
