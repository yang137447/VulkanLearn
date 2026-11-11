#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace VL
{

struct RetiredResource
{
    std::string debugName;
    uint64_t ownerGeneration = 0;
    uint64_t lastUsedEpoch = 0;
    std::shared_ptr<void> resource;
};

struct PreparedResourceRetirements
{
    RetiredResource& GetAdditionalResource(size_t index) noexcept
    {
        return resources[additionalResourceOffset + index];
    }

    const RetiredResource& GetAdditionalResource(size_t index) const noexcept
    {
        return resources[additionalResourceOffset + index];
    }

private:
    friend class ResourceRetireQueue;

    std::vector<RetiredResource> resources;
    size_t additionalResourceOffset = 0;
};

// Holds renderer resources until the GPU has completed the frame epoch that
// could still reference them. Existing resource destructors still own the
// Vulkan destroy calls; this queue controls when their last shared_ptr can drop.
class ResourceRetireQueue
{
public:
    static ResourceRetireQueue& GetInstance()
    {
        static ResourceRetireQueue instance;
        return instance;
    }

    template <typename T>
    void RetireShared(
        std::string debugName,
        uint64_t ownerGeneration,
        uint64_t lastUsedEpoch,
        std::shared_ptr<T> resource)
    {
        if (!resource)
        {
            return;
        }

        Retire(
            std::move(debugName),
            ownerGeneration,
            lastUsedEpoch,
            std::static_pointer_cast<void>(std::move(resource)));
    }

    void Retire(
        std::string debugName,
        uint64_t ownerGeneration,
        uint64_t lastUsedEpoch,
        std::shared_ptr<void> resource);
    PreparedResourceRetirements PrepareRetirements(
        std::vector<RetiredResource> resources) const;
    void CommitPreparedRetirements(
        PreparedResourceRetirements retirements) noexcept;

    void MarkFrameSubmitted(uint64_t submittedEpoch);
    void CollectCompletedEpoch(uint64_t completedEpoch);
    void ForceReleaseAll();

    // Conservative floor for new retirements: GT may publish a snapshot that
    // the render thread has not resolved yet, so resources referenced by it
    // must not be retired at the real submitted epoch.
    void SetPendingRenderFrames(size_t frames);

    uint64_t GetLastSubmittedEpoch() const;
    uint64_t GetLastCompletedEpoch() const;
    size_t GetPendingCount() const;
    std::weak_ptr<void> FindPendingResourceForTest(
        std::string_view debugName,
        uint64_t ownerGeneration) const noexcept;

private:
    ResourceRetireQueue() = default;

    uint64_t lastSubmittedEpoch = 0;
    uint64_t lastCompletedEpoch = 0;
    size_t pendingRenderFrames = 0;
    std::vector<RetiredResource> pendingResources;
    mutable std::mutex mutex;
};

} // namespace VL
