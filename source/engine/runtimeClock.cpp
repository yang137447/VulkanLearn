#include "engine/runtimeClock.h"

namespace VL
{

void RuntimeClock::Reset()
{
    previousTime = Clock::now();
    hasPreviousTime = true;
}

float RuntimeClock::TickDeltaSeconds()
{
    const Clock::time_point currentTime = Clock::now();
    if (!hasPreviousTime)
    {
        previousTime = currentTime;
        hasPreviousTime = true;
        return 0.0f;
    }

    const float deltaSeconds = std::chrono::duration<float>(currentTime - previousTime).count();
    previousTime = currentTime;
    return deltaSeconds;
}

} // namespace VL
