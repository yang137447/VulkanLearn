#pragma once

#include "terrain/terrainCompositedVTStore.h"
#include "terrain/terrainRvtBudget.h"

#include <future>
#include <memory>
#include <optional>
#include <map>
#include <set>

namespace VL
{
struct TerrainCompositedResidencyBudget
{
    uint32_t slots = 64;
    uint32_t concurrentReads = 2;
    uint32_t queuedRequests = 4096;
    uint32_t uploadsPerFrame = 4;
    uint32_t maxReadAttempts = 3;
    uint64_t cpuBytes = 16 * 1024 * 1024;
    uint64_t uploadBytesPerFrame = 1024 * 1024;
};

struct TerrainCompositedResidencyStats
{
    uint64_t cpuBytes = 0;
    uint64_t readsStarted = 0;
    uint64_t readFailures = 0;
    uint64_t discardedResults = 0;
    uint64_t throttledRequests = 0;
    uint64_t parentFallbacks = 0;
    uint64_t misses = 0;
    uint64_t uploads = 0;
    uint32_t budgetSelectedPages = 0;
    uint32_t budgetMipBias = 0;
    uint64_t queuedCancel = 0;
    uint64_t evictedReads = 0;
    // 已完成回读的有效请求数与目标 mip；不是像素计数或实际 shader 采样结果。
    uint64_t feedbackRequests = 0;
    uint64_t feedbackEstimatedExactHits = 0;
    uint64_t feedbackEstimatedParentFallbacks = 0;
    uint64_t feedbackEstimatedMisses = 0;
    uint64_t feedbackEstimatedStaleHits = 0;
    std::vector<uint64_t> feedbackTargetMipCounts;
    std::vector<uint64_t> feedbackEstimatedResolvedMipCounts;
    uint32_t pending = 0;
    uint32_t reading = 0;
    uint32_t resident = 0;
    uint32_t retiring = 0;
    uint32_t dirty = 0;
    uint32_t stale = 0;
    bool baselineFailed = false;
};

struct TerrainCompositedPageUpload
{
    TerrainCompositedVTTaskToken token;
    uint32_t slot = 0;
    std::shared_ptr<const std::vector<uint8_t>> bytes;
    uint64_t reservation = 0;
};

struct TerrainCompositedPageDiagnostics
{
    uint32_t slot = UINT32_MAX;
    uint32_t slotState = 0;
    uint32_t attempts = 0;
    uint32_t maxAttempts = 0;
    uint32_t queued = 0;
    uint32_t freeSlots = 0;
    uint32_t retiringSlots = 0;
    uint64_t backoffTicks = 0;
    uint64_t reservation = 0;
    uint64_t publishEpoch = 0;
    uint64_t retireEpoch = 0;
    uint64_t referenceEpoch = 0;
    bool tokenCurrent = false;
    bool retirementOwnerAlive = false;
    bool retirementReusable = false;
    uint64_t completedEpoch = 0;
    uint64_t schedulingTick = 0;
    uint64_t retryAfterTick = 0;
    bool pageQueued = false;
    bool pageReading = false;
    bool pageReady = false;
    bool pageProduced = false;
    bool slotTokenMatches = false;
    TerrainCompositedVTTaskToken reservedToken;
    TerrainCompositedVTTaskToken publishedToken;
};

enum class TerrainCompositedDiagnosticSlotState : uint32_t
{
    Free, Uploading, Resident, Retiring
};

struct TerrainCompositedSlotDiagnostics
{
    uint32_t slot = UINT32_MAX;
    TerrainCompositedDiagnosticSlotState state = TerrainCompositedDiagnosticSlotState::Free;
    uint64_t reservation = 0;
    TerrainCompositedVTTaskToken reservedToken;
    TerrainCompositedVTTaskToken publishedToken;
    uint64_t publishEpoch = 0;
    uint64_t retireEpoch = 0;
    uint64_t referenceEpoch = 0;
    uint64_t completedEpoch = 0;
    bool retirementOwnerAlive = false;
    bool retirementReusable = false;
};

struct TerrainCompositedSlotReuseDiagnostics
{
    TerrainCompositedSlotDiagnostics original;
    TerrainCompositedSlotDiagnostics current;
    uint64_t boundaryChecks = 0;
    uint64_t blockedByEpoch = 0;
    uint64_t blockedByOwner = 0;
    uint64_t lastBoundaryCompletedEpoch = 0;
    uint64_t releaseCompletedEpoch = 0;
    uint64_t releaseRetireEpoch = 0;
    uint64_t releaseReferenceEpoch = 0;
    uint64_t replacementReservation = 0;
    uint64_t replacementCompletedEpoch = 0;
    bool retirementObserved = false;
    bool releasedAtBoundary = false;
    bool replacementObserved = false;
    bool replacementStillPresent = false;
};

struct TerrainCompositedResidencyProgressDiagnostics
{
    uint64_t worldGeneration = 0;
    uint64_t revision = 0;
    uint64_t requiredPages = 0;
    uint64_t currentPages = 0;
    uint32_t queued = 0;
    uint32_t reading = 0;
    uint32_t ready = 0;
    uint32_t produced = 0;
    bool rootCurrent = false;
    bool retired = false;
};

struct TerrainCompositedResidencyBatch
{
    uint64_t id = 0;
    std::vector<TerrainCompositedPageUpload> uploads;
    std::vector<TerrainCompositedVTPageKey> removals;
};

struct TerrainCompositedSlotOwner
{
    uint32_t slot = 0;
    bool valid = false;
    TerrainCompositedVTTaskToken token;
};

// World 的所有 Surface 共用同一物理槽目录；页表无效提交完成前禁止复用退休槽。
class TerrainCompositedResidencyPool
{
public:
    explicit TerrainCompositedResidencyPool(uint32_t capacity, uint32_t freeReserve = 0);
    ~TerrainCompositedResidencyPool();
    uint32_t Capacity() const;
    std::optional<TerrainCompositedSlotDiagnostics> SlotDiagnostics(
        uint32_t slot, uint64_t completedEpoch) const;
    bool ArmSlotReuseDiagnostics(uint32_t slot, TerrainRvtBudgetOwner& budgetOwner);
    void CancelSlotReuseDiagnostics() noexcept;
    std::optional<TerrainCompositedSlotReuseDiagnostics> SlotReuseDiagnostics(
        uint64_t completedEpoch) const;

private:
    friend class TerrainCompositedResidency;
    friend class TerrainCompositedRuntime;
    friend struct TerrainCompositedRequestProbe;
    void CollectReusable(uint64_t completedEpoch);
    void ObserveReservation(uint32_t slot, uint64_t completedEpoch) noexcept;
    struct SlotReuseWatch;
    std::unique_ptr<SlotReuseWatch> slotReuseWatch;
    uint32_t UnassignedRoots() const;
    enum class SlotState { Free, Uploading, Resident, Retiring };
    struct Slot
    {
        SlotState state = SlotState::Free;
        TerrainCompositedVTPageKey key;
        uint64_t lastUsed = 0;
        uint64_t retireEpoch = 0;
        bool root = false;
        bool selected = false;
        bool unmapping = false;
        TerrainCompositedVTTaskToken publishedToken;
        TerrainCompositedVTTaskToken reservedToken;
        uint64_t reservation = 0;
        uint64_t publishEpoch = 0;
        uint64_t referenceEpoch = 0;
        std::weak_ptr<void> retirementBinding;
    };
    std::vector<Slot> slots;
    uint64_t clock = 0;
    uint32_t freeReserve = 0;
    uint64_t nextReservation = 0;
    std::set<std::string> rootNamespaces;
};

// 页面包是只读输入；本类只管理 CPU IO/槽位事务，GPU 资源由 Runtime 持有。
class TerrainCompositedResidency
{
public:
    TerrainCompositedResidency(
        std::shared_ptr<const TerrainCompositedVTStore> store,
        const TerrainCompositedVTBuildDesc& surface,
        uint64_t generation, uint64_t revision,
        TerrainCompositedResidencyBudget budget = {},
        std::shared_ptr<TerrainCompositedResidencyPool> sharedPool = {},
        const std::string& namespaceId = {});
    ~TerrainCompositedResidency();
    void SeedReadOnlyBaseline();
    void ConsumeFeedback(const std::vector<TerrainCompositedVTTaskToken>& requests);
    void InvalidatePreview(const TerrainCompositedVTBuildDesc& surface,
        uint64_t generation, uint64_t revision,
        const std::vector<TerrainCompositedVTPageKey>& dirtyPages);
    void SubmitPreviewPages(const TerrainCompositedVTBuildDesc& surface,
        uint64_t generation, uint64_t revision,
        const std::vector<TerrainCompositedVTPageData>& pages);
    TerrainCompositedResidencyBatch BeginUploads(uint64_t completedEpoch);
    TerrainCompositedResidencyBatch BeginUploads(
        uint64_t completedEpoch, uint64_t byteAllowance, uint32_t pageAllowance);
    void Commit(uint64_t batchId, uint64_t submittedEpoch);
    void Abort(uint64_t batchId);
    bool Ready() const;
    bool CurrentRoot() const;
    std::optional<uint32_t> Resolve(const TerrainCompositedVTPageKey& key);
    TerrainCompositedResidencyStats Statistics() const;
    TerrainCompositedVTTaskToken Token(const TerrainCompositedVTPageKey& key) const;
    std::vector<TerrainCompositedVTTaskToken> PreviewRequests() const;
    std::vector<TerrainCompositedVTTaskToken> ProductionRequests() const;
    std::vector<TerrainCompositedSlotOwner> CommittedOwners() const;
    std::optional<TerrainCompositedPageUpload> ReserveProduction(
        const TerrainCompositedVTTaskToken& token, uint64_t completedEpoch);
    bool CompleteProduction(const TerrainCompositedPageUpload& page, uint64_t epoch, bool success);
    bool OwnsProduction(const TerrainCompositedPageUpload& page) const;
    TerrainCompositedPageDiagnostics Diagnostics(
        const TerrainCompositedVTTaskToken& token, uint64_t completedEpoch) const;
    TerrainCompositedResidencyProgressDiagnostics ProgressDiagnostics(
        const std::vector<TerrainCompositedVTPageKey>* requiredPages,
        uint64_t completedEpoch) const;
    void MarkReferenced(uint64_t epoch);
    bool Retired() const;
    bool CanRetire() const noexcept;
    void RetireAll(uint64_t epoch, std::weak_ptr<void> bindingLifetime = {});
    void RetireAllNoThrow(uint64_t epoch, std::weak_ptr<void> bindingLifetime) noexcept;
    bool AllCurrent(const std::vector<TerrainCompositedVTPageKey>& pages,
        uint64_t completedEpoch = UINT64_MAX) const;
    std::vector<TerrainCompositedVTPageKey> SelectedPages() const;
    const TerrainCompositedVTLayout& Layout() const { return layout; }

private:
    friend struct TerrainCompositedRequestProbe;
    using SlotState = TerrainCompositedResidencyPool::SlotState;
    using Slot = TerrainCompositedResidencyPool::Slot;
    struct Read
    {
        TerrainCompositedVTTaskToken token;
        std::future<std::vector<uint8_t>> result;
    };
    void Request(const TerrainCompositedVTPageKey& key);
    void PollReads();
    void StartReads();
    bool IsResident(const TerrainCompositedVTPageKey& key) const;
    bool Needed(const TerrainCompositedVTPageKey& key) const;
    std::optional<uint32_t> ResolveMapping(const TerrainCompositedVTPageKey& key, bool touch);
    std::shared_ptr<const TerrainCompositedVTStore> store;
    TerrainCompositedVTLayout layout;
    TerrainCompositedVTTaskToken identity;
    TerrainCompositedResidencyBudget budget;
    TerrainCompositedVTPageKey baseline;
    uint64_t pageBytes = 0;
    uint64_t clock = 0;
    uint64_t nextBatch = 0;
    uint64_t lastSubmitted = 0;
    bool hasSubmitted = false;
    bool baselineSeeded = false;
    std::shared_ptr<TerrainCompositedResidencyPool> pool;
    std::vector<Slot>& slots;
    std::set<TerrainCompositedVTPageKey> pending;
    std::set<TerrainCompositedVTPageKey> budgetSelected;
    bool hasFeedbackSelection = false;
    std::map<TerrainCompositedVTPageKey, Read> reads;
    std::map<TerrainCompositedVTPageKey, uint32_t> attempts;
    std::map<TerrainCompositedVTPageKey, uint64_t> retryAfter;
    std::set<TerrainCompositedVTPageKey> dirty;
    std::set<TerrainCompositedVTPageKey> previewChanged;
    bool preview = false;
    bool retired = false;
    std::string storeSurfaceId;
    std::map<TerrainCompositedVTPageKey, std::shared_ptr<const std::vector<uint8_t>>> ready;
    std::map<TerrainCompositedVTPageKey, TerrainCompositedPageUpload> produced;
    TerrainCompositedResidencyBatch active;
    TerrainCompositedResidencyStats stats;
};
}
