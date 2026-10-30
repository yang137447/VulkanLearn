#include "physics/fracture/fractureBudget.h"

#include <algorithm>
#include <cmath>

namespace VL::Physics::Fracture
{
namespace
{

// 屏幕占比的近似：包围球半径 / 相机距离。不引入真正的投影计算，
// 只用于“先回收看不见的小碎片”这一启发式。
float ScreenFootprint(const FractureBudgetCandidate& candidate)
{
    const float distance = std::max(candidate.cameraDistance, 1.0f);
    return candidate.boundingRadius / distance;
}

// 回收顺序：屏幕占比小 → 距离远 → 静止更久 → owner/id（保证可复现）。
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
    return first.shardId < second.shardId;
}

} // namespace

void FractureBudget::Reset()
{
    usageByOwner.clear();
    reclaimableByOwner.clear();
    totalShards = 0;
}

std::size_t& FractureBudget::CounterFor(
    Usage& usage, FractureBudgetKind kind)
{
    switch (kind)
    {
    case FractureBudgetKind::GlassAttached:
        return usage.glassAttached;
    case FractureBudgetKind::BalloonShard:
        return usage.balloonShards;
    case FractureBudgetKind::GlassDynamic:
    default:
        return usage.glassDynamic;
    }
}

void FractureBudget::OnShardsCreated(
    const std::string& owner, FractureBudgetKind kind, std::size_t count)
{
    if (count == 0) return;
    Usage& usage = usageByOwner[owner];
    CounterFor(usage, kind) += count;
    totalShards += count;
}

void FractureBudget::OnShardsRemoved(
    const std::string& owner, FractureBudgetKind kind, std::size_t count)
{
    if (count == 0) return;
    const auto found = usageByOwner.find(owner);
    if (found == usageByOwner.end()) return;
    Usage& usage = found->second;
    std::size_t& counter = CounterFor(usage, kind);
    const std::size_t removed = std::min(counter, count);
    counter -= removed;
    totalShards = removed > totalShards ? 0 : totalShards - removed;
    if (usage.Total() == 0) usageByOwner.erase(found);
}

void FractureBudget::UpdateReclaimable(
    const FractureBudgetCandidate& candidate)
{
    // attached 板片是玻璃结构的一部分，永远不能作为回收候选。
    if (candidate.kind == FractureBudgetKind::GlassAttached) return;
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
    FractureBudgetDecision decision;
    if (request.count == 0)
    {
        decision.granted = true;
        return decision;
    }

    const std::size_t ownerTotal = OwnerShards(request.owner);
    const std::size_t ownerLimit =
        request.kind == FractureBudgetKind::BalloonShard
            ? BalloonLimit
            : GlassPaneLimit;
    const std::size_t ownerRemaining =
        ownerTotal < ownerLimit ? ownerLimit - ownerTotal : 0;
    const std::size_t globalRemaining =
        totalShards < GlobalLimit ? GlobalLimit - totalShards : 0;

    // 只有“已落地静止足够久”的动态碎片能被回收；attached 板片不在候选里。
    const std::vector<FractureBudgetCandidate> reclaimable =
        ReclaimableByPriority();
    std::size_t reclaimableInOwner = 0;
    for (const FractureBudgetCandidate& candidate : reclaimable)
        if (candidate.owner == request.owner) ++reclaimableInOwner;

    const std::size_t globalCapacity = globalRemaining + reclaimable.size();
    const std::size_t ownerCapacity = ownerRemaining + reclaimableInOwner;
    std::size_t granted =
        std::min(request.count, std::min(globalCapacity, ownerCapacity));

    // 玻璃 attached 板片不存在“少生成一点”的合理形态：要么切碎，要么整块掉落。
    if (granted < request.count &&
        request.kind == FractureBudgetKind::GlassAttached &&
        request.allowWholePlateDrop)
    {
        decision.dropWholePlate = true;
        return decision;
    }

    // 气球爆裂至少保留 MinimumBalloonShards 片，避免“炸开以后什么都不剩”。
    if (request.kind == FractureBudgetKind::BalloonShard)
    {
        const std::size_t floorCount =
            std::min(request.count, MinimumBalloonShards);
        if (granted < floorCount)
        {
            granted = floorCount;
            decision.exceededLimit = true;
        }
    }

    decision.grantedCount = granted;
    decision.granted = granted > 0;
    if (granted == 0) return decision;

    // 需要回收多少片：全局与单对象容量都要补足。只取静止候选，
    // 因此“刚落地”的碎片不会被瞬间抹掉。
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
        // 全局额度已经补足时，不再牺牲其他对象的碎片去凑单对象额度。
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
