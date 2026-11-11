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
    if (!latestSnapshot)
    {
        latestSnapshot = std::move(snapshot);
        return;
    }
    if (snapshot == latestSnapshot)
    {
        return;
    }

    // latest-wins 丢弃的是整帧快照；头发 delta 是累计时间，不能随之丢失。
    // 其它字段不是累计量，后续新增累计 delta 时必须在这里显式合并。
    WorldSnapshot merged = *snapshot;
    merged.hairSimulationDeltaSeconds +=
        latestSnapshot->hairSimulationDeltaSeconds;
    latestSnapshot = std::make_shared<const WorldSnapshot>(std::move(merged));
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

void WorldSnapshotQueue::Clear() noexcept
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
