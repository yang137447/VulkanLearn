#include "terrain/terrainRvtBudget.h"

#include <algorithm>
#include <atomic>
#include <mutex>
#include <stdexcept>

namespace VL
{
#if defined(VL_FILE_IO_TEST_HOOKS)
thread_local int64_t TerrainRvtPhaseLockFailureCountdown = -1;
#endif
namespace
{
size_t Index(TerrainRvtCostClass value)
{
    const auto index = static_cast<size_t>(value);
    if (index >= 3)
        throw std::invalid_argument("Invalid RVT cost class");
    return index;
}

size_t Index(TerrainRvtResourcePhase value)
{
    const auto index = static_cast<size_t>(value);
    if (index >= 3)
        throw std::invalid_argument("Invalid RVT resource phase");
    return index;
}

void Add(TerrainRvtCost& target, const TerrainRvtCost& cost)
{
    target.gpuBytes += cost.gpuBytes;
    target.cpuBytes += cost.cpuBytes;
    target.physicalSlots += cost.physicalSlots;
    target.productionTasks += cost.productionTasks;
}

void Subtract(TerrainRvtCost& target, const TerrainRvtCost& cost)
{
    target.gpuBytes -= cost.gpuBytes;
    target.cpuBytes -= cost.cpuBytes;
    target.physicalSlots -= cost.physicalSlots;
    target.productionTasks -= cost.productionTasks;
}

bool Fits(uint64_t used, uint64_t extra, uint64_t cap)
{
    return used <= cap && extra <= cap - used;
}

template<typename Value>
struct MetadataAllocator
{
    using value_type = Value;

    MetadataAllocator() = default;
    template<typename Other>
    MetadataAllocator(const MetadataAllocator<Other>&) {}

    Value* allocate(size_t count)
    {
        if (count > TerrainRvtBudgetOwner::MetadataBytesPerLease / sizeof(Value))
            throw std::bad_alloc();
        return std::allocator<Value>{}.allocate(count);
    }

    void deallocate(Value* allocation, size_t count)
    {
        std::allocator<Value>{}.deallocate(allocation, count);
    }

    template<typename Other>
    bool operator==(const MetadataAllocator<Other>&) const { return true; }
    template<typename Other>
    bool operator!=(const MetadataAllocator<Other>&) const { return false; }
};
}

struct TerrainRvtBudgetState
{
    explicit TerrainRvtBudgetState(TerrainRvtBudgetCaps limits) : caps(limits) {}
    TerrainRvtBudgetCaps caps;
    TerrainRvtBudgetStats stats;
    mutable std::mutex mutex;
    TerrainRvtBudgetCharge* charges = nullptr;
    TerrainRvtResourcePhase phase = TerrainRvtResourcePhase::Candidate;
};

struct TerrainRvtBudgetCharge
{
    std::shared_ptr<TerrainRvtBudgetState> state;
    TerrainRvtCost cost;
    TerrainRvtCost accountedCost;
    TerrainRvtCostClass costClass;
    TerrainRvtResourcePhase phase;
    bool accounted = false;
    TerrainRvtBudgetCharge* previous = nullptr;
    TerrainRvtBudgetCharge* next = nullptr;
    std::optional<TerrainRvtBudgetLease> parentLease;
    std::optional<TerrainRvtBudgetLease> secondParentLease;

    ~TerrainRvtBudgetCharge()
    {
        if (!accounted)
            return;
        std::lock_guard<std::mutex> lock(state->mutex);
        if (previous)
            previous->next = next;
        else
            state->charges = next;
        if (next)
            next->previous = previous;
        Subtract(state->stats.current, accountedCost);
        Subtract(state->stats.byClass[Index(costClass)], accountedCost);
        Subtract(state->stats.byPhase[Index(phase)], accountedCost);
        state->stats.metadataBytes -= TerrainRvtBudgetOwner::MetadataBytesPerLease;
        --state->stats.allocations;
    }

    void SetPhase(TerrainRvtResourcePhase target, bool rejectRegression)
    {
        std::lock_guard<std::mutex> lock(state->mutex);
        SetPhaseLocked(target, rejectRegression);
    }

