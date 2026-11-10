#pragma once

#include <cstddef>
#include <cstdint>
#include <condition_variable>
#include <mutex>
#include <optional>
#include <string>
#include <thread>

class RenderSystem;

namespace VL
{

// Pure frame-budget bookkeeping shared by RenderThread and its tests. It is not
// thread-safe by itself; RenderThread guards every access with its mutex.
//
// Semantics:
//  - GT may publish at most kMaxFramesAhead frames that have not finished
//    rendering. A publish while the budget is full coalesces into the pending
//    request (latest-wins) and never blocks.
//  - BeginRender() snapshots the newest requested serial, so every request
//    published while a frame is in flight is collapsed into one next frame.
//  - EndRender() releases the frame slot only after the frame fully finished,
//    because that is the point where RT stops referencing GT-published state.
class RenderFrameBudget
{
public:
    static constexpr size_t kMaxFramesAhead = 1;

    void Reset()
    {
        pendingFrames = 0;
        frameRequested = false;
        renderBusy = false;
        requestedSerial = 0;
        renderingSerial = 0;
        completedSerial = 0;
    }

    bool CanPublish() const noexcept
    {
        return pendingFrames < kMaxFramesAhead;
    }

    // Increments the requested serial and marks one frame outstanding. If the
    // budget is already full the request is collapsed into the outstanding one.
    void Publish() noexcept
    {
        ++requestedSerial;
        frameRequested = true;
        if (pendingFrames < kMaxFramesAhead)
        {
            ++pendingFrames;
        }
    }

    // Starts the newest requested frame. Returns its serial, or nullopt when no
    // request is waiting.
    std::optional<uint64_t> BeginRender() noexcept
    {
        if (!frameRequested)
        {
            return std::nullopt;
        }
        frameRequested = false;
        renderBusy = true;
        // Keep the slot occupied while the frame is in flight.
        pendingFrames = kMaxFramesAhead;
        renderingSerial = requestedSerial;
        return renderingSerial;
    }

    // Collapses all requests that arrived during the frame: they will be served
    // by the next BeginRender() against the newest snapshot.
    void EndRender() noexcept
    {
        renderBusy = false;
        pendingFrames = 0;
        completedSerial = renderingSerial;
    }

    // Clears every outstanding request. Used on shutdown and render failure so
    // frame-slot waiters cannot deadlock.
    void Abort() noexcept
    {
        pendingFrames = 0;
        frameRequested = false;
        renderBusy = false;
    }

    bool HasPendingRequest() const noexcept { return frameRequested; }
    bool IsRenderBusy() const noexcept { return renderBusy; }
    bool IsIdle() const noexcept
    {
        return !frameRequested && !renderBusy && pendingFrames == 0;
    }
    size_t GetPendingFrameCount() const noexcept { return pendingFrames; }
    uint64_t GetRequestedFrameSerial() const noexcept { return requestedSerial; }
    uint64_t GetRenderingFrameSerial() const noexcept { return renderingSerial; }
    uint64_t GetCompletedFrameSerial() const noexcept { return completedSerial; }

private:
    size_t pendingFrames = 0;
    bool frameRequested = false;
    bool renderBusy = false;
    uint64_t requestedSerial = 0;
    uint64_t renderingSerial = 0;
    uint64_t completedSerial = 0;
};

// Optional render-thread runner for workerThreadCount=2. GT publishes immutable
// WorldSnapshot instances into RenderSystem's mailbox; this worker consumes the
// latest snapshot and records/submits the Vulkan frame. The pipeline is bounded
// async: GT may publish one frame while RT records another, then waits for that
// slot before publishing again. Structural renderer mutations still use the
// explicit safe points in EngineLoop/RenderSystem.
class RenderThread
{
public:
    RenderThread() = default;
    ~RenderThread();

    RenderThread(const RenderThread&) = delete;
    RenderThread& operator=(const RenderThread&) = delete;

    void Start(RenderSystem& renderSystem);
    void Stop();
    // Blocks while the previous published frame has not finished rendering.
    // Must be called before publishing a new snapshot. Returns immediately when
    // the thread is stopped or failed.
    void WaitForFrameSlot();
    // Publishes one frame request. Never waits and never blocks the caller.
    void SubmitFrame();
    // Waits until no frame is queued, in flight, or requested.
    void WaitUntilIdle();

    bool IsRunning() const;
    bool HasFatalError() const;
    std::string ConsumeFatalError();

    size_t GetPendingFrameCount() const;
    bool IsFramePending() const;
    uint64_t GetRequestedFrameSerial() const;
    uint64_t GetCompletedFrameSerial() const;

private:
    void ThreadMain();
    void StoreFatalError(const std::string& message);

    mutable std::mutex mutex;
    std::condition_variable condition;
    std::thread worker;
    RenderSystem* renderSystem = nullptr;
    bool running = false;
    bool stopRequested = false;
    RenderFrameBudget frameBudget;
    std::string fatalError;
};

} // namespace VL
