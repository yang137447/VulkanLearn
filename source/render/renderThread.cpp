#include "render/renderThread.h"

#include <exception>

#include "profiler.h"
#include "renderSystem.h"

namespace VL
{

RenderThread::~RenderThread()
{
    Stop();
}

void RenderThread::Start(RenderSystem& renderSystem)
{
    std::lock_guard<std::mutex> lock(mutex);
    if (running)
    {
        return;
    }

    this->renderSystem = &renderSystem;
    stopRequested = false;
    frameBudget.Reset();
    fatalError.clear();
    worker = std::thread(&RenderThread::ThreadMain, this);
    running = true;
}

void RenderThread::Stop()
{
    {
        std::lock_guard<std::mutex> lock(mutex);
        if (!running)
        {
            return;
        }

        stopRequested = true;
        // Clear every outstanding request so WaitForFrameSlot/WaitUntilIdle
        // cannot wait forever on a frame that will never be recorded.
        frameBudget.Abort();
    }

    condition.notify_all();
    if (worker.joinable())
    {
        worker.join();
    }

    std::lock_guard<std::mutex> lock(mutex);
    running = false;
    renderSystem = nullptr;
    frameBudget.Reset();
}

void RenderThread::WaitForFrameSlot()
{
    std::unique_lock<std::mutex> lock(mutex);
    while (running && fatalError.empty() && !frameBudget.CanPublish())
    {
        condition.wait(lock);
    }
}

void RenderThread::SubmitFrame()
{
    {
        std::lock_guard<std::mutex> lock(mutex);
        if (!running || stopRequested || !fatalError.empty())
        {
            return;
        }

        frameBudget.Publish();
    }

    condition.notify_all();
}

void RenderThread::WaitUntilIdle()
{
    std::unique_lock<std::mutex> lock(mutex);
    while (running && !frameBudget.IsIdle())
    {
        // A fatal render failure aborts the budget; a concurrent Stop() also
        // clears running, so neither case can spin here.
        condition.wait(lock);
    }
}

size_t RenderThread::GetPendingFrameCount() const
{
    std::lock_guard<std::mutex> lock(mutex);
    return frameBudget.GetPendingFrameCount();
}

bool RenderThread::IsFramePending() const
{
    std::lock_guard<std::mutex> lock(mutex);
    return !frameBudget.IsIdle();
}

uint64_t RenderThread::GetRequestedFrameSerial() const
{
    std::lock_guard<std::mutex> lock(mutex);
    return frameBudget.GetRequestedFrameSerial();
}

uint64_t RenderThread::GetCompletedFrameSerial() const
{
    std::lock_guard<std::mutex> lock(mutex);
    return frameBudget.GetCompletedFrameSerial();
}

bool RenderThread::IsRunning() const
{
    std::lock_guard<std::mutex> lock(mutex);
    return running;
}

bool RenderThread::HasFatalError() const
{
    std::lock_guard<std::mutex> lock(mutex);
    return !fatalError.empty();
}

std::string RenderThread::ConsumeFatalError()
{
    std::lock_guard<std::mutex> lock(mutex);
    std::string message = fatalError;
    fatalError.clear();
    return message;
}

void RenderThread::ThreadMain()
{
    while (true)
    {
        {
            std::unique_lock<std::mutex> lock(mutex);
            while (!stopRequested && !frameBudget.HasPendingRequest())
            {
                condition.wait(lock);
            }

            if (stopRequested)
            {
                break;
            }

            if (!frameBudget.BeginRender().has_value())
            {
                continue;
            }
        }

        try
        {
            PROFILE_SCOPE("RenderThread::Frame");
            renderSystem->RenderLatestSnapshotOrLastGood();
        }
        catch (const std::exception& exception)
        {
            StoreFatalError(exception.what());
        }
        catch (...)
        {
            StoreFatalError("Unknown render thread failure");
        }

        {
            std::lock_guard<std::mutex> lock(mutex);
            frameBudget.EndRender();
            if (!fatalError.empty())
            {
                // Release every waiter even if a frame request arrived while
                // the failing frame was still in flight.
                stopRequested = true;
                frameBudget.Abort();
            }
        }

        condition.notify_all();

        std::lock_guard<std::mutex> lock(mutex);
        if (stopRequested)
        {
            break;
        }
    }

    {
        std::lock_guard<std::mutex> lock(mutex);
        frameBudget.Abort();
    }
    condition.notify_all();
}

void RenderThread::StoreFatalError(const std::string& message)
{
    std::lock_guard<std::mutex> lock(mutex);
    if (fatalError.empty())
    {
        fatalError = message;
    }
}

} // namespace VL
