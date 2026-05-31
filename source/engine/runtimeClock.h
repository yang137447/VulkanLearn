#pragma once

#include <chrono>

namespace VL
{

// RuntimeClock owns frame delta calculation for EngineLoop. Keeping time here
// avoids pulling the broad CommonFunction utility header into engine runtime
// orchestration and makes future fixed-step or test clocks easier to add.
class RuntimeClock
{
public:
    void Reset();
    float TickDeltaSeconds();

private:
    using Clock = std::chrono::high_resolution_clock;

    bool hasPreviousTime = false;
    Clock::time_point previousTime;
};

} // namespace VL
