#include "render/renderThread.h"

#include <exception>
#include <stdexcept>

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
    frameRequested = false;
    renderBusy = false;
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
        frameRequested = false;
    }

    condition.notify_all();
    if (worker.joinable())
    {
        worker.join();
    }

    std::lock_guard<std::mutex> lock(mutex);
    running = false;
    renderSystem = nullptr;
    renderBusy = false;
    frameRequested = false;
}

void RenderThread::SubmitFrame()
{
    {
        std::lock_guard<std::mutex> lock(mutex);
        if (!running || !fatalError.empty())
        {
            return;
        }

        frameRequested = true;
    }

    condition.notify_one();
}

void RenderThread::WaitUntilIdle()
{
    std::unique_lock<std::mutex> lock(mutex);
    while (running && (renderBusy || frameRequested))
    {
        condition.wait(lock);
    }
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
            while (!stopRequested && !frameRequested)
            {
                condition.wait(lock);
            }

            if (stopRequested)
            {
                break;
            }

            frameRequested = false;
            renderBusy = true;
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
            renderBusy = false;
            if (!fatalError.empty())
            {
                stopRequested = true;
                frameRequested = false;
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
        renderBusy = false;
        frameRequested = false;
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
