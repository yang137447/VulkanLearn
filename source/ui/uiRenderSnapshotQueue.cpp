#include "ui/uiRenderSnapshotQueue.h"

#include <utility>

namespace VL
{

void UiRenderSnapshotQueue::Publish(std::shared_ptr<const UiRenderSnapshot> snapshot)
{
    std::lock_guard<std::mutex> lock(mutex);
    latestSnapshot = std::move(snapshot);
}

std::shared_ptr<const UiRenderSnapshot> UiRenderSnapshotQueue::ConsumeLatest()
{
    std::lock_guard<std::mutex> lock(mutex);
    std::shared_ptr<const UiRenderSnapshot> snapshot = std::move(latestSnapshot);
    latestSnapshot.reset();
    return snapshot;
}

void UiRenderSnapshotQueue::Clear()
{
    std::lock_guard<std::mutex> lock(mutex);
    latestSnapshot.reset();
}

} // namespace VL
