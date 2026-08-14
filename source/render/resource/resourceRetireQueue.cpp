#include "render/resource/resourceRetireQueue.h"

#include <algorithm>
#include <type_traits>
#include <utility>

namespace VL
{
namespace
{

static_assert(std::is_nothrow_move_constructible_v<RetiredResource>);
static_assert(std::is_nothrow_move_assignable_v<RetiredResource>);

class CompletedEpochRetirePredicate
{
public:
    explicit CompletedEpochRetirePredicate(uint64_t completedEpoch)
        : completedEpoch(completedEpoch)
    {
    }

    bool operator()(const RetiredResource& resource) const
    {
        return resource.lastUsedEpoch <= completedEpoch;
    }

private:
    uint64_t completedEpoch = 0;
};

} // namespace

void ResourceRetireQueue::Retire(
    std::string debugName,
    uint64_t ownerGeneration,
    uint64_t lastUsedEpoch,
    std::shared_ptr<void> resource)
{
    if (!resource)
    {
        return;
    }

    RetiredResource retiredResource;
    retiredResource.debugName = std::move(debugName);
    retiredResource.ownerGeneration = ownerGeneration;
    retiredResource.lastUsedEpoch = lastUsedEpoch;
    retiredResource.resource = std::move(resource);
    pendingResources.push_back(std::move(retiredResource));
}

PreparedResourceRetirements
ResourceRetireQueue::PrepareRetirements(
    std::vector<RetiredResource> resources) const
{
    PreparedResourceRetirements prepared;
    prepared.resources.reserve(
        pendingResources.size() + resources.size());
    prepared.resources.insert(
        prepared.resources.end(),
        pendingResources.begin(),
        pendingResources.end());
    prepared.additionalResourceOffset =
        prepared.resources.size();
    prepared.resources.insert(
        prepared.resources.end(),
        std::make_move_iterator(resources.begin()),
        std::make_move_iterator(resources.end()));
    return prepared;
}

void ResourceRetireQueue::CommitPreparedRetirements(
    PreparedResourceRetirements retirements) noexcept
{
    const auto emptyBegin = std::remove_if(
        retirements.resources.begin(),
        retirements.resources.end(),
        [](const RetiredResource& resource)
        {
            return !resource.resource;
        });
    retirements.resources.erase(
        emptyBegin,
        retirements.resources.end());
    pendingResources.swap(retirements.resources);
}

void ResourceRetireQueue::MarkFrameSubmitted(uint64_t submittedEpoch)
{
    lastSubmittedEpoch = std::max(lastSubmittedEpoch, submittedEpoch);
}

void ResourceRetireQueue::CollectCompletedEpoch(uint64_t completedEpoch)
{
    lastCompletedEpoch = std::max(lastCompletedEpoch, completedEpoch);

    auto keepBegin = std::remove_if(
        pendingResources.begin(),
        pendingResources.end(),
        CompletedEpochRetirePredicate(lastCompletedEpoch));
    pendingResources.erase(keepBegin, pendingResources.end());
}

void ResourceRetireQueue::ForceReleaseAll()
{
    pendingResources.clear();
    lastCompletedEpoch = std::max(lastCompletedEpoch, lastSubmittedEpoch);
}

std::weak_ptr<void>
ResourceRetireQueue::FindPendingResourceForTest(
    std::string_view debugName,
    uint64_t ownerGeneration) const noexcept
{
    for (const RetiredResource& resource :
         pendingResources)
    {
        if (resource.debugName == debugName &&
            resource.ownerGeneration == ownerGeneration)
        {
            return resource.resource;
        }
    }
    return {};
}

} // namespace VL
