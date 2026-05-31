#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
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

    void MarkFrameSubmitted(uint64_t submittedEpoch);
    void CollectCompletedEpoch(uint64_t completedEpoch);
    void ForceReleaseAll();

    uint64_t GetLastSubmittedEpoch() const { return lastSubmittedEpoch; }
    uint64_t GetLastCompletedEpoch() const { return lastCompletedEpoch; }
    size_t GetPendingCount() const { return pendingResources.size(); }

private:
    ResourceRetireQueue() = default;

    uint64_t lastSubmittedEpoch = 0;
    uint64_t lastCompletedEpoch = 0;
    std::vector<RetiredResource> pendingResources;
};

} // namespace VL
