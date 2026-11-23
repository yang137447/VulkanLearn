#pragma once
#include "render/terrain/terrainCompositedRuntime.h"
#include <algorithm>
#include <array>
#include <ostream>
#include <nlohmann/json.hpp>

namespace VL
{
struct TerrainCompositedRequestProbe
{
    struct Row
    {
        TerrainCompositedVTTaskToken requested;
        std::optional<TerrainCompositedPageDiagnostics> page;
        const char* currentGate = "namespace-absent";
        uint32_t freeReserve = 0;
        uint32_t unassignedRoots = 0;
        uint32_t reusableRetiring = 0;
        uint32_t blockedRetiringEpoch = 0;
        uint32_t blockedRetiringOwner = 0;
        uint32_t uploadingSlots = 0;
        uint32_t residentSlots = 0;
        uint32_t eligibleEvictionSlots = 0;
        uint32_t effectiveMipBias = 0;
        bool budgetInsufficient = false;
        uint64_t maxRetireEpoch = 0;
        uint64_t maxReferenceEpoch = 0;
        uint64_t matchingRetirementOwnerReferences = 0;
        uint64_t readFailures = 0;
        uint64_t discardedResults = 0;
        uint64_t queuedCancel = 0;
        uint64_t evictedReads = 0;
        uint64_t throttledRequests = 0;
        bool needed = false;
        bool selected = false;
        bool transactionActive = false;
    };

    static Row Inspect(const TerrainCompositedRuntime& runtime,
        const TerrainCompositedVTTaskToken& requested, uint64_t completedEpoch)
    {
        Row result;
        result.requested = requested;
        result.effectiveMipBias = runtime.effectiveMipBias;
        result.budgetInsufficient = runtime.budgetInsufficient;
        const auto found = runtime.surfaces.find(requested.key.surfaceId);
        if (found == runtime.surfaces.end())
            return result;
        const auto& residency = *found->second.residency;
        const auto& pool = *residency.pool;
        result.page = residency.Diagnostics(requested, completedEpoch);
        result.needed = residency.Needed(requested.key);
        result.selected = residency.budgetSelected.count(requested.key) != 0;
        result.transactionActive = residency.active.id != 0;
        result.freeReserve = pool.freeReserve;
        result.unassignedRoots = pool.UnassignedRoots();
        result.readFailures = residency.stats.readFailures;
        result.discardedResults = residency.stats.discardedResults;
        result.queuedCancel = residency.stats.queuedCancel;
        result.evictedReads = residency.stats.evictedReads;
        result.throttledRequests = residency.stats.throttledRequests;
        for (const auto& slot : pool.slots)
        {
            using State = TerrainCompositedResidencyPool::SlotState;
            result.uploadingSlots += slot.state == State::Uploading;
            result.residentSlots += slot.state == State::Resident;
            result.eligibleEvictionSlots += slot.state == State::Resident &&
                !slot.root && !slot.selected && !slot.unmapping;
            if (slot.state != State::Retiring)
                continue;
            result.reusableRetiring += slot.retireEpoch <= completedEpoch &&
                slot.retirementBinding.expired();
            result.blockedRetiringEpoch += slot.retireEpoch > completedEpoch;
            result.blockedRetiringOwner += !slot.retirementBinding.expired();
            result.maxRetireEpoch = std::max(result.maxRetireEpoch, slot.retireEpoch);
            result.maxReferenceEpoch = std::max(result.maxReferenceEpoch, slot.referenceEpoch);
            if (slot.key == requested.key)
                result.matchingRetirementOwnerReferences = slot.retirementBinding.use_count();
        }
        const auto& page = *result.page;
        if (found->second.retired) result.currentGate = "surface-retired";
        else if (!residency.layout.Contains(requested.key)) result.currentGate = "outside-layout";
        else if (!page.tokenCurrent) result.currentGate = "stale-token";
        else if (!result.needed) result.currentGate = "not-selected";
        else if (page.slotTokenMatches && page.slotState == 2) result.currentGate = "resident";
        else if (page.pageProduced) result.currentGate = "produced-awaiting-table";
        else if (page.pageReading) result.currentGate = "read-in-flight";
        else if (page.pageReady) result.currentGate = "payload-ready";
        else if (page.slotTokenMatches && page.slotState == 1) result.currentGate = "reservation-in-flight";
        else if (page.attempts >= page.maxAttempts) result.currentGate = "attempts-exhausted";
        else if (page.backoffTicks) result.currentGate = "backoff";
        else if (result.transactionActive) result.currentGate = "upload-transaction-active";
        else if (!page.pageQueued) result.currentGate =
            residency.pending.size() >= residency.budget.queuedRequests ?
                "queue-cap-at-observation" : "not-queued-unknown-history";
        else if (!page.freeSlots && !result.reusableRetiring) result.currentGate = "no-reusable-slot";
        else if (!(requested.key == residency.baseline) &&
            page.freeSlots + result.reusableRetiring <= result.freeReserve + result.unassignedRoots)
            result.currentGate = "free-reserve-watermark";
        else result.currentGate = "queued-eligible";
        return result;
    }