    void SetPhaseLocked(TerrainRvtResourcePhase target, bool rejectRegression)
    {
        const auto targetIndex = Index(target);
        const auto previousIndex = Index(phase);
        if (targetIndex < previousIndex && rejectRegression)
            throw std::logic_error("RVT resource phase cannot move backwards");
        if (targetIndex <= previousIndex)
            return;
        Subtract(state->stats.byPhase[previousIndex], accountedCost);
        Add(state->stats.byPhase[targetIndex], accountedCost);
        phase = target;
        if (parentLease)
            parentLease->charge->SetPhase(target, false);
        if (secondParentLease)
            secondParentLease->charge->SetPhase(target, false);
    }
};

struct TerrainRvtBudgetPhaseBatch::Impl
{
    struct Entry
    {
        std::shared_ptr<TerrainRvtBudgetCharge> charge;
        TerrainRvtResourcePhase phase = TerrainRvtResourcePhase::Candidate;
    };
    std::optional<TerrainRvtBudgetLease> metadata;
    std::vector<Entry> entries;
    std::vector<TerrainRvtBudgetState*> states;
    std::vector<std::unique_lock<std::mutex>> locks;
    uint64_t metadataBytes = 0;
    bool committed = false;
    std::atomic_flag attempting = ATOMIC_FLAG_INIT;

