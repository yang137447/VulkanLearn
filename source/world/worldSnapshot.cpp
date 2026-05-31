#include "world/worldSnapshot.h"

#include <stdexcept>

namespace VL
{

void WorldSnapshotQueue::Publish(WorldSnapshot snapshot)
{
    Publish(std::make_shared<const WorldSnapshot>(std::move(snapshot)));
}

void WorldSnapshotQueue::Publish(WorldSnapshotPtr snapshot)
{
    if (!snapshot)
    {
        throw std::invalid_argument("WorldSnapshotQueue cannot publish a null snapshot");
    }

    std::lock_guard<std::mutex> lock(mutex);
    latestSnapshot = std::move(snapshot);
}

std::optional<WorldSnapshotPtr> WorldSnapshotQueue::ConsumeLatest()
{
    std::lock_guard<std::mutex> lock(mutex);
    if (!latestSnapshot)
    {
        return std::nullopt;
    }

    WorldSnapshotPtr snapshot = std::move(latestSnapshot);
    latestSnapshot.reset();
    return snapshot;
}

std::optional<WorldSnapshotPtr> WorldSnapshotQueue::PeekLatest() const
{
    std::lock_guard<std::mutex> lock(mutex);
    if (!latestSnapshot)
    {
        return std::nullopt;
    }

    return latestSnapshot;
}

void WorldSnapshotQueue::Clear()
{
    std::lock_guard<std::mutex> lock(mutex);
    latestSnapshot.reset();
}

bool WorldSnapshotQueue::HasPendingSnapshot() const
{
    std::lock_guard<std::mutex> lock(mutex);
    return latestSnapshot != nullptr;
}

} // namespace VL
