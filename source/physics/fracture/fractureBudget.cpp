#include "physics/fracture/fractureBudget.h"

#include <algorithm>
#include <cmath>
#include <stdexcept>

namespace VL::Physics::Fracture
{
namespace
{

float ScreenFootprint(const FractureBudgetCandidate& candidate)
{
    const float distance = std::max(candidate.cameraDistance, 1.0f);
    return candidate.boundingRadius / distance;
}

bool HigherPriority(const FractureBudgetCandidate& first,
    const FractureBudgetCandidate& second)
{
    const float firstFootprint = ScreenFootprint(first);
    const float secondFootprint = ScreenFootprint(second);
    if (firstFootprint != secondFootprint)
        return firstFootprint < secondFootprint;
    if (first.cameraDistance != second.cameraDistance)
        return first.cameraDistance > second.cameraDistance;
    if (first.settledSeconds != second.settledSeconds)
        return first.settledSeconds > second.settledSeconds;
    if (first.owner != second.owner) return first.owner < second.owner;
    if (first.budgetClass != second.budgetClass)
        return first.budgetClass < second.budgetClass;
    return first.shardId < second.shardId;
}

bool Excluded(
    const FractureBudgetRequest& request,
    const FractureBudgetCandidate& candidate)
{
    if (candidate.owner != request.owner) return false;
    return std::find(request.excludeShardIds.begin(),
        request.excludeShardIds.end(), candidate.shardId) !=
        request.excludeShardIds.end();
}

} // namespace

std::size_t FractureBudget::Usage::Total() const
{
    std::size_t result = 0;
    for (const auto& entry : byClass) result += entry.second;
    return result;
}

FractureBudget::FractureBudget()
{
    RegisterClass({"glass.dynamic", "glass.pane", GlassPaneLimit, 0, true});
    RegisterClass({"glass.attached", "glass.pane", GlassPaneLimit, 0, false});
    RegisterClass({"balloon.shard", "balloon", BalloonLimit,
        MinimumBalloonShards, true});
}

void FractureBudget::RegisterClass(const FractureBudgetClass& budgetClass)
{
    if (budgetClass.key.empty() || budgetClass.ownerGroup.empty() ||
        budgetClass.perOwnerLimit == 0)
        throw std::invalid_argument(
            "FractureBudget class requires key, owner group and positive limit");
    classes[budgetClass.key] = {
        budgetClass.ownerGroup, budgetClass.perOwnerLimit,
        budgetClass.floorCount, budgetClass.reclaimable};
}

void FractureBudget::Reset()
{
    usageByOwner.clear();
    reclaimableByOwner.clear();
    totalShards = 0;
}

const FractureBudget::RegisteredClass& FractureBudget::ClassFor(
    const std::string& key) const
{
    const auto found = classes.find(key);
    if (found == classes.end())
        throw std::invalid_argument(
            "Unknown fracture budget class: " + key);
    return found->second;
}

void FractureBudget::OnShardsCreated(
    const std::string& owner, const std::string& budgetClass,
    std::size_t count)
{
    if (count == 0) return;
    ClassFor(budgetClass);
    Usage& usage = usageByOwner[owner];
    usage.byClass[budgetClass] += count;
    totalShards += count;
}

void FractureBudget::OnShardsRemoved(
    const std::string& owner, const std::string& budgetClass,
    std::size_t count)
{
    if (count == 0) return;
    ClassFor(budgetClass);
    const auto found = usageByOwner.find(owner);
    if (found == usageByOwner.end()) return;
    Usage& usage = found->second;
    const auto counter = usage.byClass.find(budgetClass);
    if (counter == usage.byClass.end()) return;
    const std::size_t removed = std::min(counter->second, count);
    counter->second -= removed;
    if (counter->second == 0) usage.byClass.erase(counter);
    totalShards = removed > totalShards ? 0 : totalShards - removed;
    if (usage.Total() == 0) usageByOwner.erase(found);
}

void FractureBudget::UpdateReclaimable(
    const FractureBudgetCandidate& candidate)
{
    if (!ClassFor(candidate.budgetClass).reclaimable) return;
    std::vector<FractureBudgetCandidate>& candidates =
        reclaimableByOwner[candidate.owner];
    for (FractureBudgetCandidate& existing : candidates)
    {
        if (existing.shardId != candidate.shardId) continue;
        existing = candidate;
        return;
    }
    candidates.push_back(candidate);
}

void FractureBudget::UntrackReclaimable(
    const std::string& owner, std::uint64_t shardId)
{
    const auto found = reclaimableByOwner.find(owner);
    if (found == reclaimableByOwner.end()) return;
    std::vector<FractureBudgetCandidate>& candidates = found->second;
    for (auto iterator = candidates.begin(); iterator != candidates.end();
        ++iterator)
    {
        if (iterator->shardId != shardId) continue;
        candidates.erase(iterator);
        break;
    }
    if (candidates.empty()) reclaimableByOwner.erase(found);
}

std::size_t FractureBudget::OwnerShards(const std::string& owner) const
{
    const auto found = usageByOwner.find(owner);
    return found == usageByOwner.end() ? 0 : found->second.Total();
}

std::size_t FractureBudget::OwnerGroupShards(
    const std::string& owner, const std::string& group) const
{
    const auto found = usageByOwner.find(owner);
    if (found == usageByOwner.end()) return 0;
    std::size_t count = 0;
    for (const auto& entry : found->second.byClass)
    {
        const auto registered = classes.find(entry.first);
        if (registered != classes.end() &&
            registered->second.ownerGroup == group)
            count += entry.second;
    }
    return count;
}

std::size_t FractureBudget::ReclaimableShards() const
{
    std::size_t count = 0;
    for (const auto& entry : reclaimableByOwner)
        count += entry.second.size();
    return count;
}

std::vector<FractureBudgetCandidate> FractureBudget::ReclaimableByPriority() const
{
    std::vector<FractureBudgetCandidate> result;
    for (const auto& entry : reclaimableByOwner)
    {
        for (const FractureBudgetCandidate& candidate : entry.second)
        {
            if (candidate.settledSeconds < SettledSecondsToReclaim) continue;
            result.push_back(candidate);
        }
    }
    std::sort(result.begin(), result.end(), HigherPriority);
    return result;
}

FractureBudgetDecision FractureBudget::Request(
    const FractureBudgetRequest& request) const
{
    if (request.count == 0) return FractureBudgetDecision{true, 0, false, false, {}};
    const RegisteredClass& requestedClass = ClassFor(request.budgetClass);

    const std::size_t ownerUsed =
        OwnerGroupShards(request.owner, requestedClass.ownerGroup);
    const std::size_t ownerRemaining = ownerUsed < requestedClass.perOwnerLimit
        ? requestedClass.perOwnerLimit - ownerUsed : 0;
    const std::size_t globalRemaining =
        totalShards < GlobalLimit ? GlobalLimit - totalShards : 0;

    std::vector<FractureBudgetCandidate> reclaimable = ReclaimableByPriority();
    reclaimable.erase(std::remove_if(reclaimable.begin(), reclaimable.end(),
        [&request](const FractureBudgetCandidate& candidate)
        {
            return Excluded(request, candidate);
        }), reclaimable.end());
    std::size_t reclaimableInOwner = 0;
    for (const FractureBudgetCandidate& candidate : reclaimable)
    {
        const auto registered = classes.find(candidate.budgetClass);
        if (candidate.owner == request.owner && registered != classes.end() &&
            registered->second.ownerGroup == requestedClass.ownerGroup)
            ++reclaimableInOwner;
    }

    const std::size_t globalCapacity = globalRemaining + reclaimable.size();
    const std::size_t ownerCapacity = ownerRemaining + reclaimableInOwner;
    std::size_t granted =
        std::min(request.count, std::min(globalCapacity, ownerCapacity));

    if (granted < request.count && request.allowWholePlateDrop)
    {
        FractureBudgetDecision decision;
        decision.dropWholePlate = true;
        return decision;
    }

    if (requestedClass.floorCount > 0)
    {
        const std::size_t floorCount =
            std::min(request.count, requestedClass.floorCount);
        if (granted < floorCount)
        {
            granted = floorCount;
        }
    }

    FractureBudgetDecision decision;
    decision.grantedCount = granted;
    decision.granted = granted > 0;
    decision.exceededLimit =
        requestedClass.floorCount > 0 &&
        granted > std::min(request.count,
            std::min(globalCapacity, ownerCapacity));
    if (granted == 0) return decision;

    const std::size_t requiredGlobal =
        granted > globalRemaining ? granted - globalRemaining : 0;
    const std::size_t requiredOwner =
        granted > ownerRemaining ? granted - ownerRemaining : 0;
    std::size_t evictedTotal = 0;
    std::size_t evictedInOwner = 0;
    for (const FractureBudgetCandidate& candidate : reclaimable)
    {
        if (evictedTotal >= requiredGlobal && evictedInOwner >= requiredOwner)
            break;
        if (evictedTotal >= requiredGlobal && candidate.owner != request.owner)
            continue;
        decision.evictions.push_back(candidate);
        ++evictedTotal;
        if (candidate.owner == request.owner) ++evictedInOwner;
    }
    return decision;
}

FractureBudgetDecision AcquireFractureBudget(
    FractureBudget& budget,
    FractureEvictionDispatcher& dispatcher,
    const FractureBudgetRequest& request)
{
    FractureBudgetDecision decision = budget.Request(request);
    if (!decision.evictions.empty())
        dispatcher.EvictFractureBudgetShards(decision.evictions);
    return decision;
}

} // namespace VL::Physics::Fracture
