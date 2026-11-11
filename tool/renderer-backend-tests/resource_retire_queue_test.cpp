#include <gtest/gtest.h>

#include <algorithm>
#include <atomic>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include "render/resource/resourceRetireQueue.h"

namespace
{

struct TrackedResource
{
    std::atomic<bool>* destroyed = nullptr;

    ~TrackedResource()
    {
        if (destroyed)
        {
            destroyed->store(true, std::memory_order_release);
        }
    }
};

} // namespace

TEST(ResourceRetireQueue, KeepsResourceUntilCompletedEpoch)
{
    std::atomic<bool> destroyed{false};
    auto resource = std::make_shared<TrackedResource>();
    resource->destroyed = &destroyed;
    const std::weak_ptr<TrackedResource> weak = resource;

    VL::ResourceRetireQueue::GetInstance().RetireShared(
        "ResourceRetireQueueTest:KeepsUntilEpoch",
        9001,
        7,
        resource);
    resource.reset();

    VL::ResourceRetireQueue::GetInstance().MarkFrameSubmitted(7);
    VL::ResourceRetireQueue::GetInstance().CollectCompletedEpoch(6);
    EXPECT_FALSE(weak.expired());
    EXPECT_FALSE(destroyed.load(std::memory_order_acquire));

    VL::ResourceRetireQueue::GetInstance().CollectCompletedEpoch(7);
    EXPECT_TRUE(weak.expired());
    EXPECT_TRUE(destroyed.load(std::memory_order_acquire));
}

TEST(ResourceRetireQueue, ConcurrentRetireAndCollectDoNotReleaseEarly)
{
    constexpr int kResourceCount = 128;
    constexpr uint64_t kRetireEpoch = 42;
    std::atomic<bool> start{false};
    std::vector<std::weak_ptr<TrackedResource>> resources;
    resources.reserve(kResourceCount);

    std::thread collector([&]
    {
        while (!start.load(std::memory_order_acquire))
        {
            std::this_thread::yield();
        }
        for (int i = 0; i < 64; ++i)
        {
            VL::ResourceRetireQueue::GetInstance().CollectCompletedEpoch(41);
        }
    });

    std::thread producer([&]
    {
        while (!start.load(std::memory_order_acquire))
        {
            std::this_thread::yield();
        }
        for (int i = 0; i < kResourceCount; ++i)
        {
            auto resource = std::make_shared<TrackedResource>();
            resources.push_back(resource);
            VL::ResourceRetireQueue::GetInstance().RetireShared(
                "ResourceRetireQueueTest:Concurrent" + std::to_string(i),
                9002,
                kRetireEpoch,
                std::move(resource));
        }
    });

    start.store(true, std::memory_order_release);
    producer.join();
    collector.join();

    for (const auto& resource : resources)
    {
        EXPECT_FALSE(resource.expired());
    }

    VL::ResourceRetireQueue::GetInstance().CollectCompletedEpoch(kRetireEpoch);
    for (const auto& resource : resources)
    {
        EXPECT_TRUE(resource.expired());
    }
}

TEST(ResourceRetireQueue, PendingRenderFramesDelayReleaseByOneEpoch)
{
    VL::ResourceRetireQueue& queue =
        VL::ResourceRetireQueue::GetInstance();
    queue.SetPendingRenderFrames(0);

    // Previous cases may have advanced both epochs; start above them so the
    // floor arithmetic is independent of test order.
    const uint64_t baseEpoch =
        std::max(
            queue.GetLastSubmittedEpoch(),
            queue.GetLastCompletedEpoch()) +
        100;
    queue.MarkFrameSubmitted(baseEpoch);

    ASSERT_EQ(queue.GetLastSubmittedEpoch(), baseEpoch);

    queue.SetPendingRenderFrames(1);
    ASSERT_EQ(queue.GetLastSubmittedEpoch(), baseEpoch + 1);

    std::atomic<bool> destroyed{false};
    auto resource = std::make_shared<TrackedResource>();
    resource->destroyed = &destroyed;
    const std::weak_ptr<TrackedResource> weak = resource;

    queue.RetireShared(
        "ResourceRetireQueueTest:PendingFrameFloor",
        9003,
        queue.GetLastSubmittedEpoch(),
        resource);
    resource.reset();

    // The real submitted epoch completed, but the conservative floor is one
    // frame higher, so the resource must survive.
    queue.CollectCompletedEpoch(baseEpoch);
    EXPECT_FALSE(weak.expired());
    EXPECT_FALSE(destroyed.load(std::memory_order_acquire));

    queue.SetPendingRenderFrames(0);
    queue.CollectCompletedEpoch(baseEpoch + 1);
    EXPECT_TRUE(weak.expired());
    EXPECT_TRUE(destroyed.load(std::memory_order_acquire));
}

