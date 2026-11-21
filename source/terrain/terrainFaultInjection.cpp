#include "terrain/terrainFaultInjection.h"

#include <algorithm>
#include <limits>
#include <stdexcept>
#include <nlohmann/json.hpp>
#include "core/runtimeEvidence.h"

namespace VL
{
namespace
{
bool Identifier(const std::string& value, size_t maximum)
{
    return !value.empty() && value.size() <= maximum &&
        std::all_of(value.begin(), value.end(), [](unsigned char character) {
            return character >= 32 && character != 127;
        });
}

bool RelativeScene(const std::string& value)
{
    if (!Identifier(value, 512) || value.front() == '/' || value.front() == '\\' ||
        value.find(':') != std::string::npos || value.find('\\') != std::string::npos)
        return false;
    size_t start = 0;
    while (start <= value.size())
    {
        const auto end = value.find('/', start);
        const auto component = value.substr(start, end - start);
        if (component.empty() || component == "." || component == "..")
            return false;
        if (end == std::string::npos)
            break;
        start = end + 1;
    }
    return value.size() >= 5 && value.substr(value.size() - 5) == ".json";
}

bool SamePage(const TerrainFaultPageIdentity& requested, const TerrainFaultPageIdentity& observed)
{
    return requested.surfaceId == observed.surfaceId && requested.mip == observed.mip &&
        requested.x == observed.x && requested.y == observed.y && requested.revision == observed.revision;
}

bool ValidSubmitToken(const TerrainFaultSubmitToken& token)
{
    return token.generation && token.submissionEpoch && token.contentIdentity.size() == 64 &&
        std::all_of(token.contentIdentity.begin(), token.contentIdentity.end(), [](unsigned char character) {
            return (character >= '0' && character <= '9') || (character >= 'a' && character <= 'f');
        });
}

bool SameSubmitTarget(const TerrainFaultSubmitRequest& request, const TerrainFaultSubmitObservation& observed)
{
    return request.fault.worldGeneration == observed.worldGeneration &&
        SamePage(request.fault.page, observed.page) &&
        request.token.generation == observed.token.generation &&
        request.token.contentIdentity == observed.token.contentIdentity &&
        request.token.submissionEpoch == observed.token.submissionEpoch;
}

nlohmann::json SlotToken(const TerrainCompositedVTTaskToken& token)
{
    return {{"worldGeneration", token.worldGeneration}, {"revision", token.revision},
        {"contentHashBytes", token.contentHash.bytes}, {"surfaceId", token.key.surfaceId},
        {"mip", token.key.mip}, {"x", token.key.x}, {"y", token.key.y}};
}

nlohmann::json SlotSnapshot(const TerrainCompositedSlotDiagnostics& slot)
{
    return {{"slot", slot.slot}, {"state", static_cast<uint32_t>(slot.state)},
        {"reservation", slot.reservation}, {"reservedToken", SlotToken(slot.reservedToken)},
        {"publishedToken", SlotToken(slot.publishedToken)}, {"publishEpoch", slot.publishEpoch},
        {"retireEpoch", slot.retireEpoch}, {"referenceEpoch", slot.referenceEpoch},
        {"completedEpoch", slot.completedEpoch}, {"retirementOwnerAlive", slot.retirementOwnerAlive},
        {"retirementReusable", slot.retirementReusable}};
}

void SlotEvidence(const TerrainFaultRequest& request, const char* event,
    const std::optional<TerrainCompositedSlotReuseDiagnostics>& watch = {})
{
    nlohmann::json payload{{"protocol", 1}, {"event", event}, {"hook", "slotReuse"},
        {"faultId", request.faultId}, {"worldGeneration", request.worldGeneration}};
    if (watch)
    {
        const auto& observed = *watch;
        payload["observation"] = {
            {"original", SlotSnapshot(observed.original)}, {"current", SlotSnapshot(observed.current)},
            {"boundaryChecks", observed.boundaryChecks}, {"blockedByEpoch", observed.blockedByEpoch},
            {"blockedByOwner", observed.blockedByOwner},
            {"lastBoundaryCompletedEpoch", observed.lastBoundaryCompletedEpoch},
            {"releaseCompletedEpoch", observed.releaseCompletedEpoch},
            {"releaseRetireEpoch", observed.releaseRetireEpoch},
            {"releaseReferenceEpoch", observed.releaseReferenceEpoch},
            {"replacementReservation", observed.replacementReservation},
            {"replacementCompletedEpoch", observed.replacementCompletedEpoch},
            {"retirementObserved", observed.retirementObserved},
            {"releasedAtBoundary", observed.releasedAtBoundary},
            {"replacementObserved", observed.replacementObserved},
            {"replacementStillPresent", observed.replacementStillPresent}};
    }
    RuntimeEvidence::Record("RVT_EVIDENCE", payload);
}

bool SlotMatches(const TerrainFaultRequest& request, const TerrainCompositedSlotDiagnostics& slot)
{
    const auto& token = slot.publishedToken;
    return slot.slot == request.slot && slot.reservation &&
        slot.state != TerrainCompositedDiagnosticSlotState::Free &&
        slot.publishEpoch == request.writeEpoch && token.worldGeneration == request.worldGeneration &&
        token.revision == request.page.revision && token.key.surfaceId == request.page.surfaceId &&
        token.key.mip == request.page.mip && token.key.x == request.page.x && token.key.y == request.page.y;
}
}

const char* TerrainFaultKindName(TerrainFaultKind kind)
{
    switch (kind)
    {
    case TerrainFaultKind::StatusNonzero: return "statusNonzero";
    case TerrainFaultKind::SubmitFail: return "submitFail";
    case TerrainFaultKind::StaleRevision: return "staleRevision";
    case TerrainFaultKind::BudgetReject: return "budgetReject";
    case TerrainFaultKind::SlotReuse: return "slotReuse";
    case TerrainFaultKind::WorldCandidateBadInput: return "worldCandidateBadInput";
    case TerrainFaultKind::Cancel: return "cancel";
    default: return "none";
    }
}

const char* TerrainFaultQueueCodeName(TerrainFaultQueueCode code)
{
    switch (code)
    {
    case TerrainFaultQueueCode::Queued: return "queued";
    case TerrainFaultQueueCode::PendingHookMissing: return "pendinghookmissing";
    case TerrainFaultQueueCode::InvalidRequest: return "invalidrequest";
    case TerrainFaultQueueCode::Busy: return "busy";
    case TerrainFaultQueueCode::Duplicate: return "duplicate";
    case TerrainFaultQueueCode::Cancelled: return "cancelled";
    case TerrainFaultQueueCode::NotFound: return "notfound";
    }
    return "invalidrequest";
}

std::string ValidateTerrainFaultRequest(const TerrainFaultRequest& request)
{
    if (!Identifier(request.faultId, 96))
        return "faultId must contain 1..96 printable bytes";
    if (request.page.surfaceId.size() > 256 || request.candidateScene.size() > 512 ||
        request.objectId.size() > 256)
        return "Fault request string storage exceeds its fixed bounds";
    if (request.kind == TerrainFaultKind::Cancel)
        return {};
    if (!request.worldGeneration)
        return "An exact nonzero World generation is required";
    if (request.kind == TerrainFaultKind::WorldCandidateBadInput)
    {
        if (!RelativeScene(request.candidateScene) || !Identifier(request.objectId, 256) ||
            !request.xorMask || request.xorMask > 255)
            return "Candidate input requires relative scene, objectId and xorMask 1..255";
        return {};
    }
    if (request.kind == TerrainFaultKind::BudgetReject)
        return request.testAdmissionLimitBytes ? std::string{} : "A positive reduced test admission limit is required";
    if (!Identifier(request.page.surfaceId, 256) || request.page.surfaceId.size() <= 8 ||
        request.page.surfaceId.rfind("terrain|", 0) != 0 ||
        request.page.mip > 31 ||
        request.page.x > 131071 || request.page.y > 131071)
        return "An exact bounded terrain page and revision are required";
    switch (request.kind)
    {
    case TerrainFaultKind::StatusNonzero:
        return request.status ? std::string{} : "Injected status must be nonzero";
    case TerrainFaultKind::SubmitFail:
        return request.submitResult < 0 ? std::string{} : "Injected VkResult must be negative";
    case TerrainFaultKind::StaleRevision:
        return request.replacementRevision > request.page.revision ? std::string{} :
            "Replacement revision must be newer than the held task";
    case TerrainFaultKind::SlotReuse:
        return request.writeEpoch ? std::string{} : "Slot reuse requires an actual nonzero write epoch";
    default:
        return "Unsupported fault kind";
    }
}

void TerrainFaultInjectionController::ClearPending()
{
    pending.reset();
    pendingSubmit.reset();
    pendingRegistration = 0;
    awaitingAbortTicket = 0;
    slotArmed = false;
    slotCancelRequested = false;
    actionConsumed = false;
    actionCancelRequested = false;
}

void TerrainFaultInjectionController::ExpirePending()
{
    if (pending && pending->kind != TerrainFaultKind::SlotReuse &&
        pending->kind != TerrainFaultKind::BudgetReject && pending->kind != TerrainFaultKind::StaleRevision &&
        std::chrono::steady_clock::now() >= expires)
        ClearPending();
}

uint64_t TerrainFaultInjectionController::RegisterStatusHook(uint64_t worldGeneration)
{
    std::lock_guard<std::mutex> lock(mutex);
    if (!worldGeneration || statusOwners.size() + submitOwners.size() + slotOwners.size() + actionOwners.size() >= MaxOwners ||
        nextRegistration == std::numeric_limits<uint64_t>::max() ||
        std::any_of(statusOwners.begin(), statusOwners.end(), [&](const auto& owner) {
            return owner.second == worldGeneration;
        }))
        return 0;
    const uint64_t registration = nextRegistration++;
    statusOwners.emplace(registration, worldGeneration);
    return registration;
}

void TerrainFaultInjectionController::UnregisterStatusHook(uint64_t registration)
{
    std::lock_guard<std::mutex> lock(mutex);
    if (statusOwners.erase(registration) && pendingRegistration == registration)
        ClearPending();
}

TerrainFaultQueueResult TerrainFaultInjectionController::Queue(const TerrainFaultRequest& request)
{
    const auto invalid = ValidateTerrainFaultRequest(request);
    if (!invalid.empty())
        return {TerrainFaultQueueCode::InvalidRequest, invalid};
    std::lock_guard<std::mutex> lock(mutex);
    ExpirePending();
    if (request.kind == TerrainFaultKind::Cancel)
    {
        if (!pending || pending->faultId != request.faultId)
            return {TerrainFaultQueueCode::NotFound, "No matching pending request"};
        if (awaitingAbortTicket)
            return {TerrainFaultQueueCode::Busy, "Consumed submit fault still awaits actual Abort observation"};
        if (pending->kind == TerrainFaultKind::SlotReuse)
        {
            slotCancelRequested = true;
            return {TerrainFaultQueueCode::Queued, "Cancellation queued for the serial Runtime owner"};
        }
        if (pending->kind == TerrainFaultKind::BudgetReject || pending->kind == TerrainFaultKind::StaleRevision)
        {
            actionCancelRequested = true;
            return {TerrainFaultQueueCode::Queued, "Cancellation queued; serial action owner must release actual held resources"};
        }
        ClearPending();
        return {TerrainFaultQueueCode::Cancelled, "Pending request cancelled"};
    }
    if (request.kind == TerrainFaultKind::SlotReuse)
    {
        const auto owner = std::find_if(slotOwners.begin(), slotOwners.end(), [&](const auto& entry) {
            return entry.second.worldGeneration == request.worldGeneration;
        });
        if (owner == slotOwners.end())
            return {TerrainFaultQueueCode::PendingHookMissing, "No live serial slot watch owner"};
        if (std::find(recentIds.begin(), recentIds.end(), request.faultId) != recentIds.end())
            return {TerrainFaultQueueCode::Duplicate, "faultId already queued within the bounded recent window"};
        if (pending)
            return {TerrainFaultQueueCode::Busy, "One fault request is already pending"};
        pending = request;
        pendingRegistration = owner->first;
        expires = std::chrono::steady_clock::now() + RequestLifetime;
        recentIds.push_back(request.faultId);
        if (recentIds.size() > RecentFaultIds)
            recentIds.pop_front();
        return {TerrainFaultQueueCode::Queued, "Slot request queued; owner identity check and charged arm still required"};
    }
    if (request.kind == TerrainFaultKind::SubmitFail || request.kind == TerrainFaultKind::BudgetReject ||
        request.kind == TerrainFaultKind::StaleRevision)
    {
        uint64_t registration = 0;
        if (request.kind == TerrainFaultKind::SubmitFail)
        {
            for (const auto& owner : submitOwners)
                if (owner.second == request.worldGeneration)
                    registration = owner.first;
        }
        else
        {
            for (const auto& owner : actionOwners)
                if (owner.second.worldGeneration == request.worldGeneration && owner.second.kind == request.kind)
                    registration = owner.first;
        }
        if (!registration)
            return {TerrainFaultQueueCode::PendingHookMissing, "No live serial owner for the exact World and fault kind"};
        if (std::find(recentIds.begin(), recentIds.end(), request.faultId) != recentIds.end())
            return {TerrainFaultQueueCode::Duplicate, "faultId already queued within the bounded recent window"};
        if (pending)
            return {TerrainFaultQueueCode::Busy, "One explicit fault request is already pending"};
        pending = request;
        pendingRegistration = registration;
        expires = std::chrono::steady_clock::now() + RequestLifetime;
        recentIds.push_back(request.faultId);
        if (recentIds.size() > RecentFaultIds)
            recentIds.pop_front();
        return {TerrainFaultQueueCode::Queued,
            "Explicit selector queued; only the serial owner can bind and consume an actual task or admission"};
    }
    if (request.kind != TerrainFaultKind::StatusNonzero)
        return {TerrainFaultQueueCode::PendingHookMissing, "Owner injection boundary is not connected"};
    const auto owner = std::find_if(statusOwners.begin(), statusOwners.end(), [&](const auto& entry) {
        return entry.second == request.worldGeneration;
    });
    if (owner == statusOwners.end())
        return {TerrainFaultQueueCode::PendingHookMissing, "No live status owner for the exact World generation"};
    if (std::find(recentIds.begin(), recentIds.end(), request.faultId) != recentIds.end())
        return {TerrainFaultQueueCode::Duplicate, "faultId already queued within the bounded recent window"};
    if (pending)
        return {TerrainFaultQueueCode::Busy, "One fault request is already pending"};
    pending = request;
    pendingRegistration = owner->first;
    expires = std::chrono::steady_clock::now() + RequestLifetime;
    recentIds.push_back(request.faultId);
    if (recentIds.size() > RecentFaultIds)
        recentIds.pop_front();
    return {TerrainFaultQueueCode::Queued, "One-shot request queued; this is not consumption or failure evidence"};
}

std::optional<TerrainFaultStatusOverride> TerrainFaultInjectionController::ConsumeStatus(
    uint64_t registration, const TerrainFaultStatusObservation& observation)
{
    std::lock_guard<std::mutex> lock(mutex);
    ExpirePending();
    const auto owner = statusOwners.find(registration);
    if (!pending || pending->kind != TerrainFaultKind::StatusNonzero ||
        pendingRegistration != registration || owner == statusOwners.end() ||
        owner->second != observation.worldGeneration ||
        pending->worldGeneration != observation.worldGeneration ||
        !SamePage(pending->page, observation.page) || observation.originalStatus != 0 ||
        !observation.writeEpoch || observation.fenceCompletedEpoch < observation.writeEpoch)
        return std::nullopt;
    TerrainFaultStatusOverride result{pending->faultId, observation.originalStatus, pending->status};
    ClearPending();
    return result;
}

uint64_t TerrainFaultInjectionController::RegisterSubmitHook(uint64_t worldGeneration)
{
    std::lock_guard<std::mutex> lock(mutex);
    if (!worldGeneration || statusOwners.size() + submitOwners.size() + slotOwners.size() + actionOwners.size() >= MaxOwners ||
        nextRegistration == std::numeric_limits<uint64_t>::max() ||
        std::any_of(submitOwners.begin(), submitOwners.end(), [&](const auto& owner) {
            return owner.second == worldGeneration;
        }))
        return 0;
    const uint64_t registration = nextRegistration++;
    submitOwners.emplace(registration, worldGeneration);
    return registration;
}

void TerrainFaultInjectionController::UnregisterSubmitHook(uint64_t registration)
{
    std::lock_guard<std::mutex> lock(mutex);
    if (submitOwners.erase(registration) && pendingRegistration == registration)
        ClearPending();
}

TerrainFaultQueueResult TerrainFaultInjectionController::QueueSubmit(const TerrainFaultSubmitRequest& request)
{
    const auto invalid = ValidateTerrainFaultRequest(request.fault);
    if (request.fault.kind != TerrainFaultKind::SubmitFail || !invalid.empty() ||
        !ValidSubmitToken(request.token) ||
        (request.fault.writeEpoch && request.fault.writeEpoch != request.token.submissionEpoch))
        return {TerrainFaultQueueCode::InvalidRequest, "submitFail requires bounded exact World/page/token/epoch and negative result"};
    std::lock_guard<std::mutex> lock(mutex);
    ExpirePending();
    const auto owner = std::find_if(submitOwners.begin(), submitOwners.end(), [&](const auto& entry) {
        return entry.second == request.fault.worldGeneration;
    });
    if (owner == submitOwners.end())
        return {TerrainFaultQueueCode::PendingHookMissing, "No live preSubmit/Abort owner for the exact World generation"};
    if (std::find(recentIds.begin(), recentIds.end(), request.fault.faultId) != recentIds.end())
        return {TerrainFaultQueueCode::Duplicate, "faultId already queued within the bounded recent window"};
    if (pending)
        return {TerrainFaultQueueCode::Busy, "One fault request or Abort observation is already pending"};
    pending = request.fault;
    pendingSubmit = request;
    pendingRegistration = owner->first;
    expires = std::chrono::steady_clock::now() + RequestLifetime;
    recentIds.push_back(request.fault.faultId);
    if (recentIds.size() > RecentFaultIds)
        recentIds.pop_front();
    return {TerrainFaultQueueCode::Queued, "Exact preSubmit request queued; no submission or Abort has been observed"};
}

std::optional<TerrainFaultSubmitFailure> TerrainFaultInjectionController::ConsumeBeforeSubmit(
    uint64_t registration, const TerrainFaultSubmitObservation& observation)
{
    std::lock_guard<std::mutex> lock(mutex);
    ExpirePending();
    const auto owner = submitOwners.find(registration);
    if (pending && pending->kind == TerrainFaultKind::SubmitFail && !pendingSubmit &&
        pendingRegistration == registration && owner != submitOwners.end() &&
        owner->second == observation.worldGeneration && pending->worldGeneration == observation.worldGeneration &&
        SamePage(pending->page, observation.page) && ValidSubmitToken(observation.token) &&
        (!pending->writeEpoch || pending->writeEpoch == observation.token.submissionEpoch))
        pendingSubmit = TerrainFaultSubmitRequest{*pending, observation.token};
    if (!pendingSubmit || awaitingAbortTicket || pendingRegistration != registration ||
        owner == submitOwners.end() || owner->second != observation.worldGeneration ||
        !SameSubmitTarget(*pendingSubmit, observation) ||
        nextAbortTicket == std::numeric_limits<uint64_t>::max())
        return std::nullopt;
    awaitingAbortTicket = nextAbortTicket++;
    return TerrainFaultSubmitFailure{
        awaitingAbortTicket, pendingSubmit->fault.faultId, pendingSubmit->fault.submitResult};
}

std::optional<TerrainFaultSubmitAbortObservation> TerrainFaultInjectionController::ObserveSubmitAbort(
    uint64_t registration, const TerrainFaultSubmitAbortObservation& observation)
{
    std::lock_guard<std::mutex> lock(mutex);
    ExpirePending();
    const auto owner = submitOwners.find(registration);
    if (!pendingSubmit || !awaitingAbortTicket || observation.abortTicket != awaitingAbortTicket ||
        pendingRegistration != registration || owner == submitOwners.end() ||
        owner->second != observation.task.worldGeneration || !SameSubmitTarget(*pendingSubmit, observation.task))
        return std::nullopt;
    const auto result = observation;
    ClearPending();
    return result;
}

TerrainFaultInjectionController& TerrainFaultInjection()
{
    static TerrainFaultInjectionController controller;
    return controller;
}

uint64_t TerrainFaultInjectionController::RegisterSlotHook(
    uint64_t worldGeneration, TerrainFaultSlotHooks hooks)
{
    std::lock_guard<std::mutex> lock(mutex);
    if (!worldGeneration || !hooks.slot || !hooks.arm || !hooks.poll || !hooks.cancel ||
        statusOwners.size() + submitOwners.size() + slotOwners.size() + actionOwners.size() >= MaxOwners ||
        nextRegistration == std::numeric_limits<uint64_t>::max() ||
        std::any_of(slotOwners.begin(), slotOwners.end(), [&](const auto& owner) {
            return owner.second.worldGeneration == worldGeneration;
        }))
        return 0;
    const auto registration = nextRegistration++;
    slotOwners.emplace(registration, SlotOwner{worldGeneration, std::move(hooks)});
    return registration;
}

void TerrainFaultInjectionController::UnregisterSlotHook(uint64_t registration)
{
    std::lock_guard<std::mutex> lock(mutex);
    const auto owner = slotOwners.find(registration);
    if (owner == slotOwners.end())
        return;
    if (pending && pendingRegistration == registration)
    {
        const auto request = *pending;
        if (slotArmed)
            owner->second.hooks.cancel();
        ClearPending();
        slotOwners.erase(owner);
        SlotEvidence(request, "slot-watch-owner-teardown");
        return;
    }
    slotOwners.erase(owner);
}

void TerrainFaultInjectionController::PumpSlotHook(uint64_t registration, uint64_t completedEpoch,
    std::chrono::steady_clock::time_point now)
{
    std::lock_guard<std::mutex> lock(mutex);
    const auto owner = slotOwners.find(registration);
    if (owner == slotOwners.end() || !pending || pendingRegistration != registration ||
        pending->kind != TerrainFaultKind::SlotReuse)
        return;
    const auto request = *pending;
    auto& hooks = owner->second.hooks;
    try
    {
        if (slotCancelRequested || now >= expires)
        {
            const bool cancelled = slotCancelRequested;
            const auto observed = slotArmed ? hooks.poll(completedEpoch) :
                std::optional<TerrainCompositedSlotReuseDiagnostics>{};
            if (slotArmed)
                hooks.cancel();
            ClearPending();
            SlotEvidence(request, cancelled ? "slot-watch-cancelled" : "slot-watch-timeout", observed);
            return;
        }
        if (!slotArmed)
        {
            const auto current = hooks.slot(request.slot, completedEpoch);
            if (!current || !SlotMatches(request, *current))
            {
                ClearPending();
                SlotEvidence(request, "slot-watch-identity-rejected");
                return;
            }
            if (!hooks.arm(request.slot))
            {
                ClearPending();
                SlotEvidence(request, "slot-watch-admission-rejected");
                return;
            }
            slotArmed = true;
            const auto watch = hooks.poll(completedEpoch);
            if (!watch || !SlotMatches(request, watch->original) ||
                watch->original.reservation != current->reservation ||
                watch->original.publishedToken.contentHash != current->publishedToken.contentHash)
                throw std::logic_error("Charged slot watch did not preserve the exact original identity");
            SlotEvidence(request, "slot-watch-armed", watch);
            return;
        }
        const auto watch = hooks.poll(completedEpoch);
        if (!watch)
            throw std::logic_error("Armed native slot watch disappeared before cancellation");
        const bool terminal = watch->replacementObserved;
        if (terminal)
        {
            hooks.cancel();
            ClearPending();
        }
        SlotEvidence(request, terminal ? "slot-watch-replacement-observed" : "slot-watch-poll", watch);
    }
    catch (...)
    {
        if (slotArmed)
            hooks.cancel();
        ClearPending();
        throw;
    }
}

uint64_t TerrainFaultInjectionController::RegisterActionHook(uint64_t worldGeneration, TerrainFaultKind kind)
{
    std::lock_guard<std::mutex> lock(mutex);
    if (!worldGeneration || (kind != TerrainFaultKind::BudgetReject && kind != TerrainFaultKind::StaleRevision) ||
        statusOwners.size() + submitOwners.size() + slotOwners.size() + actionOwners.size() >= MaxOwners ||
        nextRegistration == std::numeric_limits<uint64_t>::max() ||
        std::any_of(actionOwners.begin(), actionOwners.end(), [&](const auto& owner) {
            return owner.second.worldGeneration == worldGeneration && owner.second.kind == kind;
        }))
        return 0;
    const auto registration = nextRegistration++;
    actionOwners.emplace(registration, ActionOwner{worldGeneration, kind});
    return registration;
}

void TerrainFaultInjectionController::UnregisterActionHook(uint64_t registration)
{
    std::lock_guard<std::mutex> lock(mutex);
    if (actionOwners.erase(registration) && pendingRegistration == registration)
        ClearPending();
}

std::optional<TerrainFaultActionState> TerrainFaultInjectionController::ReadAction(
    uint64_t registration, std::chrono::steady_clock::time_point now) const
{
    std::lock_guard<std::mutex> lock(mutex);
    if (!pending || pendingRegistration != registration || !actionOwners.count(registration))
        return {};
    return TerrainFaultActionState{*pending, actionConsumed, actionCancelRequested, now >= expires};
}

std::optional<TerrainFaultRequest> TerrainFaultInjectionController::ConsumeAction(
    uint64_t registration, uint64_t worldGeneration, const TerrainFaultPageIdentity& page)
{
    std::lock_guard<std::mutex> lock(mutex);
    const auto owner = actionOwners.find(registration);
    if (!pending || pendingRegistration != registration || owner == actionOwners.end() ||
        actionConsumed || actionCancelRequested || std::chrono::steady_clock::now() >= expires ||
        owner->second.worldGeneration != worldGeneration || pending->worldGeneration != worldGeneration ||
        owner->second.kind != pending->kind ||
        (pending->kind == TerrainFaultKind::StaleRevision && !SamePage(pending->page, page)))
        return {};
    auto result = *pending;
    actionConsumed = true;
    return result;
}

bool TerrainFaultInjectionController::FinishAction(uint64_t registration, const std::string& faultId)
{
    std::lock_guard<std::mutex> lock(mutex);
    if (!pending || pendingRegistration != registration || pending->faultId != faultId ||
        !actionOwners.count(registration))
        return false;
    ClearPending();
    return true;
}
}
