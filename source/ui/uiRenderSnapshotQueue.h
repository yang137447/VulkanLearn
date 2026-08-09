#pragma once

#include <memory>
#include <mutex>

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

} // namespace VL
