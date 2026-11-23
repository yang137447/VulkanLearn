#pragma once

#include <cmath>
#include <cstdint>
#include <mutex>
#include <stdexcept>

namespace VL
{
class TerrainCompositedQualityClock
{
public:
    void Start(bool initialWorld, double processStartedMs,
        double initialLimitMs, double poseLimitMs)
    {
        if (!std::isfinite(processStartedMs) || !std::isfinite(initialLimitMs) ||
            !std::isfinite(poseLimitMs) || !std::isfinite(processStartedMs + initialLimitMs) ||
            initialLimitMs <= 0 || poseLimitMs <= 0)
            throw std::invalid_argument("Quality clock requires finite frozen deadlines");
        std::lock_guard<std::mutex> lock(mutex);
        if (started)
            throw std::logic_error("Quality clock already started");
        started = true;
        initialPending = initialWorld;
        initialDeadline = processStartedMs + initialLimitMs;
        poseLimit = poseLimitMs;
        deadline = initialWorld ? initialDeadline : 0;
    }

    void SetPose(uint64_t identity, double arrivedMs)
    {
        if (!identity || !std::isfinite(arrivedMs))
            throw std::invalid_argument("Quality pose needs its actual finite arrival");
        std::lock_guard<std::mutex> lock(mutex);
        if (!started)
            throw std::logic_error("Quality clock not started");
        if (!std::isfinite(arrivedMs + poseLimit))
            throw std::invalid_argument("Quality pose deadline overflow");
        if (pose == identity)
            return;
        Expire(arrivedMs);
        pose = identity;
        ready = false;
        deadline = initialPending ? initialDeadline : arrivedMs + poseLimit;
    }

    bool Observe(double completedMs, bool meetsQuality)
    {
        std::lock_guard<std::mutex> lock(mutex);
        if (!started || !pose || !std::isfinite(completedMs))
            return false;
        if (!ready || !meetsQuality)
        {
            ready = false;
            Expire(completedMs);
        }
        ready = !failed && meetsQuality;
        if (ready)
            initialPending = false;
        return ready;
    }

    bool Failed(double nowMs) noexcept
    {
        std::lock_guard<std::mutex> lock(mutex);
        Expire(nowMs);
        return failed;
    }

    double DeadlineMs() const
    {
        std::lock_guard<std::mutex> lock(mutex);
        return deadline;
    }

private:
    void Expire(double nowMs) noexcept
    {
        if (started && !ready && deadline != 0 &&
            (!std::isfinite(nowMs) || nowMs > deadline))
            failed = true;
    }

    mutable std::mutex mutex;
    bool started = false;
    bool initialPending = false;
    bool ready = false;
    bool failed = false;
    uint64_t pose = 0;
    double deadline = 0;
    double initialDeadline = 0;
    double poseLimit = 0;
};
}