    static void Write(std::ostream& output, const Row& row, uint64_t actualRenderFrame,
        uint64_t completedEpoch)
    {
        nlohmann::json record{
            {"event", "rvt-explicit-request-probe"}, {"renderFrame", actualRenderFrame},
            {"completedEpoch", completedEpoch}, {"namespace", row.requested.key.surfaceId},
            {"worldGeneration", row.requested.worldGeneration}, {"revision", row.requested.revision},
            {"requestedContent", row.requested.contentHash.ToHex()},
            {"page", {row.requested.key.mip, row.requested.key.x, row.requested.key.y}},
            {"currentGate", row.currentGate}, {"historicalRejectionProven", false},
            {"needed", row.needed}, {"selected", row.selected},
            {"freeReserve", row.freeReserve}, {"unassignedRoots", row.unassignedRoots},
            {"reusableRetiring", row.reusableRetiring}, {"blockedRetiringEpoch", row.blockedRetiringEpoch},
            {"blockedRetiringOwner", row.blockedRetiringOwner}, {"uploadingSlots", row.uploadingSlots},
            {"residentSlots", row.residentSlots}, {"eligibleEvictionSlots", row.eligibleEvictionSlots},
            {"effectiveMipBias", row.effectiveMipBias}, {"budgetInsufficient", row.budgetInsufficient},
            {"maxRetireEpoch", row.maxRetireEpoch}, {"maxReferenceEpoch", row.maxReferenceEpoch},
            {"matchingRetirementOwnerReferences", row.matchingRetirementOwnerReferences},
            {"readFailures", row.readFailures}, {"discardedResults", row.discardedResults},
            {"queuedCancel", row.queuedCancel}, {"evictedReads", row.evictedReads},
            {"throttledRequests", row.throttledRequests}};
        if (row.page)
        {
            const auto& page = *row.page;
            record["native"] = {{"slot", page.slot}, {"state", page.slotState},
                {"reservation", page.reservation}, {"queued", page.queued},
                {"pageQueued", page.pageQueued}, {"reading", page.pageReading},
                {"produced", page.pageProduced}, {"ready", page.pageReady},
                {"attempts", page.attempts}, {"maxAttempts", page.maxAttempts},
                {"backoffTicks", page.backoffTicks}, {"schedulingTick", page.schedulingTick},
                {"retryAfterTick", page.retryAfterTick}, {"freeSlots", page.freeSlots},
                {"retiringSlots", page.retiringSlots}, {"publishEpoch", page.publishEpoch},
                {"referenceEpoch", page.referenceEpoch}, {"retireEpoch", page.retireEpoch},
                {"ownerAlive", page.retirementOwnerAlive}, {"tokenCurrent", page.tokenCurrent},
                {"reservedGeneration", page.reservedToken.worldGeneration},
                {"reservedRevision", page.reservedToken.revision},
                {"reservedNamespace", page.reservedToken.key.surfaceId},
                {"reservedContent", page.reservedToken.contentHash.ToHex()},
                {"publishedGeneration", page.publishedToken.worldGeneration},
                {"publishedRevision", page.publishedToken.revision},
                {"publishedNamespace", page.publishedToken.key.surfaceId},
                {"publishedContent", page.publishedToken.contentHash.ToHex()}};
        }
        output << record.dump() << '\n';
    }
};

class TerrainCompositedExplicitRequestCollector
{
public:
    static constexpr size_t MaxPages = 8;
    static constexpr uint32_t MaxCaptures = 12;
    bool ArmPlan(const TerrainCompositedRuntime& runtime, const nlohmann::json& plan,
        TerrainRvtBudgetOwner& owner)
    {
        if (lease || !plan.is_object() || plan.value("schema", "") != "rvt-explicit-page-probe-v1" ||
            !plan.contains("pages") || !plan["pages"].is_array() ||
            plan["pages"].empty() || plan["pages"].size() > MaxPages)
            return false;
        auto preparation = owner.TryReserve({0, 65536, 0, 0},
            TerrainRvtCostClass::Workspace, TerrainRvtResourcePhase::Active);
        if (!preparation)
            return false;
        std::vector<TerrainCompositedVTTaskToken> requested;
        requested.reserve(plan["pages"].size());
        for (const auto& page : plan["pages"])
        {
            const auto surface = page.at("surface").get<std::string>();
            if (surface.size() > 256)
                return false;
            const TerrainCompositedVTPageKey key{surface,
                page.at("mip").get<uint32_t>(), page.at("x").get<uint32_t>(),
                page.at("y").get<uint32_t>()};
            const auto token = runtime.Token(key);
            if (page.contains("expectedWorldGeneration") &&
                page["expectedWorldGeneration"].get<uint64_t>() != token.worldGeneration)
                return false;
            requested.push_back(token);
        }
        return Arm(requested, owner);
    }
    bool Arm(const std::vector<TerrainCompositedVTTaskToken>& requested,
        TerrainRvtBudgetOwner& owner)
    {
        if (lease || requested.empty() || requested.size() > MaxPages)
            return false;
        uint64_t strings = 0;
        for (const auto& token : requested)
        {
            if (token.key.surfaceId.size() > 256)
                return false;
            strings += token.key.surfaceId.size() + 1;
        }
        auto admitted = owner.TryReserve(
            {0, sizeof(*this) + 65536 + strings * 8, 0, 0},
            TerrainRvtCostClass::Workspace, TerrainRvtResourcePhase::Active);
        if (!admitted)
            return false;
        std::array<TerrainCompositedVTTaskToken, MaxPages> prepared{};
        std::copy(requested.begin(), requested.end(), prepared.begin());
        tokens.swap(prepared);
        lease = std::move(admitted);
        count = requested.size();
        captures = 0;
        return true;
    }
    bool Capture(const TerrainCompositedRuntime& runtime, uint64_t frame,
        uint64_t completedEpoch, std::ostream& output)
    {
        if (!lease || captures >= MaxCaptures)
            return false;
        ++captures;
        for (size_t index = 0; index < count; ++index)
            TerrainCompositedRequestProbe::Write(output,
                TerrainCompositedRequestProbe::Inspect(runtime, tokens[index], completedEpoch),
                frame, completedEpoch);
        return true;
    }
    void Cancel() noexcept
    {
        for (auto& token : tokens)
            token = {};
        count = captures = 0;
        lease.reset();
    }
    bool MatchesCurrent(const TerrainCompositedRuntime& runtime) const
    {
        for (size_t index = 0; index < count; ++index)
        {
            auto key = tokens[index].key;
            key.surfaceId = runtime.LogicalSurfaceId(key.surfaceId);
            if (!tokens[index].Matches(runtime.Token(key)))
                return false;
        }
        return true;
    }
private:
    std::optional<TerrainRvtBudgetLease> lease;
    std::array<TerrainCompositedVTTaskToken, MaxPages> tokens{};
    size_t count = 0;
    uint32_t captures = 0;
};
}