    void Unlock() noexcept
    {
        for (auto iterator = locks.rbegin(); iterator != locks.rend(); ++iterator)
            if (iterator->owns_lock())
                iterator->unlock();
    }
};

TerrainRvtBudgetPhaseBatch::TerrainRvtBudgetPhaseBatch(std::unique_ptr<Impl> implementation)
    : impl(std::move(implementation))
{
}

TerrainRvtBudgetPhaseBatch::~TerrainRvtBudgetPhaseBatch() = default;
TerrainRvtBudgetPhaseBatch::TerrainRvtBudgetPhaseBatch(TerrainRvtBudgetPhaseBatch&&) noexcept = default;
TerrainRvtBudgetPhaseBatch& TerrainRvtBudgetPhaseBatch::operator=(
    TerrainRvtBudgetPhaseBatch&&) noexcept = default;

std::optional<TerrainRvtBudgetPhaseBatch> TerrainRvtBudgetPhaseBatch::Prepare(
    const std::vector<TerrainRvtBudgetPhaseChange>& changes, TerrainRvtBudgetOwner& metadataOwner)
{
    if (changes.empty() || changes.size() > MaxCharges)
        throw std::invalid_argument("Invalid RVT phase batch size");
    std::array<Impl::Entry, MaxCharges> collected;
    std::array<size_t, MaxCharges * 3> queue{};
    size_t count = 0;
    size_t queueEnd = 0;
    auto collect = [&](const std::shared_ptr<TerrainRvtBudgetCharge>& charge,
        TerrainRvtResourcePhase target) {
        if (!charge)
            throw std::invalid_argument("Moved RVT phase batch lease");
        Index(target);
        size_t index = 0;
        while (index < count && collected[index].charge.get() != charge.get())
            ++index;
        if (index == count)
        {
            if (count == MaxCharges)
                throw std::length_error("RVT phase batch ancestor limit exceeded");
            collected[count++] = {charge, target};
            queue[queueEnd++] = index;
        }
        else if (static_cast<size_t>(target) > static_cast<size_t>(collected[index].phase))
        {
            collected[index].phase = target;
            queue[queueEnd++] = index;
        }
    };
    for (const auto& change : changes)
        collect(change.lease.charge, change.phase);
    for (size_t cursor = 0; cursor < queueEnd; ++cursor)
    {
        const auto entry = collected[queue[cursor]];
        if (entry.charge->parentLease)
            collect(entry.charge->parentLease->charge, entry.phase);
        if (entry.charge->secondParentLease)
            collect(entry.charge->secondParentLease->charge, entry.phase);
    }
    for (size_t index = 0; index < count; ++index)
    {
        const auto& entry = collected[index];
        std::lock_guard<std::mutex> lock(entry.charge->state->mutex);
        if (!entry.charge->accounted || Index(entry.phase) < Index(entry.charge->phase))
            throw std::logic_error("RVT phase batch would regress an ancestor");
    }
    static_assert(sizeof(Impl::Entry) + sizeof(TerrainRvtBudgetState*) +
        sizeof(std::unique_lock<std::mutex>) <= 128);
    const uint64_t bytes = sizeof(Impl) + 512 + count * 128;
    auto metadata = metadataOwner.TryReserve({0, bytes, 0, 0},
        TerrainRvtCostClass::Workspace, TerrainRvtResourcePhase::Candidate);
    if (!metadata)
        return {};
    auto prepared = std::make_unique<Impl>();
    prepared->metadata = std::move(metadata);
    prepared->metadataBytes = bytes;
    prepared->entries.reserve(count);
    prepared->states.reserve(count);
    for (size_t index = 0; index < count; ++index)
    {
        prepared->entries.push_back(std::move(collected[index]));
        prepared->states.push_back(prepared->entries.back().charge->state.get());
    }
    std::sort(prepared->states.begin(), prepared->states.end(), std::less<TerrainRvtBudgetState*>{});
    prepared->states.erase(std::unique(prepared->states.begin(), prepared->states.end()),
        prepared->states.end());
    prepared->locks.reserve(prepared->states.size());
    for (auto* state : prepared->states)
        prepared->locks.emplace_back(state->mutex, std::defer_lock);
    return TerrainRvtBudgetPhaseBatch(std::move(prepared));
}

bool TerrainRvtBudgetPhaseBatch::TryCommitNoThrow() noexcept
{
    if (!impl || impl->attempting.test_and_set(std::memory_order_acquire))
        return false;
    struct Attempt
    {
        std::atomic_flag& flag;
        ~Attempt() { flag.clear(std::memory_order_release); }
    } attempt{impl->attempting};
    if (impl->committed)
        return false;
    try
    {
        for (auto& lock : impl->locks)
        {
#if defined(VL_FILE_IO_TEST_HOOKS)
            if (TerrainRvtPhaseLockFailureCountdown == 0)
            {
                TerrainRvtPhaseLockFailureCountdown = -1;
                throw 0;
            }
            if (TerrainRvtPhaseLockFailureCountdown > 0)
                --TerrainRvtPhaseLockFailureCountdown;
#endif
            if (!lock.try_lock())
            {
                impl->Unlock();
                return false;
            }
        }
    }
    catch (...)
    {
        impl->Unlock();
        return false;
    }
    for (const auto& entry : impl->entries)
    {
        const auto target = static_cast<size_t>(entry.phase);
        const auto previous = static_cast<size_t>(entry.charge->phase);
        if (!entry.charge->accounted || target >= 3 || previous >= 3 || target < previous)
        {
            impl->Unlock();
            return false;
        }
    }
    for (const auto& entry : impl->entries)
    {
        const auto target = static_cast<size_t>(entry.phase);
        const auto previous = static_cast<size_t>(entry.charge->phase);
        if (target != previous)
        {
            Subtract(entry.charge->state->stats.byPhase[previous], entry.charge->accountedCost);
            Add(entry.charge->state->stats.byPhase[target], entry.charge->accountedCost);
            entry.charge->phase = entry.phase;
        }
    }
    impl->committed = true;
    impl->Unlock();
    return true;
}

uint64_t TerrainRvtBudgetPhaseBatch::MetadataBytes() const noexcept
{
    return impl ? impl->metadataBytes : 0;
}

size_t TerrainRvtBudgetPhaseBatch::ChargeCount() const noexcept
{
    return impl ? impl->entries.size() : 0;
}

bool TerrainRvtCost::operator==(const TerrainRvtCost& other) const
{
    return gpuBytes == other.gpuBytes && cpuBytes == other.cpuBytes &&
        physicalSlots == other.physicalSlots && productionTasks == other.productionTasks;
}

TerrainRvtBudgetLease::TerrainRvtBudgetLease(std::shared_ptr<TerrainRvtBudgetCharge> value)
    : charge(std::move(value))
{
}

TerrainRvtCost TerrainRvtBudgetLease::Cost() const
{
    if (!charge)
        throw std::logic_error("Moved RVT budget lease");
    return charge->cost;
}

TerrainRvtResourcePhase TerrainRvtBudgetLease::Phase() const
{
    if (!charge)
        throw std::logic_error("Moved RVT budget lease");
    std::lock_guard<std::mutex> lock(charge->state->mutex);
    return charge->phase;
}

void TerrainRvtBudgetLease::SetPhase(TerrainRvtResourcePhase phase)
{
    Index(phase);
    if (!charge)
        throw std::logic_error("Moved RVT budget lease");
    charge->SetPhase(phase, true);
}

TerrainRvtBudgetOwner::TerrainRvtBudgetOwner(TerrainRvtBudgetCaps frozenCaps,
    std::shared_ptr<TerrainRvtBudgetOwner> outer,
    std::shared_ptr<TerrainRvtBudgetOwner> terrain)
    : state(std::make_shared<TerrainRvtBudgetState>(frozenCaps)), parent(std::move(outer)),
      secondParent(std::move(terrain))
{
}

std::optional<TerrainRvtBudgetLease> TerrainRvtBudgetOwner::TryReserve(
    TerrainRvtCost cost, TerrainRvtCostClass costClass, TerrainRvtResourcePhase phase)
{
    const auto category = Index(costClass);
    Index(phase);
    if (cost == TerrainRvtCost{})
        throw std::invalid_argument("Empty RVT resource reservation");
    std::lock_guard<std::mutex> lock(state->mutex);
    if (static_cast<size_t>(phase) < static_cast<size_t>(state->phase))
        phase = state->phase;
    const auto phaseIndex = Index(phase);
    const auto& caps = state->caps;
    const auto& used = state->stats.current;
    if (state->stats.allocations >= MaxRegistryEntries ||
        !Fits(cost.cpuBytes, MetadataBytesPerLease, caps.totalCpuBytes))
    {
        ++state->stats.rejected;
        return std::nullopt;
    }
    auto accountedCost = cost;
    accountedCost.cpuBytes += MetadataBytesPerLease;
    const std::array<uint64_t, 3> categoryCaps{
        caps.physicalPoolGpuBytes, caps.inputGpuBytes, caps.workspaceGpuBytes};
    const std::array<uint64_t, 3> cpuCaps{
        caps.physicalPoolCpuBytes, caps.inputCpuBytes, caps.workspaceCpuBytes};
    if (!Fits(used.gpuBytes, cost.gpuBytes, caps.totalGpuBytes) ||
        !Fits(used.cpuBytes, accountedCost.cpuBytes, caps.totalCpuBytes) ||
        !Fits(used.physicalSlots, cost.physicalSlots, caps.physicalSlots) ||
        !Fits(used.productionTasks, cost.productionTasks, caps.productionTasks) ||
        !Fits(state->stats.byClass[category].gpuBytes, cost.gpuBytes, categoryCaps[category]) ||
        !Fits(state->stats.byClass[category].cpuBytes, accountedCost.cpuBytes, cpuCaps[category]))
    {
        ++state->stats.rejected;
        return std::nullopt;
    }
    auto charge = std::allocate_shared<TerrainRvtBudgetCharge>(
        MetadataAllocator<TerrainRvtBudgetCharge>{});
    if (parent)
    {
        charge->parentLease = parent->TryReserve(accountedCost, costClass, phase);
        if (!charge->parentLease)
        {
            ++state->stats.rejected;
            return std::nullopt;
        }
    }
    if (secondParent)
    {
        charge->secondParentLease = secondParent->TryReserve(accountedCost, costClass, phase);
        if (!charge->secondParentLease)
        {
            ++state->stats.rejected;
            return std::nullopt;
        }
    }
    charge->state = state;
    charge->cost = cost;
    charge->accountedCost = accountedCost;
    charge->costClass = costClass;
    charge->phase = phase;
    Add(state->stats.current, accountedCost);
    Add(state->stats.byClass[category], accountedCost);
    Add(state->stats.byPhase[phaseIndex], accountedCost);
    auto& peak = state->stats.peak;
    peak.gpuBytes = std::max(peak.gpuBytes, state->stats.current.gpuBytes);
    peak.cpuBytes = std::max(peak.cpuBytes, state->stats.current.cpuBytes);
    peak.physicalSlots = std::max(peak.physicalSlots, state->stats.current.physicalSlots);
    peak.productionTasks = std::max(peak.productionTasks, state->stats.current.productionTasks);
    ++state->stats.allocations;
    state->stats.metadataBytes += MetadataBytesPerLease;
    state->stats.peakMetadataBytes = std::max(state->stats.peakMetadataBytes,
        state->stats.metadataBytes);
    charge->next = state->charges;
    if (charge->next)
        charge->next->previous = charge.get();
    state->charges = charge.get();
    charge->accounted = true;
    return TerrainRvtBudgetLease(std::move(charge));
}

TerrainRvtBudgetStats TerrainRvtBudgetOwner::Statistics() const
{
    std::lock_guard<std::mutex> lock(state->mutex);
    return state->stats;
}

TerrainRvtBudgetCaps TerrainRvtBudgetOwner::Caps() const
{
    return state->caps;
}

std::shared_ptr<const void> TerrainRvtBudgetOwner::IdentityToken() const
{
    return std::shared_ptr<const void>(state, state.get());
}

void TerrainRvtBudgetOwner::SetPhase(TerrainRvtResourcePhase phase)
{
    Index(phase);
    std::lock_guard<std::mutex> lock(state->mutex);
    if (static_cast<size_t>(phase) > static_cast<size_t>(state->phase))
        state->phase = phase;
    for (auto entry = state->charges; entry; entry = entry->next)
        entry->SetPhaseLocked(phase, false);
}

#if defined(VL_FILE_IO_TEST_HOOKS)
void TerrainRvtBudgetTestAccess::WithStateLock(
    const TerrainRvtBudgetOwner& owner, const std::function<void()>& callback)
{
    std::lock_guard<std::mutex> lock(owner.state->mutex);
    callback();
}

void TerrainRvtBudgetTestAccess::FailTryLockAfter(size_t acquired)
{
    TerrainRvtPhaseLockFailureCountdown = static_cast<int64_t>(acquired);
}
#endif
}
