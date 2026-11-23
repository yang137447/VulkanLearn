#include "terrain/terrainCompositedResidency.h"

#include <algorithm>
#include <chrono>
#include <stdexcept>
#include <iostream>

namespace VL
{
struct TerrainCompositedResidencyPool::SlotReuseWatch
{
    TerrainRvtBudgetLease metadata;
    TerrainCompositedSlotReuseDiagnostics observation;
    explicit SlotReuseWatch(TerrainRvtBudgetLease lease) : metadata(std::move(lease)) {}
};

TerrainCompositedResidencyPool::TerrainCompositedResidencyPool(uint32_t capacity, uint32_t reserve)
    : slots(capacity), freeReserve(reserve)
{
    if (!capacity || reserve >= capacity)
        throw std::invalid_argument("Empty World composited residency pool");
}

TerrainCompositedResidencyPool::~TerrainCompositedResidencyPool() = default;

std::optional<TerrainCompositedSlotDiagnostics> TerrainCompositedResidencyPool::SlotDiagnostics(
    uint32_t index, uint64_t completedEpoch) const
{
    if (index >= slots.size())
        return {};
    const auto& slot = slots[index];
    TerrainCompositedSlotDiagnostics result;
    result.slot = index;
    result.state = static_cast<TerrainCompositedDiagnosticSlotState>(slot.state);
    result.reservation = slot.reservation;
    result.reservedToken = slot.reservedToken;
    result.publishedToken = slot.publishedToken;
    result.publishEpoch = slot.publishEpoch;
    result.retireEpoch = slot.retireEpoch;
    result.referenceEpoch = slot.referenceEpoch;
    result.completedEpoch = completedEpoch;
    result.retirementOwnerAlive = !slot.retirementBinding.expired();
    result.retirementReusable = slot.state == SlotState::Retiring &&
        slot.retireEpoch <= completedEpoch && !result.retirementOwnerAlive;
    return result;
}

bool TerrainCompositedResidencyPool::ArmSlotReuseDiagnostics(
    uint32_t index, TerrainRvtBudgetOwner& budgetOwner)
{
    if (slotReuseWatch || index >= slots.size() ||
        slots[index].state == SlotState::Free || !slots[index].reservation)
        return false;
    const auto& slot = slots[index];
    const uint64_t bytes = sizeof(SlotReuseWatch) + 512 +
        4 * (slot.reservedToken.key.surfaceId.size() + slot.publishedToken.key.surfaceId.size());
    auto metadata = budgetOwner.TryReserve({0, bytes, 0, 0},
        TerrainRvtCostClass::Workspace, TerrainRvtResourcePhase::Active);
    if (!metadata)
        return false;
    auto prepared = std::make_unique<SlotReuseWatch>(std::move(*metadata));
    prepared->observation.original = *SlotDiagnostics(index, 0);
    slotReuseWatch = std::move(prepared);
    return true;
}

void TerrainCompositedResidencyPool::CancelSlotReuseDiagnostics() noexcept
{
    slotReuseWatch.reset();
}

std::optional<TerrainCompositedSlotReuseDiagnostics>
TerrainCompositedResidencyPool::SlotReuseDiagnostics(uint64_t completedEpoch) const
{
    if (!slotReuseWatch)
        return {};
    auto result = slotReuseWatch->observation;
    result.current = *SlotDiagnostics(result.original.slot, completedEpoch);
    result.replacementStillPresent = result.replacementObserved &&
        result.current.reservation == result.replacementReservation &&
        result.current.state != TerrainCompositedDiagnosticSlotState::Free;
    return result;
}

void TerrainCompositedResidencyPool::CollectReusable(uint64_t completedEpoch)
{
    for (uint32_t index = 0; index < slots.size(); ++index)
    {
        auto& slot = slots[index];
        if (slot.state != SlotState::Retiring)
            continue;
        if (slotReuseWatch && slotReuseWatch->observation.original.slot == index &&
            slotReuseWatch->observation.original.reservation == slot.reservation)
        {
            auto& observed = slotReuseWatch->observation;
            observed.retirementObserved = true;
            ++observed.boundaryChecks;
            observed.lastBoundaryCompletedEpoch = completedEpoch;
            observed.blockedByEpoch += slot.retireEpoch > completedEpoch;
            observed.blockedByOwner += !slot.retirementBinding.expired();
            if (slot.retireEpoch <= completedEpoch && slot.retirementBinding.expired())
            {
                observed.releasedAtBoundary = true;
                observed.releaseCompletedEpoch = completedEpoch;
                observed.releaseRetireEpoch = slot.retireEpoch;
                observed.releaseReferenceEpoch = slot.referenceEpoch;
            }
        }
        if (slot.retireEpoch <= completedEpoch && slot.retirementBinding.expired())
            slot = {};
    }
}

void TerrainCompositedResidencyPool::ObserveReservation(
    uint32_t index, uint64_t completedEpoch) noexcept
{
    if (!slotReuseWatch)
        return;
    auto& observed = slotReuseWatch->observation;
    if (observed.original.slot == index && observed.releasedAtBoundary &&
        !observed.replacementObserved &&
        slots[index].reservation != observed.original.reservation)
    {
        observed.replacementObserved = true;
        observed.replacementReservation = slots[index].reservation;
        observed.replacementCompletedEpoch = completedEpoch;
    }
}

uint32_t TerrainCompositedResidencyPool::Capacity() const
{
    return static_cast<uint32_t>(slots.size());
}

uint32_t TerrainCompositedResidencyPool::UnassignedRoots() const
{
    return static_cast<uint32_t>(std::count_if(rootNamespaces.begin(), rootNamespaces.end(),
        [&](const auto& id) {
            return std::none_of(slots.begin(), slots.end(), [&](const auto& slot) {
                return slot.key.surfaceId == id && slot.root &&
                    (slot.state == SlotState::Resident || slot.state == SlotState::Uploading);
            });
        }));
}

TerrainCompositedResidency::TerrainCompositedResidency(
    std::shared_ptr<const TerrainCompositedVTStore> source,
    const TerrainCompositedVTBuildDesc& surface,
    uint64_t generation, uint64_t revision,
    TerrainCompositedResidencyBudget limits,
    std::shared_ptr<TerrainCompositedResidencyPool> sharedPool,
    const std::string& namespaceId)
    : store(std::move(source)),
      layout(namespaceId.empty() ? surface.surfaceId : namespaceId,
          surface.width, surface.height, surface.addressMode), budget(limits),
      baseline{layout.SurfaceId(), layout.MipCount() - 1, 0, 0},
      pageBytes(surface.PageBytes()),
      pool(sharedPool ? std::move(sharedPool) :
          std::make_shared<TerrainCompositedResidencyPool>(limits.slots)),
      slots(pool->slots)
{
    storeSurfaceId = surface.surfaceId;
    if (pool->Capacity() != budget.slots)
        throw std::invalid_argument("World residency capacity differs from frozen budget");
    if (!budget.slots || !budget.concurrentReads || !budget.queuedRequests ||
        !budget.uploadsPerFrame || !budget.maxReadAttempts || !pageBytes ||
        budget.cpuBytes < pageBytes || budget.uploadBytesPerFrame < pageBytes)
        throw std::invalid_argument("Invalid composited VT residency budget");
    identity = {generation, revision, surface.ContentHash(), baseline};
    stats.feedbackTargetMipCounts.resize(layout.MipCount(), 0);
    stats.feedbackEstimatedResolvedMipCounts.resize(layout.MipCount(), 0);
    if (store)
    {
        const auto found = std::find_if(store->Surfaces().begin(), store->Surfaces().end(),
            [&](const auto& candidate) {
                return candidate.surfaceId == surface.surfaceId &&
                    candidate.ContentHash() == identity.contentHash;
            });
        auto storeRoot = baseline;
        storeRoot.surfaceId = storeSurfaceId;
        if (found == store->Surfaces().end() || store->Pages().count(storeRoot) == 0)
            throw std::invalid_argument("Composited VT surface/baseline absent from store");
    }
    else
    {
        if (budget.slots < 2)
            throw std::invalid_argument("Preview requires at least two physical slots");
        preview = true;
        dirty.insert(baseline);
        previewChanged.insert(baseline);
    }
    if (!pool->rootNamespaces.insert(layout.SurfaceId()).second)
        throw std::invalid_argument("Duplicate residency namespace in shared pool");
    if (pool->rootNamespaces.size() + pool->freeReserve > pool->slots.size())
    {
        pool->rootNamespaces.erase(layout.SurfaceId());
        throw std::runtime_error("World pool cannot reserve all roots and free watermark");
    }
    Request(baseline);
}

TerrainCompositedResidency::~TerrainCompositedResidency()
{
    pool->rootNamespaces.erase(layout.SurfaceId());
}

void TerrainCompositedResidency::SeedReadOnlyBaseline()
{
    if (!store || preview || hasSubmitted || active.id || !reads.empty())
        throw std::logic_error("Read-only baseline seeding requires a fresh Store residency");
    if (!ready.count(baseline))
    {
        try
        {
            auto storeRoot = baseline;
            storeRoot.surfaceId = storeSurfaceId;
            auto bytes = store->ReadPage(storeRoot);
            if (bytes.size() != pageBytes || (ready.size() + 1) * pageBytes > budget.cpuBytes)
                throw std::runtime_error("Read-only baseline payload exceeds CPU budget or has wrong size");
            ready.emplace(baseline,
                std::make_shared<const std::vector<uint8_t>>(std::move(bytes)));
            ++stats.readsStarted;
        }
        catch (const std::exception& error)
        {
            previewChanged.insert(baseline);
            pending.insert(baseline);
            std::cerr << "[TerrainVT] root payload unavailable; queued GPU regeneration: "
                << baseline.surfaceId << " " << error.what() << std::endl;
        }
    }
    if (ready.count(baseline))
        pending.erase(baseline);
    baselineSeeded = true;
}

TerrainCompositedVTTaskToken TerrainCompositedResidency::Token(
    const TerrainCompositedVTPageKey& key) const
{
    auto token = identity;
    token.key = key;
    return token;
}

bool TerrainCompositedResidency::IsResident(const TerrainCompositedVTPageKey& key) const
{
    return std::any_of(slots.begin(), slots.end(), [&](const auto& slot) {
        return slot.state == SlotState::Resident && slot.key == key;
    });
}

void TerrainCompositedResidency::Request(const TerrainCompositedVTPageKey& key)
{
    if (!Needed(key))
        return;
    if ((IsResident(key) && !dirty.count(key)) || ready.count(key) ||
        reads.count(key) || pending.count(key) || produced.count(key) ||
        std::any_of(slots.begin(), slots.end(), [&](const auto& slot) {
            return slot.state == SlotState::Uploading && slot.key == key;
        }))
        return;
    const auto attempt = attempts.find(key);
    if (attempt != attempts.end() && attempt->second >= budget.maxReadAttempts)
        return;
    if (pending.size() >= budget.queuedRequests)
    {
        ++stats.throttledRequests;
        return;
    }
    pending.insert(key);
}

void TerrainCompositedResidency::ConsumeFeedback(
    const std::vector<TerrainCompositedVTTaskToken>& requests)
{
    if (active.id)
        throw std::logic_error("Cannot change feedback selection during an upload transaction");
    ++clock;
    std::set<TerrainCompositedVTPageKey> targets;
    for (const auto& request : requests)
    {
        if (!request.Matches(Token(request.key)) || !layout.Contains(request.key) ||
            request.key.mip >= layout.MipCount())
        {
            ++stats.discardedResults;
            continue;
        }
        ++stats.feedbackRequests;
        ++stats.feedbackTargetMipCounts[request.key.mip];
        // fence 保证回读已完成；这里只解析消费时的 CPU 映射，不能还原采样时的 GPU 页表。
        const auto resolved = ResolveMapping(request.key, false);
        if (!resolved)
            ++stats.feedbackEstimatedMisses;
        else
        {
            const auto& resolvedKey = slots[*resolved].key;
            ++stats.feedbackEstimatedResolvedMipCounts[resolvedKey.mip];
            if (resolvedKey == request.key)
                ++stats.feedbackEstimatedExactHits;
            else
                ++stats.feedbackEstimatedParentFallbacks;
            if (dirty.count(resolvedKey))
                ++stats.feedbackEstimatedStaleHits;
        }
        targets.insert(request.key);
    }
    if (targets.empty() && !requests.empty())
        return;
    const size_t selectedLimit = std::max(1u, budget.slots - 1);
    budgetSelected = targets;
    budgetSelected.insert(baseline);
    stats.budgetMipBias = 0;
    while (budgetSelected.size() > selectedLimit)
    {
        std::set<TerrainCompositedVTPageKey> parents{baseline};
        for (const auto& key : budgetSelected)
            parents.insert(key == baseline ? baseline : layout.Parent(key));
        budgetSelected = std::move(parents);
        ++stats.budgetMipBias;
    }
    hasFeedbackSelection = true;
    stats.budgetSelectedPages = static_cast<uint32_t>(budgetSelected.size());
    for (auto iterator = pending.begin(); iterator != pending.end();)
        if (!Needed(*iterator))
        {
            iterator = pending.erase(iterator);
            ++stats.queuedCancel;
        }
        else
            ++iterator;
    for (auto iterator = ready.begin(); iterator != ready.end();)
        if (!Needed(iterator->first))
        {
            iterator = ready.erase(iterator);
            ++stats.evictedReads;
        }
        else
            ++iterator;
    for (auto iterator = attempts.begin(); iterator != attempts.end();)
        if (!Needed(iterator->first))
        {
            retryAfter.erase(iterator->first);
            iterator = attempts.erase(iterator);
        }
        else
            ++iterator;
    for (auto& slot : slots)
        if (slot.key.surfaceId == layout.SurfaceId())
        {
            slot.selected = budgetSelected.count(slot.key) != 0;
            if (slot.state == SlotState::Resident && slot.selected)
                slot.lastUsed = ++pool->clock;
        }
    for (const auto& key : budgetSelected)
        Request(key);
}

bool TerrainCompositedResidency::Needed(const TerrainCompositedVTPageKey& key) const
{
    return !retired && layout.Contains(key) &&
        (key == baseline || !hasFeedbackSelection || budgetSelected.count(key) != 0);
}

void TerrainCompositedResidency::InvalidatePreview(
    const TerrainCompositedVTBuildDesc& surface, uint64_t generation, uint64_t revision,
    const std::vector<TerrainCompositedVTPageKey>& dirtyPages)
{
    if (active.id)
        throw std::logic_error("Cannot invalidate a recorded composited VT transaction");
    const auto proposed = surface.Layout();
    if (generation != identity.worldGeneration || revision < identity.revision ||
        proposed.SurfaceId() != layout.SurfaceId() ||
        proposed.Mip(0).width != layout.Mip(0).width ||
        proposed.Mip(0).height != layout.Mip(0).height ||
        proposed.AddressMode() != layout.AddressMode() ||
        surface.PageBytes() != pageBytes || budget.slots < 2)
        throw std::invalid_argument("Preview requires matching World/layout and replacement capacity");
    const auto contentHash = surface.ContentHash();
    if (revision == identity.revision && contentHash != identity.contentHash)
        throw std::invalid_argument("Composited VT content changed without a new revision");
    for (const auto& key : dirtyPages)
        if (!layout.Contains(key))
            throw std::invalid_argument("Invalid preview dirty page");
    if (revision != identity.revision || contentHash != identity.contentHash)
    {
        identity.revision = revision;
        identity.contentHash = contentHash;
        ready.clear();
        attempts.clear();
        retryAfter.clear();
    }
    preview = true;
    pending.clear();
    stats.baselineFailed = false;
    for (const auto& key : dirtyPages)
    {
        dirty.insert(key);
        previewChanged.insert(key);
        ready.erase(key);
    }
    if (!IsResident(baseline) || dirty.count(baseline))
        Request(baseline);
    for (const auto& key : budgetSelected)
        Request(key);
}

void TerrainCompositedResidency::SubmitPreviewPages(
    const TerrainCompositedVTBuildDesc& surface, uint64_t generation, uint64_t revision,
    const std::vector<TerrainCompositedVTPageData>& pages)
{
    if (active.id || generation != identity.worldGeneration ||
        revision != identity.revision || surface.ContentHash() != identity.contentHash || !preview)
        throw std::invalid_argument("Stale/unregistered composited VT preview result");
    uint64_t incoming = 0;
    std::set<TerrainCompositedVTPageKey> unique;
    for (const auto& page : pages)
    {
        if (!layout.Contains(page.key) ||
            page.bytes.size() != pageBytes || !unique.insert(page.key).second)
            throw std::invalid_argument("Invalid composited VT preview payload");
        if (Needed(page.key) && !ready.count(page.key))
            incoming += pageBytes;
    }
    if ((reads.size() + ready.size()) * pageBytes + incoming > budget.cpuBytes)
        throw std::runtime_error("Composited VT preview CPU budget exhausted");
    for (const auto& page : pages)
    {
        dirty.insert(page.key);
        previewChanged.insert(page.key);
        if (!Needed(page.key))
        {
            ++stats.evictedReads;
            continue;
        }
        ready[page.key] = std::make_shared<const std::vector<uint8_t>>(page.bytes);
        pending.erase(page.key);
    }
}

void TerrainCompositedResidency::PollReads()
{
    for (auto iterator = reads.begin(); iterator != reads.end();)
    {
        auto& read = iterator->second;
        const auto completedKey = iterator->first;
        if (read.result.wait_for(std::chrono::seconds(0)) != std::future_status::ready)
        {
            ++iterator;
            continue;
        }
        try
        {
            auto bytes = read.result.get();
            if (!Needed(read.token.key))
                ++stats.evictedReads;
            else if (previewChanged.count(read.token.key) || !read.token.Matches(Token(read.token.key)))
                ++stats.discardedResults;
            else if (bytes.size() != pageBytes)
                throw std::runtime_error("Composited VT page payload size mismatch");
            else
            {
                ready.emplace(iterator->first,
                    std::make_shared<const std::vector<uint8_t>>(std::move(bytes)));
                attempts.erase(iterator->first);
                retryAfter.erase(iterator->first);
            }
        }
        catch (...)
        {
            if (!Needed(read.token.key) || previewChanged.count(read.token.key) ||
                !read.token.Matches(Token(read.token.key)))
            {
                ++stats.discardedResults;
                iterator = reads.erase(iterator);
                if (dirty.count(completedKey))
                    Request(completedKey);
                continue;
            }
            ++stats.readFailures;
            if (Needed(iterator->first) && attempts[iterator->first] < budget.maxReadAttempts)
            {
                pending.insert(iterator->first);
                retryAfter[iterator->first] = clock +
                    (uint64_t(1) << std::min(6u, attempts[iterator->first]));
            }
            else if (iterator->first == baseline)
                stats.baselineFailed = true;
        }
        iterator = reads.erase(iterator);
        if (dirty.count(completedKey))
            Request(completedKey);
    }
}

void TerrainCompositedResidency::StartReads()
{
    while (!pending.empty() && reads.size() < budget.concurrentReads &&
        (reads.size() + ready.size() + 1) * pageBytes <= budget.cpuBytes)
    {
        auto selected = pending.end();
        for (auto iterator = pending.begin(); iterator != pending.end(); ++iterator)
            if (store && store->HasPackagedPage(
                    {storeSurfaceId, iterator->mip, iterator->x, iterator->y}) &&
                !previewChanged.count(*iterator) &&
                (!retryAfter.count(*iterator) || retryAfter.at(*iterator) <= clock) &&
                (selected == pending.end() || iterator->mip > selected->mip))
                selected = iterator;
        if (selected == pending.end())
            break;
        const auto key = *selected;
        pending.erase(selected);
        if (IsResident(key) || ready.count(key) || reads.count(key))
            continue;
        auto token = Token(key);
        auto retainedStore = store;
        auto storeKey = key;
        storeKey.surfaceId = storeSurfaceId;
        auto future = std::async(std::launch::async, [retainedStore, storeKey]() {
            return retainedStore->ReadPage(storeKey);
        });
        reads.emplace(key, Read{token, std::move(future)});
        ++attempts[key];
        ++stats.readsStarted;
    }
}

std::vector<TerrainCompositedVTTaskToken> TerrainCompositedResidency::PreviewRequests() const
{
    std::vector<TerrainCompositedVTTaskToken> result;
    for (const auto& key : pending)
        if (!store || previewChanged.count(key))
            result.push_back(Token(key));
    return result;
}

std::vector<TerrainCompositedVTTaskToken> TerrainCompositedResidency::ProductionRequests() const
{
    std::vector<TerrainCompositedVTTaskToken> result;
    for (const auto& key : pending)
        if (Needed(key) && (!store || !store->HasPackagedPage(
                {storeSurfaceId, key.mip, key.x, key.y}) || previewChanged.count(key)) &&
            (!retryAfter.count(key) || retryAfter.at(key) <= clock) &&
            (!attempts.count(key) || attempts.at(key) < budget.maxReadAttempts))
            result.push_back(Token(key));
    return result;
}

std::optional<TerrainCompositedPageUpload> TerrainCompositedResidency::ReserveProduction(
    const TerrainCompositedVTTaskToken& token, uint64_t completedEpoch)
{
    if (active.id || !token.Matches(Token(token.key)) || !pending.count(token.key) || !Needed(token.key))
        return {};
    pool->CollectReusable(completedEpoch);
    const auto freeCount = std::count_if(slots.begin(), slots.end(),
        [](const auto& slot) { return slot.state == SlotState::Free; });
    if (freeCount == 0 || (!(token.key == baseline) &&
        freeCount <= pool->freeReserve + pool->UnassignedRoots()))
        return {};
    auto found = std::find_if(slots.begin(), slots.end(),
        [](const auto& slot) { return slot.state == SlotState::Free; });
    found->state = SlotState::Uploading;
    found->key = token.key;
    found->root = token.key == baseline;
    found->lastUsed = ++pool->clock;
    found->selected = budgetSelected.count(token.key) != 0;
    found->reservedToken = token;
    found->reservation = ++pool->nextReservation;
    pool->ObserveReservation(static_cast<uint32_t>(found - slots.begin()), completedEpoch);
    pending.erase(token.key);
    ++attempts[token.key];
    return TerrainCompositedPageUpload{token, static_cast<uint32_t>(found - slots.begin()),
        {}, found->reservation};
}

bool TerrainCompositedResidency::OwnsProduction(const TerrainCompositedPageUpload& page) const
{
    return layout.Contains(page.token.key) && page.slot < slots.size() &&
        slots[page.slot].state == SlotState::Uploading &&
        slots[page.slot].reservation == page.reservation &&
        page.token.Matches(slots[page.slot].reservedToken);
}

TerrainCompositedPageDiagnostics TerrainCompositedResidency::Diagnostics(
    const TerrainCompositedVTTaskToken& token, uint64_t completedEpoch) const
{
    TerrainCompositedPageDiagnostics result;
    result.maxAttempts = budget.maxReadAttempts;
    result.completedEpoch = completedEpoch;
    result.schedulingTick = clock;
    result.pageQueued = pending.count(token.key) != 0;
    result.pageReading = reads.count(token.key) != 0;
    result.pageReady = ready.count(token.key) != 0;
    result.pageProduced = produced.count(token.key) != 0;
    result.queued = static_cast<uint32_t>(pending.size());
    result.tokenCurrent = layout.Contains(token.key) && token.Matches(Token(token.key));
    const auto attempt = attempts.find(token.key);
    if (attempt != attempts.end())
        result.attempts = attempt->second;
    const auto retry = retryAfter.find(token.key);
    if (retry != retryAfter.end())
        result.retryAfterTick = retry->second;
    if (retry != retryAfter.end() && retry->second > clock)
        result.backoffTicks = retry->second - clock;
    for (uint32_t index = 0; index < slots.size(); ++index)
    {
        const auto& slot = slots[index];
        result.freeSlots += slot.state == SlotState::Free;
        result.retiringSlots += slot.state == SlotState::Retiring;
        if (!(slot.key == token.key) || slot.state == SlotState::Free)
            continue;
        const bool matches = slot.state == SlotState::Resident ?
            token.Matches(slot.publishedToken) : token.Matches(slot.reservedToken);
        if (result.slot != UINT32_MAX && result.slotTokenMatches && !matches)
            continue;
        result.slot = index;
        result.slotState = static_cast<uint32_t>(slot.state);
        result.reservation = slot.reservation;
        result.publishEpoch = slot.publishEpoch;
        result.retireEpoch = slot.retireEpoch;
        result.referenceEpoch = slot.referenceEpoch;
        result.retirementOwnerAlive = !slot.retirementBinding.expired();
        result.retirementReusable = slot.state == SlotState::Retiring &&
            slot.retireEpoch <= completedEpoch && !result.retirementOwnerAlive;
        result.slotTokenMatches = matches;
        result.reservedToken = slot.reservedToken;
        result.publishedToken = slot.publishedToken;
    }
    return result;
}

TerrainCompositedResidencyProgressDiagnostics TerrainCompositedResidency::ProgressDiagnostics(
    const std::vector<TerrainCompositedVTPageKey>* requiredPages, uint64_t completedEpoch) const
{
    TerrainCompositedResidencyProgressDiagnostics result;
    result.worldGeneration = identity.worldGeneration;
    result.revision = identity.revision;
    result.queued = static_cast<uint32_t>(pending.size());
    result.reading = static_cast<uint32_t>(reads.size());
    result.ready = static_cast<uint32_t>(ready.size());
    result.produced = static_cast<uint32_t>(produced.size());
    result.retired = retired;
    const auto current = [&](const TerrainCompositedVTPageKey& key) {
        return !retired && layout.Contains(key) && !dirty.count(key) &&
            std::any_of(slots.begin(), slots.end(), [&](const auto& slot) {
                return slot.state == SlotState::Resident && slot.key == key &&
                    slot.publishedToken.worldGeneration == identity.worldGeneration &&
                    slot.publishedToken.revision == identity.revision &&
                    slot.publishedToken.contentHash == identity.contentHash &&
                    !slot.unmapping && slot.publishEpoch <= completedEpoch;
            });
    };
    result.rootCurrent = current(baseline);
    const auto count = [&](const TerrainCompositedVTPageKey& key) {
        ++result.requiredPages;
        result.currentPages += current(key);
    };
    if (requiredPages)
        for (const auto& key : *requiredPages)
            count(key);
    else
    {
        for (const auto& key : budgetSelected)
            count(key);
        if (!retired && !budgetSelected.count(baseline))
            count(baseline);
    }
    return result;
}

bool TerrainCompositedResidency::CompleteProduction(
    const TerrainCompositedPageUpload& page, uint64_t epoch, bool success)
{
    if (!OwnsProduction(page) || produced.count(page.token.key))
        return false;
    slots[page.slot].retireEpoch = std::max(slots[page.slot].retireEpoch, epoch);
    const bool current = page.token.Matches(Token(page.token.key)) && Needed(page.token.key);
    if (success && current)
    {
        produced.emplace(page.token.key, page);
        return true;
    }
    slots[page.slot].state = SlotState::Retiring;
    slots[page.slot].retireEpoch = std::max(
        slots[page.slot].retireEpoch, slots[page.slot].referenceEpoch);
    if (current)
    {
        ++stats.readFailures;
        retryAfter[page.token.key] = clock + (uint64_t(1) << std::min(6u, attempts[page.token.key]));
        Request(page.token.key);
    }
    else
    {
        ++stats.discardedResults;
        Request(page.token.key);
    }
    return false;
}

std::vector<TerrainCompositedSlotOwner> TerrainCompositedResidency::CommittedOwners() const
{
    std::vector<TerrainCompositedSlotOwner> result;
    result.reserve(slots.size());
    for (uint32_t index = 0; index < slots.size(); ++index)
        result.push_back({index, slots[index].state == SlotState::Resident, slots[index].publishedToken});
    return result;
}

TerrainCompositedResidencyBatch TerrainCompositedResidency::BeginUploads(uint64_t completedEpoch)
{
    return BeginUploads(completedEpoch, budget.uploadBytesPerFrame, budget.uploadsPerFrame);
}

TerrainCompositedResidencyBatch TerrainCompositedResidency::BeginUploads(
    uint64_t completedEpoch, uint64_t byteAllowance, uint32_t pageAllowance)
{
    if (active.id)
        throw std::logic_error("Composited VT upload transaction already active");
    const uint64_t allowedBytes = std::min(byteAllowance, budget.uploadBytesPerFrame);
    const uint32_t allowedPages = std::min(pageAllowance, budget.uploadsPerFrame);
    if (!allowedPages || allowedBytes < pageBytes)
    {
        active.id = ++nextBatch;
        return active;
    }
    ++clock;
    pool->CollectReusable(completedEpoch);
    PollReads();
    StartReads();
    active.id = ++nextBatch;
    for (auto iterator = produced.begin(); iterator != produced.end();)
    {
        const auto& page = *iterator;
        if (!page.second.token.Matches(Token(page.first)) || !Needed(page.first))
        {
            const auto key = page.first;
            slots[page.second.slot].state = SlotState::Retiring;
            slots[page.second.slot].retireEpoch = std::max(
                slots[page.second.slot].retireEpoch, completedEpoch);
            iterator = produced.erase(iterator);
            Request(key);
            ++stats.discardedResults;
            continue;
        }
        if (active.uploads.size() >= allowedPages)
            break;
        if (slots[page.second.slot].retireEpoch > completedEpoch)
        {
            ++iterator;
            continue;
        }
        active.uploads.push_back(page.second);
        ++iterator;
    }
    if (!ProductionRequests().empty() &&
        std::count_if(slots.begin(), slots.end(), [](const auto& slot) {
            return slot.state == SlotState::Free;
        }) <= pool->freeReserve + pool->UnassignedRoots())
    {
        auto victim = slots.end();
        for (auto iterator = slots.begin(); iterator != slots.end(); ++iterator)
            if (iterator->state == SlotState::Resident && !iterator->root &&
                !iterator->selected && !iterator->unmapping &&
                (victim == slots.end() || iterator->lastUsed < victim->lastUsed))
                victim = iterator;
        if (victim != slots.end())
        {
            victim->unmapping = true;
            active.removals.push_back(victim->key);
        }
    }
    for (const auto& item : ready)
    {
        if (!Needed(item.first))
            continue;
        if (active.uploads.size() >= allowedPages ||
            (active.uploads.size() + 1) * pageBytes > allowedBytes)
            break;
        auto freeSlot = std::find_if(slots.begin(), slots.end(),
            [](const auto& slot) { return slot.state == SlotState::Free; });
        const auto freeCount = std::count_if(slots.begin(), slots.end(),
            [](const auto& slot) { return slot.state == SlotState::Free; });
        if (freeSlot == slots.end() ||
            (!(item.first == baseline) &&
                freeCount <= pool->freeReserve + pool->UnassignedRoots()))
        {
            auto victim = slots.end();
            for (auto iterator = slots.begin(); iterator != slots.end(); ++iterator)
                if (iterator->state == SlotState::Resident && !iterator->root && !iterator->unmapping &&
                    !iterator->selected &&
                    std::find(active.removals.begin(), active.removals.end(), iterator->key) ==
                        active.removals.end() &&
                    (victim == slots.end() || iterator->lastUsed < victim->lastUsed))
                    victim = iterator;
            if (victim != slots.end())
            {
                victim->unmapping = true;
                active.removals.push_back(victim->key);
            }
            break;
        }
        freeSlot->state = SlotState::Uploading;
        freeSlot->key = item.first;
        freeSlot->lastUsed = ++pool->clock;
        freeSlot->root = item.first == baseline;
        freeSlot->selected = budgetSelected.count(item.first) != 0;
        freeSlot->reservedToken = Token(item.first);
        freeSlot->reservation = ++pool->nextReservation;
        pool->ObserveReservation(static_cast<uint32_t>(freeSlot - slots.begin()), completedEpoch);
        active.uploads.push_back({Token(item.first),
            static_cast<uint32_t>(freeSlot - slots.begin()), item.second, freeSlot->reservation});
    }
    return active;
}

void TerrainCompositedResidency::Commit(uint64_t batchId, uint64_t submittedEpoch)
{
    const bool loadingCommit = baselineSeeded && !hasSubmitted && submittedEpoch == 0 &&
        active.removals.empty() && (active.uploads.empty() ||
            (active.uploads.size() == 1 && active.uploads.front().token.key == baseline));
    if (!active.id || active.id != batchId ||
        (!loadingCommit && submittedEpoch <= lastSubmitted))
        throw std::logic_error("Invalid composited VT upload commit/epoch");
    for (const auto& key : active.removals)
        for (auto& slot : slots)
            if (slot.state == SlotState::Resident && slot.key == key)
            {
                slot.state = SlotState::Retiring;
                slot.retireEpoch = std::max(slot.referenceEpoch, submittedEpoch);
                slot.unmapping = false;
            }
    for (const auto& upload : active.uploads)
    {
        for (uint32_t index = 0; index < slots.size(); ++index)
            if (index != upload.slot && slots[index].state == SlotState::Resident &&
                slots[index].key == upload.token.key)
            {
                slots[index].state = SlotState::Retiring;
                slots[index].retireEpoch = std::max(slots[index].referenceEpoch, submittedEpoch);
            }
        slots[upload.slot].state = SlotState::Resident;
        slots[upload.slot].publishedToken = upload.token;
        slots[upload.slot].publishEpoch = submittedEpoch;
        slots[upload.slot].referenceEpoch = submittedEpoch;
        ready.erase(upload.token.key);
        produced.erase(upload.token.key);
        attempts.erase(upload.token.key);
        retryAfter.erase(upload.token.key);
        if (upload.token.Matches(Token(upload.token.key)))
            dirty.erase(upload.token.key);
        ++stats.uploads;
    }
    lastSubmitted = submittedEpoch;
    hasSubmitted = true;
    active = {};
}

void TerrainCompositedResidency::Abort(uint64_t batchId)
{
    if (!active.id || active.id != batchId)
        throw std::logic_error("Invalid composited VT upload abort");
    for (const auto& upload : active.uploads)
        if (upload.bytes)
            slots[upload.slot] = {};
    for (const auto& key : active.removals)
        for (auto& slot : slots)
            if (slot.key == key)
                slot.unmapping = false;
    active = {};
}

bool TerrainCompositedResidency::Ready() const { return IsResident(baseline); }

void TerrainCompositedResidency::RetireAll(uint64_t epoch, std::weak_ptr<void> bindingLifetime)
{
    if (!CanRetire())
        throw std::logic_error("Cannot retire a recorded residency transaction");
    RetireAllNoThrow(epoch, std::move(bindingLifetime));
}

bool TerrainCompositedResidency::CanRetire() const noexcept
{
    return active.id == 0;
}

void TerrainCompositedResidency::RetireAllNoThrow(
    uint64_t epoch, std::weak_ptr<void> bindingLifetime) noexcept
{
    retired = true;
    pool->rootNamespaces.erase(layout.SurfaceId());
    for (auto& slot : slots)
        if (slot.key.surfaceId == layout.SurfaceId() && slot.state != SlotState::Free)
        {
            if (slot.state != SlotState::Uploading || produced.count(slot.key))
                slot.state = SlotState::Retiring;
            slot.retireEpoch = std::max({slot.retireEpoch, slot.referenceEpoch, epoch});
            slot.retirementBinding = bindingLifetime;
            slot.unmapping = false;
        }
    pending.clear();
    ready.clear();
    produced.clear();
    budgetSelected.clear();
}

void TerrainCompositedResidency::MarkReferenced(uint64_t epoch)
{
    for (auto& slot : slots)
        if (slot.key.surfaceId == layout.SurfaceId() && slot.state != SlotState::Free)
        {
            slot.referenceEpoch = std::max(slot.referenceEpoch, epoch);
            if (slot.state == SlotState::Retiring)
                slot.retireEpoch = std::max(slot.retireEpoch, epoch);
        }
}

bool TerrainCompositedResidency::Retired() const
{
    return retired && std::none_of(slots.begin(), slots.end(), [&](const auto& slot) {
        return slot.key.surfaceId == layout.SurfaceId() && slot.state != SlotState::Free;
    });
}

bool TerrainCompositedResidency::AllCurrent(
    const std::vector<TerrainCompositedVTPageKey>& pages, uint64_t completedEpoch) const
{
    if (retired)
        return false;
    for (const auto& key : pages)
        if (!layout.Contains(key) || std::none_of(slots.begin(), slots.end(), [&](const auto& slot) {
            return slot.state == SlotState::Resident && slot.key == key &&
                slot.publishedToken.Matches(Token(key)) && !slot.unmapping &&
                slot.publishEpoch <= completedEpoch && !dirty.count(key);
        }))
            return false;
    return true;
}

std::vector<TerrainCompositedVTPageKey> TerrainCompositedResidency::SelectedPages() const
{
    auto selected = budgetSelected;
    if (!retired)
        selected.insert(baseline);
    return {selected.begin(), selected.end()};
}
bool TerrainCompositedResidency::CurrentRoot() const
{
    return Ready() && !dirty.count(baseline);
}

std::optional<uint32_t> TerrainCompositedResidency::Resolve(
    const TerrainCompositedVTPageKey& requested)
{
    return ResolveMapping(requested, true);
}

std::optional<uint32_t> TerrainCompositedResidency::ResolveMapping(
    const TerrainCompositedVTPageKey& requested, bool touch)
{
    if (!layout.Contains(requested))
        return {};
    auto key = requested;
    for (;;)
    {
        for (uint32_t index = 0; index < slots.size(); ++index)
            if (slots[index].state == SlotState::Resident && slots[index].key == key)
            {
                if (touch && Needed(key))
                    slots[index].lastUsed = ++pool->clock;
                if (!(key == requested))
                    ++stats.parentFallbacks;
                return index;
            }
        if (key.mip + 1 >= layout.MipCount())
            break;
        key = layout.Parent(key);
    }
    ++stats.misses;
    return {};
}

TerrainCompositedResidencyStats TerrainCompositedResidency::Statistics() const
{
    auto result = stats;
    result.cpuBytes = (reads.size() + ready.size()) * pageBytes;
    result.pending = static_cast<uint32_t>(pending.size());
    result.reading = static_cast<uint32_t>(reads.size());
    result.dirty = static_cast<uint32_t>(dirty.size());
    for (const auto& slot : slots)
    {
        if (slot.key.surfaceId != layout.SurfaceId())
            continue;
        result.resident += slot.state == SlotState::Resident;
        result.retiring += slot.state == SlotState::Retiring;
        result.stale += slot.state == SlotState::Resident && dirty.count(slot.key) != 0;
    }
    return result;
}
}
