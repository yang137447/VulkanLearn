#pragma once

#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <vector>

namespace VL
{
struct TerrainRvtSurfaceIdentity
{
    uint64_t worldGeneration = 0;
    uint64_t revision = 0;
    std::string snapshotIdentity;
    std::string contentIdentity;
    std::string surfaceId;

    bool operator==(const TerrainRvtSurfaceIdentity& other) const;
    bool operator<(const TerrainRvtSurfaceIdentity& other) const;
};

struct TerrainRvtPageIdentity
{
    TerrainRvtSurfaceIdentity surface;
    uint32_t mip = 0;
    uint32_t x = 0;
    uint32_t y = 0;

    bool operator==(const TerrainRvtPageIdentity& other) const;
    bool operator<(const TerrainRvtPageIdentity& other) const;
};

enum class TerrainRvtSlotState { Free, Reserved, Resident, Retiring };
enum class TerrainRvtTaskState
{
    Absent, Queued, Producing, AwaitingFence, ReadyToPublish,
    Resident, FailedBackoff, Failed, Stale
};
enum class TerrainRvtProductionResult { Success, RetryableFailure, PermanentFailure };

struct TerrainRvtSchedulerConfig
{
    uint32_t physicalSlots = 0;
    uint32_t productionSlots = 0;
    uint32_t maxTrackedPages = 0;
    uint32_t maxAttempts = 0;
    uint64_t retryBaseTicks = 0;
    uint64_t retryMaxTicks = 0;
    uint64_t agePromotionTicks = 0;
    uint32_t freeSlotReserve = 0;
};

struct TerrainRvtProductionTask
{
    uint64_t serial = 0;
    TerrainRvtPageIdentity page;
    uint32_t physicalSlot = 0;
    uint32_t productionSlot = 0;
};

struct TerrainRvtSchedulerStats
{
    uint32_t free = 0;
    uint32_t reserved = 0;
    uint32_t resident = 0;
    uint32_t retiring = 0;
    uint32_t roots = 0;
    uint32_t productionInUse = 0;
    uint32_t tracked = 0;
    uint64_t rejected = 0;
    uint64_t staleResults = 0;
};

// Runtime 串行调用本状态机；GPU 资源、冻结输入和预算租约仍由 Runtime 持有。
// slot 在 status/fence 成功后仍为 Reserved，直到单独页表提交才转 Resident。
// 页地址和 root/祖先分类来自已校验布局，不在这里推导 NPOT 父页。
// Publish/CommitEviction 仅在对应页表提交成功后调用；epoch 取 backend 安全退休值。
class TerrainRvtScheduler
{
public:
    explicit TerrainRvtScheduler(TerrainRvtSchedulerConfig frozenConfig);
    void SetSurfaceIdentity(const TerrainRvtSurfaceIdentity& identity);
    bool Request(const TerrainRvtPageIdentity& page, bool root,
        bool coarseAncestor, uint64_t now);
    std::optional<TerrainRvtProductionTask> Begin(uint64_t now, uint64_t completedEpoch);
    bool Submitted(const TerrainRvtProductionTask& task, uint64_t epoch);
    bool Complete(const TerrainRvtProductionTask& task, uint64_t completedEpoch,
        TerrainRvtProductionResult result, uint64_t now);
    bool AbortUnsubmitted(const TerrainRvtProductionTask& task, uint64_t now);
    bool CancelRequest(const TerrainRvtPageIdentity& page);
    bool Publish(const TerrainRvtProductionTask& task, uint64_t pageTableEpoch);
    void Collect(uint64_t completedEpoch);
    bool Touch(const TerrainRvtPageIdentity& page, uint64_t now);
    std::optional<uint32_t> EvictionCandidate() const;
    bool CommitEviction(uint32_t slot, uint64_t unmapEpoch);
    bool RetireReplacedIdentity(const TerrainRvtSurfaceIdentity& identity, uint64_t unmapEpoch);
    void RetireAll(uint64_t unmapEpoch);
    TerrainRvtSchedulerStats Statistics() const;
    std::optional<TerrainRvtTaskState> State(const TerrainRvtPageIdentity& page) const;
    TerrainRvtTaskState Status(const TerrainRvtPageIdentity& page) const;
    TerrainRvtSlotState SlotState(uint32_t slot) const;

private:
    struct Entry
    {
        TerrainRvtTaskState state = TerrainRvtTaskState::Queued;
        bool root = false;
        bool coarseAncestor = false;
        uint64_t queuedAt = 0;
        uint64_t retryAt = 0;
        uint64_t lastUsed = 0;
        uint32_t attempts = 0;
        uint64_t submittedEpoch = 0;
        uint64_t publishedEpoch = 0;
        std::optional<TerrainRvtProductionTask> task;
    };
    struct Slot
    {
        TerrainRvtSlotState state = TerrainRvtSlotState::Free;
        std::optional<TerrainRvtPageIdentity> page;
        uint64_t retireEpoch = 0;
    };
    using Entries = std::map<TerrainRvtPageIdentity, Entry>;
    bool Current(const TerrainRvtSurfaceIdentity& identity) const;
    Entries::iterator Find(const TerrainRvtProductionTask& task);
    bool HasRoot(const TerrainRvtSurfaceIdentity& identity) const;
    void ReleaseTask(Entry& entry);
    void Fail(Entry& entry, TerrainRvtProductionResult result, uint64_t now);
    void Cancel(Entry& entry);
    uint64_t Rank(const Entry& entry, uint64_t now) const;

    TerrainRvtSchedulerConfig config;
    Entries entries;
    std::map<std::string, TerrainRvtSurfaceIdentity> identities;
    std::vector<Slot> slots;
    std::vector<bool> production;
    uint64_t serial = 0;
    uint64_t completed = 0;
    uint64_t rejected = 0;
    uint64_t staleResults = 0;
    bool accepting = true;
};
}
