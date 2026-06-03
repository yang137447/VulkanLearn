#pragma once

#include <condition_variable>
#include <mutex>
#include <string>
#include <thread>

class RenderSystem;

namespace VL
{

// Optional render-thread runner for workerThreadCount=2. GT publishes immutable
// WorldSnapshot instances into RenderSystem's mailbox; this worker consumes the
// latest snapshot and records/submits the Vulkan frame. V1 intentionally has no
// thread pool or TaskGraph, and EngineLoop still waits at frame/resource-change
// boundaries so shutdown, resize, reload, and smoke-test timing stay deterministic.
class RenderThread
{
public:
    RenderThread() = default;
    ~RenderThread();

    RenderThread(const RenderThread&) = delete;
    RenderThread& operator=(const RenderThread&) = delete;

    void Start(RenderSystem& renderSystem);
    void Stop();
    void SubmitFrame();
    void WaitUntilIdle();

    bool IsRunning() const;
    bool HasFatalError() const;
    std::string ConsumeFatalError();

private:
    void ThreadMain();
    void StoreFatalError(const std::string& message);

    mutable std::mutex mutex;
    std::condition_variable condition;
    std::thread worker;
    RenderSystem* renderSystem = nullptr;
    bool running = false;
    bool stopRequested = false;
    bool frameRequested = false;
    bool renderBusy = false;
    std::string fatalError;
};

} // namespace VL
