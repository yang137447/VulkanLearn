#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <functional>
#include <map>
#include <mutex>
#include <optional>
#include <string>
#include "terrain/terrainCompositedResidency.h"

namespace VL
{

enum class TerrainFaultKind
{
    None,
    StatusNonzero,
    SubmitFail,
    StaleRevision,
    BudgetReject,
    SlotReuse,
    WorldCandidateBadInput,
    Cancel
};

struct TerrainFaultPageIdentity
{
    std::string surfaceId;
    uint32_t mip = 0;
    uint32_t x = 0;
    uint32_t y = 0;
    uint64_t revision = 0;
};

struct TerrainFaultRequest
{
    TerrainFaultKind kind = TerrainFaultKind::None;
    std::string faultId;
    uint64_t worldGeneration = 0;
    TerrainFaultPageIdentity page;
    uint32_t status = 0;
    int32_t submitResult = 0;
    uint64_t replacementRevision = 0;
    uint64_t testAdmissionLimitBytes = 0;
    uint32_t slot = 0;
    uint64_t writeEpoch = 0;
    std::string candidateScene;
    std::string objectId;
    uint64_t byteOffset = 0;
    uint32_t xorMask = 0;
};

enum class TerrainFaultQueueCode
{
    Queued,
    PendingHookMissing,
    InvalidRequest,
    Busy,
    Duplicate,
    Cancelled,
    NotFound
};

struct TerrainFaultQueueResult
{
    TerrainFaultQueueCode code = TerrainFaultQueueCode::InvalidRequest;
    std::string diagnostic;
};

struct TerrainFaultStatusObservation
{
    uint64_t worldGeneration = 0;
    TerrainFaultPageIdentity page;
    uint64_t writeEpoch = 0;
    uint64_t fenceCompletedEpoch = 0;
    uint32_t originalStatus = 0;
};

struct TerrainFaultStatusOverride
{
    std::string faultId;
    uint32_t originalStatus = 0;
    uint32_t injectedStatus = 0;
};

struct TerrainFaultSubmitToken
{
    uint64_t generation = 0;
    std::string contentIdentity;
    uint64_t submissionEpoch = 0;
};

struct TerrainFaultSubmitRequest
{
    TerrainFaultRequest fault;
    TerrainFaultSubmitToken token;
};

struct TerrainFaultSubmitObservation
{
    uint64_t worldGeneration = 0;
    TerrainFaultPageIdentity page;
    TerrainFaultSubmitToken token;
};

struct TerrainFaultSubmitFailure
{
    uint64_t abortTicket = 0;
    std::string faultId;
    int32_t injectedResult = 0;
};

// 仅由实际 Abort 返回后的 owner 提供原始计数；不把代际相等声明为 oldWorldUsable。
struct TerrainFaultSubmitAbortObservation
{
    uint64_t abortTicket = 0;
    TerrainFaultSubmitObservation task;
    uint64_t activeGenerationBefore = 0;
    uint64_t activeGenerationAfter = 0;
    uint32_t recordedTasks = 0;
    uint32_t abortedTasks = 0;
    uint32_t publishedChannels = 0;
    uint32_t backendSubmitCalls = 0;
};

struct TerrainFaultSlotHooks
{
    std::function<std::optional<TerrainCompositedSlotDiagnostics>(uint32_t, uint64_t)> slot;
    std::function<bool(uint32_t)> arm;
    std::function<std::optional<TerrainCompositedSlotReuseDiagnostics>(uint64_t)> poll;
    std::function<void()> cancel;
};

struct TerrainFaultActionState
{
    TerrainFaultRequest request;
    bool consumed = false;
    bool cancelRequested = false;
    bool timedOut = false;
};

const char* TerrainFaultKindName(TerrainFaultKind kind);
const char* TerrainFaultQueueCodeName(TerrainFaultQueueCode code);
std::string ValidateTerrainFaultRequest(const TerrainFaultRequest& request);

// 接收显式测试 DTO；只在 owner 的实际完成边界消费一次，不持有 Vulkan/Editor 或发出通过证据。
class TerrainFaultInjectionController
{
public:
    static constexpr std::size_t MaxOwners = 8;
    static constexpr std::size_t RecentFaultIds = 64;
    static constexpr std::chrono::seconds RequestLifetime{60};

    uint64_t RegisterStatusHook(uint64_t worldGeneration);
    void UnregisterStatusHook(uint64_t registration);
    TerrainFaultQueueResult Queue(const TerrainFaultRequest& request);
    std::optional<TerrainFaultStatusOverride> ConsumeStatus(
        uint64_t registration, const TerrainFaultStatusObservation& observation);
    uint64_t RegisterSubmitHook(uint64_t worldGeneration);
    void UnregisterSubmitHook(uint64_t registration);
    TerrainFaultQueueResult QueueSubmit(const TerrainFaultSubmitRequest& request);
    std::optional<TerrainFaultSubmitFailure> ConsumeBeforeSubmit(
        uint64_t registration, const TerrainFaultSubmitObservation& observation);
    std::optional<TerrainFaultSubmitAbortObservation> ObserveSubmitAbort(
        uint64_t registration, const TerrainFaultSubmitAbortObservation& observation);
    uint64_t RegisterSlotHook(uint64_t worldGeneration, TerrainFaultSlotHooks hooks);
    void UnregisterSlotHook(uint64_t registration);
    void PumpSlotHook(uint64_t registration, uint64_t completedEpoch,
        std::chrono::steady_clock::time_point now = std::chrono::steady_clock::now());
    uint64_t RegisterActionHook(uint64_t worldGeneration, TerrainFaultKind kind);
    void UnregisterActionHook(uint64_t registration);
    std::optional<TerrainFaultActionState> ReadAction(uint64_t registration,
        std::chrono::steady_clock::time_point now = std::chrono::steady_clock::now()) const;
    std::optional<TerrainFaultRequest> ConsumeAction(uint64_t registration,
        uint64_t worldGeneration, const TerrainFaultPageIdentity& page = {});
    bool FinishAction(uint64_t registration, const std::string& faultId);

private:
    void ExpirePending();
    void ClearPending();

    mutable std::mutex mutex;
    uint64_t nextRegistration = 1;
    std::map<uint64_t, uint64_t> statusOwners;
    std::map<uint64_t, uint64_t> submitOwners;
    struct SlotOwner
    {
        uint64_t worldGeneration;
        TerrainFaultSlotHooks hooks;
    };
    std::map<uint64_t, SlotOwner> slotOwners;
    struct ActionOwner
    {
        uint64_t worldGeneration;
        TerrainFaultKind kind;
    };
    std::map<uint64_t, ActionOwner> actionOwners;
    bool actionConsumed = false;
    bool actionCancelRequested = false;
    bool slotArmed = false;
    bool slotCancelRequested = false;
    std::optional<TerrainFaultRequest> pending;
    std::optional<TerrainFaultSubmitRequest> pendingSubmit;
    uint64_t nextAbortTicket = 1;
    uint64_t awaitingAbortTicket = 0;
    uint64_t pendingRegistration = 0;
    std::chrono::steady_clock::time_point expires;
    std::deque<std::string> recentIds;
};

TerrainFaultInjectionController& TerrainFaultInjection();

}
