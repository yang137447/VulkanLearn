#include "terrain/terrainRvtScheduler.h"

#include <algorithm>
#include <limits>
#include <stdexcept>
#include <tuple>

namespace VL
{
namespace
{
uint64_t SaturatingAdd(uint64_t value, uint64_t extra)
{
    return extra > std::numeric_limits<uint64_t>::max() - value
        ? std::numeric_limits<uint64_t>::max() : value + extra;
}
}

bool TerrainRvtSurfaceIdentity::operator==(const TerrainRvtSurfaceIdentity& other) const
{
    return std::tie(worldGeneration, revision, snapshotIdentity, contentIdentity, surfaceId) ==
        std::tie(other.worldGeneration, other.revision, other.snapshotIdentity,
            other.contentIdentity, other.surfaceId);
}

bool TerrainRvtSurfaceIdentity::operator<(const TerrainRvtSurfaceIdentity& other) const
{
    return std::tie(worldGeneration, revision, snapshotIdentity, contentIdentity, surfaceId) <
        std::tie(other.worldGeneration, other.revision, other.snapshotIdentity,
            other.contentIdentity, other.surfaceId);
}

bool TerrainRvtPageIdentity::operator==(const TerrainRvtPageIdentity& other) const
{
    return surface == other.surface && std::tie(mip, x, y) == std::tie(other.mip, other.x, other.y);
}

bool TerrainRvtPageIdentity::operator<(const TerrainRvtPageIdentity& other) const
{
    return std::tie(surface, mip, x, y) < std::tie(other.surface, other.mip, other.x, other.y);
}

TerrainRvtScheduler::TerrainRvtScheduler(TerrainRvtSchedulerConfig frozenConfig)
    : config(frozenConfig)
{
    if (!config.physicalSlots || !config.productionSlots || !config.maxTrackedPages ||
        !config.maxAttempts || !config.retryBaseTicks ||
        config.retryMaxTicks < config.retryBaseTicks || !config.agePromotionTicks ||
        config.freeSlotReserve >= config.physicalSlots)
        throw std::invalid_argument("Invalid explicit RVT scheduler configuration");
    slots.resize(config.physicalSlots);
    production.resize(config.productionSlots, false);
}

bool TerrainRvtScheduler::Current(const TerrainRvtSurfaceIdentity& identity) const
{
    const auto found = identities.find(identity.surfaceId);
    return accepting && found != identities.end() && found->second == identity;
}

void TerrainRvtScheduler::SetSurfaceIdentity(const TerrainRvtSurfaceIdentity& identity)
{
    if (!accepting || !identity.worldGeneration || identity.snapshotIdentity.empty() ||
        identity.contentIdentity.empty() || identity.surfaceId.empty())
        throw std::invalid_argument("Invalid RVT surface identity");
    const auto previous = identities.find(identity.surfaceId);
    if (previous != identities.end() && previous->second == identity)
        return;
    if (previous != identities.end() &&
        (identity.worldGeneration < previous->second.worldGeneration ||
         (identity.worldGeneration == previous->second.worldGeneration &&
          identity.revision < previous->second.revision)))
        throw std::invalid_argument("RVT surface identity cannot move backwards");
    if (previous == identities.end() && identities.size() >= config.maxTrackedPages)
        throw std::runtime_error("RVT surface identity capacity exceeded");
    identities.insert_or_assign(identity.surfaceId, identity);
    for (auto iterator = entries.begin(); iterator != entries.end();)
    {
        if (iterator->first.surface.surfaceId != identity.surfaceId ||
            iterator->first.surface == identity ||
            iterator->second.state == TerrainRvtTaskState::Resident)
        {
            ++iterator;
            continue;
        }
        Cancel(iterator->second);
        if (!iterator->second.task)
            iterator = entries.erase(iterator);
        else
            ++iterator;
    }
}

bool TerrainRvtScheduler::Request(
    const TerrainRvtPageIdentity& page, bool root, bool coarseAncestor, uint64_t now)
{
    if (!Current(page.surface))
        return false;
    const auto found = entries.find(page);
    if (found != entries.end())
    {
        if (found->second.root != root)
            throw std::invalid_argument("RVT root classification changed");
        found->second.coarseAncestor = found->second.coarseAncestor || coarseAncestor;
        found->second.lastUsed = std::max(now, found->second.lastUsed);
        return found->second.state != TerrainRvtTaskState::Failed &&
            found->second.state != TerrainRvtTaskState::Stale;
    }
    if (entries.size() >= config.maxTrackedPages)
    {
        ++rejected;
        return false;
    }
    if (root)
    {
        uint32_t currentRoots = 0;
        for (const auto& item : entries)
        {
            if (item.first.surface == page.surface && item.second.root)
                throw std::invalid_argument("Duplicate RVT surface root");
            if (item.second.root && Current(item.first.surface))
                ++currentRoots;
        }
        if (currentRoots >= config.physicalSlots - config.freeSlotReserve)
        {
            ++rejected;
            return false;
        }
    }
    Entry entry;
    entry.root = root;
    entry.coarseAncestor = coarseAncestor;
    entry.queuedAt = entry.lastUsed = now;
    entries.emplace(page, std::move(entry));
    return true;
}

bool TerrainRvtScheduler::HasRoot(const TerrainRvtSurfaceIdentity& identity) const
{
    for (const auto& item : entries)
        if (item.first.surface == identity && item.second.root &&
            item.second.state == TerrainRvtTaskState::Resident)
            return true;
    return false;
}

uint64_t TerrainRvtScheduler::Rank(const Entry& entry, uint64_t now) const
{
    const uint64_t age = now > entry.queuedAt ? now - entry.queuedAt : 0;
    return SaturatingAdd(age / config.agePromotionTicks,
        entry.root ? 2 : (entry.coarseAncestor ? 1 : 0));
}

std::optional<TerrainRvtProductionTask> TerrainRvtScheduler::Begin(
    uint64_t now, uint64_t completedEpoch)
{
    Collect(completedEpoch);
    if (!accepting)
        return std::nullopt;
    const auto productionSlot = std::find(production.begin(), production.end(), false);
    const auto physicalSlot = std::find_if(slots.begin(), slots.end(),
        [](const Slot& slot) { return slot.state == TerrainRvtSlotState::Free; });
    if (productionSlot == production.end() || physicalSlot == slots.end())
        return std::nullopt;
    const auto freeCount = std::count_if(slots.begin(), slots.end(),
        [](const Slot& slot) { return slot.state == TerrainRvtSlotState::Free; });
    auto chosen = entries.end();
    for (auto iterator = entries.begin(); iterator != entries.end(); ++iterator)
    {
        const auto& entry = iterator->second;
        if (!Current(iterator->first.surface) ||
            (entry.state != TerrainRvtTaskState::Queued &&
             entry.state != TerrainRvtTaskState::FailedBackoff) ||
            now < entry.retryAt || (!entry.root && !HasRoot(iterator->first.surface)) ||
            (!entry.root && freeCount <= config.freeSlotReserve))
            continue;
        if (chosen == entries.end() || Rank(entry, now) > Rank(chosen->second, now) ||
            (Rank(entry, now) == Rank(chosen->second, now) &&
             entry.queuedAt < chosen->second.queuedAt))
            chosen = iterator;
    }
    if (chosen == entries.end())
        return std::nullopt;
    if (serial == std::numeric_limits<uint64_t>::max())
        throw std::overflow_error("RVT task serial exhausted");
    TerrainRvtProductionTask task{++serial, chosen->first,
        static_cast<uint32_t>(physicalSlot - slots.begin()),
        static_cast<uint32_t>(productionSlot - production.begin())};
    chosen->second.task = task;
    chosen->second.state = TerrainRvtTaskState::Producing;
    chosen->second.submittedEpoch = 0;
    ++chosen->second.attempts;
    physicalSlot->page = chosen->first;
    physicalSlot->state = TerrainRvtSlotState::Reserved;
    *productionSlot = true;
    return task;
}

TerrainRvtScheduler::Entries::iterator TerrainRvtScheduler::Find(
    const TerrainRvtProductionTask& task)
{
    const auto found = entries.find(task.page);
    if (found == entries.end() || !found->second.task ||
        found->second.task->serial != task.serial ||
        found->second.task->physicalSlot != task.physicalSlot ||
        found->second.task->productionSlot != task.productionSlot)
        return entries.end();
    return found;
}

bool TerrainRvtScheduler::Submitted(const TerrainRvtProductionTask& task, uint64_t epoch)
{
    const auto found = Find(task);
    if (found == entries.end() || found->second.state != TerrainRvtTaskState::Producing ||
        !Current(task.page.surface) || epoch <= completed)
        return false;
    found->second.submittedEpoch = epoch;
    found->second.state = TerrainRvtTaskState::AwaitingFence;
    return true;
}

void TerrainRvtScheduler::ReleaseTask(Entry& entry)
{
    if (!entry.task)
        return;
    production[entry.task->productionSlot] = false;
    slots[entry.task->physicalSlot] = {};
    entry.task.reset();
}

void TerrainRvtScheduler::Fail(
    Entry& entry, TerrainRvtProductionResult result, uint64_t now)
{
    ReleaseTask(entry);
    if (result == TerrainRvtProductionResult::PermanentFailure ||
        entry.attempts >= config.maxAttempts)
    {
        entry.state = TerrainRvtTaskState::Failed;
        return;
    }
    uint64_t delay = config.retryBaseTicks;
    for (uint32_t attempt = 1; attempt < entry.attempts && delay < config.retryMaxTicks; ++attempt)
        delay = std::min(config.retryMaxTicks, SaturatingAdd(delay, delay));
    entry.retryAt = SaturatingAdd(now, delay);
    entry.state = TerrainRvtTaskState::FailedBackoff;
}

bool TerrainRvtScheduler::Complete(const TerrainRvtProductionTask& task,
    uint64_t completedEpoch, TerrainRvtProductionResult result, uint64_t now)
{
    const auto found = Find(task);
    if (found == entries.end())
    {
        ++staleResults;
        return false;
    }
    auto& entry = found->second;
    if ((entry.state != TerrainRvtTaskState::AwaitingFence &&
         entry.state != TerrainRvtTaskState::Stale) ||
        entry.submittedEpoch > completedEpoch)
        return false;
    completed = std::max(completed, completedEpoch);
    if (!Current(task.page.surface) || entry.state == TerrainRvtTaskState::Stale)
    {
        ++staleResults;
        ReleaseTask(entry);
        entries.erase(found);
        return false;
    }
    if (result != TerrainRvtProductionResult::Success)
    {
        Fail(entry, result, now);
        return true;
    }
    entry.state = TerrainRvtTaskState::ReadyToPublish;
    return true;
}

bool TerrainRvtScheduler::AbortUnsubmitted(
    const TerrainRvtProductionTask& task, uint64_t now)
{
    const auto found = Find(task);
    if (found == entries.end() || found->second.state != TerrainRvtTaskState::Producing)
        return false;
    Fail(found->second, TerrainRvtProductionResult::RetryableFailure, now);
    return true;
}

bool TerrainRvtScheduler::CancelRequest(const TerrainRvtPageIdentity& page)
{
    const auto found = entries.find(page);
    if (found == entries.end() || found->second.state == TerrainRvtTaskState::Resident)
        return false;
    Cancel(found->second);
    if (!found->second.task)
        entries.erase(found);
    return true;
}

bool TerrainRvtScheduler::Publish(
    const TerrainRvtProductionTask& task, uint64_t pageTableEpoch)
{
    const auto found = Find(task);
    if (found == entries.end() || found->second.state != TerrainRvtTaskState::ReadyToPublish ||
        !Current(task.page.surface) || pageTableEpoch <= completed ||
        pageTableEpoch <= found->second.submittedEpoch)
        return false;
    auto& entry = found->second;
    entry.state = TerrainRvtTaskState::Resident;
    entry.publishedEpoch = pageTableEpoch;
    slots[task.physicalSlot].state = TerrainRvtSlotState::Resident;
    production[task.productionSlot] = false;
    return true;
}

void TerrainRvtScheduler::Cancel(Entry& entry)
{
    if (entry.task && entry.state == TerrainRvtTaskState::AwaitingFence)
    {
        entry.state = TerrainRvtTaskState::Stale;
        auto& slot = slots[entry.task->physicalSlot];
        slot.state = TerrainRvtSlotState::Retiring;
        slot.retireEpoch = entry.submittedEpoch;
        return;
    }
    if (entry.state == TerrainRvtTaskState::Stale && entry.task)
        return;
    ReleaseTask(entry);
    entry.state = TerrainRvtTaskState::Stale;
}

void TerrainRvtScheduler::Collect(uint64_t completedEpoch)
{
    completed = std::max(completed, completedEpoch);
    for (auto iterator = entries.begin(); iterator != entries.end();)
    {
        if (iterator->second.state == TerrainRvtTaskState::Stale &&
            iterator->second.task && iterator->second.submittedEpoch <= completed)
        {
            ReleaseTask(iterator->second);
            iterator = entries.erase(iterator);
        }
        else
            ++iterator;
    }
    for (auto& slot : slots)
        if (slot.state == TerrainRvtSlotState::Retiring && slot.retireEpoch <= completed)
            slot = {};
}

bool TerrainRvtScheduler::Touch(const TerrainRvtPageIdentity& page, uint64_t now)
{
    const auto found = entries.find(page);
    if (found == entries.end() || found->second.state != TerrainRvtTaskState::Resident)
        return false;
    found->second.lastUsed = std::max(now, found->second.lastUsed);
    return true;
}

std::optional<uint32_t> TerrainRvtScheduler::EvictionCandidate() const
{
    std::optional<uint32_t> chosen;
    uint64_t oldest = std::numeric_limits<uint64_t>::max();
    for (uint32_t index = 0; index < slots.size(); ++index)
    {
        const auto& slot = slots[index];
        if (slot.state != TerrainRvtSlotState::Resident)
            continue;
        const auto& entry = entries.at(*slot.page);
        if (!entry.root && HasRoot(slot.page->surface) &&
            (!chosen || entry.lastUsed < oldest))
        {
            chosen = index;
            oldest = entry.lastUsed;
        }
    }
    return chosen;
}

bool TerrainRvtScheduler::CommitEviction(uint32_t slotIndex, uint64_t unmapEpoch)
{
    if (slotIndex >= slots.size())
        return false;
    auto& slot = slots[slotIndex];
    if (slot.state != TerrainRvtSlotState::Resident)
        return false;
    const auto found = entries.find(*slot.page);
    if (found->second.root || !HasRoot(slot.page->surface) ||
        unmapEpoch <= completed || unmapEpoch <= found->second.publishedEpoch)
        return false;
    entries.erase(found);
    slot.state = TerrainRvtSlotState::Retiring;
    slot.retireEpoch = unmapEpoch;
    return true;
}

bool TerrainRvtScheduler::RetireReplacedIdentity(
    const TerrainRvtSurfaceIdentity& identity, uint64_t unmapEpoch)
{
    const auto current = identities.find(identity.surfaceId);
    if (current == identities.end() || current->second == identity ||
        !HasRoot(current->second) || unmapEpoch <= completed)
        return false;
    for (const auto& item : entries)
        if (item.first.surface == identity && item.second.state == TerrainRvtTaskState::Resident &&
            unmapEpoch <= item.second.publishedEpoch)
            return false;
    for (auto iterator = entries.begin(); iterator != entries.end();)
    {
        if (!(iterator->first.surface == identity) ||
            iterator->second.state != TerrainRvtTaskState::Resident)
        {
            ++iterator;
            continue;
        }
        auto& slot = slots[iterator->second.task->physicalSlot];
        slot.state = TerrainRvtSlotState::Retiring;
        slot.retireEpoch = unmapEpoch;
        iterator = entries.erase(iterator);
    }
    return true;
}

void TerrainRvtScheduler::RetireAll(uint64_t unmapEpoch)
{
    if (unmapEpoch <= completed)
        throw std::invalid_argument("RVT shutdown requires a future unmap epoch");
    for (const auto& item : entries)
        if (item.second.state == TerrainRvtTaskState::Resident &&
            unmapEpoch <= item.second.publishedEpoch)
            throw std::invalid_argument("RVT unmap precedes publication");
    accepting = false;
    for (auto iterator = entries.begin(); iterator != entries.end();)
    {
        auto& entry = iterator->second;
        if (entry.state == TerrainRvtTaskState::Resident)
        {
            auto& slot = slots[entry.task->physicalSlot];
            slot.state = TerrainRvtSlotState::Retiring;
            slot.retireEpoch = unmapEpoch;
            iterator = entries.erase(iterator);
        }
        else
        {
            Cancel(entry);
            if (entry.task)
                ++iterator;
            else
                iterator = entries.erase(iterator);
        }
    }
}

TerrainRvtSchedulerStats TerrainRvtScheduler::Statistics() const
{
    TerrainRvtSchedulerStats result;
    for (const auto& slot : slots)
        switch (slot.state)
        {
        case TerrainRvtSlotState::Free: ++result.free; break;
        case TerrainRvtSlotState::Reserved: ++result.reserved; break;
        case TerrainRvtSlotState::Resident: ++result.resident; break;
        case TerrainRvtSlotState::Retiring: ++result.retiring; break;
        }
    for (const auto& item : entries)
        result.roots += item.second.root ? 1 : 0;
    result.productionInUse = static_cast<uint32_t>(
        std::count(production.begin(), production.end(), true));
    result.tracked = static_cast<uint32_t>(entries.size());
    result.rejected = rejected;
    result.staleResults = staleResults;
    return result;
}

std::optional<TerrainRvtTaskState> TerrainRvtScheduler::State(
    const TerrainRvtPageIdentity& page) const
{
    const auto found = entries.find(page);
    return found == entries.end() ? std::nullopt : std::optional<TerrainRvtTaskState>(found->second.state);
}

TerrainRvtSlotState TerrainRvtScheduler::SlotState(uint32_t slot) const
{
    return slots.at(slot).state;
}

TerrainRvtTaskState TerrainRvtScheduler::Status(const TerrainRvtPageIdentity& page) const
{
    return State(page).value_or(TerrainRvtTaskState::Absent);
}
}
