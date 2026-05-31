#include "render/resource/resourceRetireQueue.h"

#include <algorithm>
#include <utility>

namespace VL
{
namespace
{

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

} // namespace VL
