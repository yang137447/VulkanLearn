#pragma once

#include <cstdint>
#include <memory>
#include <mutex>
#include <utility>

#include "ui/uiRenderSnapshot.h"

namespace VL
{

// Transfers the newest immutable UI snapshot from the game thread to the render thread.
// It does not traverse or retain live widget contexts.
class UiRenderSnapshotQueue
{
public:
    void Publish(std::shared_ptr<const UiRenderSnapshot> snapshot);
    std::shared_ptr<const UiRenderSnapshot> ConsumeLatest();
    void Clear();

private:
    std::mutex mutex;
    std::shared_ptr<const UiRenderSnapshot> latestSnapshot;
};

// GT 侧发布闸门：只有调度器判定"这一帧要发布"时才替换快照。
// 跳过帧不调用 Publish，因此 RT 队列保持空、RT 继续持有自己上一次消费到的
// 不可变快照；本类额外保留最后一份已发布 shared_ptr，用于证明身份未变且
// 避免同一帧被清空后没有可回退的引用。
class UiRenderSnapshotPublisher
{
public:
    void Publish(std::shared_ptr<const UiRenderSnapshot> snapshot)
    {
        lastPublished = std::move(snapshot);
        queue.Publish(lastPublished);
        ++publishedCount;
    }

    // 空操作也是一种状态：计数只用于诊断/测试，不改变队列内容。
    void Skip() noexcept { ++skippedCount; }

    const std::shared_ptr<const UiRenderSnapshot>& LastPublished() const noexcept
    {
        return lastPublished;
    }

    UiRenderSnapshotQueue& Queue() noexcept { return queue; }
    uint64_t PublishedCount() const noexcept { return publishedCount; }
    uint64_t SkippedCount() const noexcept { return skippedCount; }

    void Clear() noexcept
    {
        queue.Clear();
        lastPublished.reset();
        publishedCount = 0;
        skippedCount = 0;
    }

private:
    UiRenderSnapshotQueue queue;
    std::shared_ptr<const UiRenderSnapshot> lastPublished;
    uint64_t publishedCount = 0;
    uint64_t skippedCount = 0;
};

} // namespace VL
