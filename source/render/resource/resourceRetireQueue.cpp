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
    std::lock_guard<std::mutex> lock(mutex);
    pendingResources.push_back(std::move(retiredResource));
}

PreparedResourceRetirements
ResourceRetireQueue::PrepareRetirements(
    std::vector<RetiredResource> resources) const
{
    std::lock_guard<std::mutex> lock(mutex);
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
    {
        std::lock_guard<std::mutex> lock(mutex);
        pendingResources.swap(retirements.resources);
    }
    // swap 到参数里的旧 pending 在锁外析构，避免 allocator/Vulkan 析构路径
    // 在持有退休队列锁时运行。
}

void ResourceRetireQueue::MarkFrameSubmitted(uint64_t submittedEpoch)
{
    std::lock_guard<std::mutex> lock(mutex);
    lastSubmittedEpoch = std::max(lastSubmittedEpoch, submittedEpoch);
}

void ResourceRetireQueue::SetPendingRenderFrames(size_t frames)
{
    std::lock_guard<std::mutex> lock(mutex);
    pendingRenderFrames = frames;
}

void ResourceRetireQueue::CollectCompletedEpoch(uint64_t completedEpoch)
{
    std::vector<RetiredResource> keep;
    std::vector<RetiredResource> released;
    {
        std::lock_guard<std::mutex> lock(mutex);
        lastCompletedEpoch = std::max(lastCompletedEpoch, completedEpoch);

        keep.reserve(pendingResources.size());
        released.reserve(pendingResources.size());
        const CompletedEpochRetirePredicate canRetire(lastCompletedEpoch);
        for (RetiredResource& resource : pendingResources)
        {
            if (canRetire(resource))
            {
                released.push_back(std::move(resource));
            }
            else
            {
                keep.push_back(std::move(resource));
            }
        }
        pendingResources.swap(keep);
    }
    // 最后一个 shared_ptr 必须在这里释放，而不是在 mutex 内。
    keep.clear();
    released.clear();
}

void ResourceRetireQueue::ForceReleaseAll()
{
    std::vector<RetiredResource> released;
    {
        std::lock_guard<std::mutex> lock(mutex);
        released.swap(pendingResources);
        lastCompletedEpoch = std::max(lastCompletedEpoch, lastSubmittedEpoch);
    }
    released.clear();
}

std::weak_ptr<void>
ResourceRetireQueue::FindPendingResourceForTest(
    std::string_view debugName,
    uint64_t ownerGeneration) const noexcept
{
    std::lock_guard<std::mutex> lock(mutex);
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

uint64_t ResourceRetireQueue::GetLastSubmittedEpoch() const
{
    std::lock_guard<std::mutex> lock(mutex);
    // Published-but-unresolved snapshots may still reference resources that GT
    // is about to retire. Report a conservative floor; MarkFrameSubmitted and
    // CollectCompletedEpoch keep using the real epochs.
    return lastSubmittedEpoch +
        static_cast<uint64_t>(pendingRenderFrames);
}

uint64_t ResourceRetireQueue::GetLastCompletedEpoch() const
{
    std::lock_guard<std::mutex> lock(mutex);
    return lastCompletedEpoch;
}

size_t ResourceRetireQueue::GetPendingCount() const
{
    std::lock_guard<std::mutex> lock(mutex);
    return pendingResources.size();
}

} // namespace VL
