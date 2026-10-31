#pragma once

// File responsibility: CPU-side conversion and accumulation for particle GPU
// pass timing. Deliberately free of Vulkan types so mask/period/average rules
// stay unit-testable without a device.

#include <algorithm>
#include <cmath>
#include <cstdint>

namespace VL
{

struct ParticleGpuTimingSnapshot
{
    bool supported = false;
    double lastMilliseconds = 0.0;
    double averageMilliseconds = 0.0;
    double maxMilliseconds = 0.0;
    uint64_t sampleCount = 0;
};

// 时间戳有效位不足 64 时，差值必须在掩码内回绕后再解释。
inline uint64_t ParticleGpuTimestampMask(uint32_t validBits)
{
    return validBits >= 64 ? ~uint64_t{ 0 } : (uint64_t{ 1 } << validBits) - 1;
}

inline double ParticleGpuTimestampDeltaMilliseconds(
    uint64_t beginTicks,
    uint64_t endTicks,
    uint32_t validBits,
    float timestampPeriodNanoseconds)
{
    if (validBits == 0 || !(timestampPeriodNanoseconds > 0.0f))
    {
        return 0.0;
    }
    const uint64_t ticks =
        (endTicks - beginTicks) & ParticleGpuTimestampMask(validBits);
    return static_cast<double>(ticks) *
        static_cast<double>(timestampPeriodNanoseconds) /
        1000000.0;
}

// 不支持计时时整份快照归零；Disable 与 Reset(false) 语义相同。
class ParticleGpuTimingAccumulator
{
public:
    void Reset(bool supported)
    {
        snapshot = ParticleGpuTimingSnapshot{};
        snapshot.supported = supported;
    }

    void Disable()
    {
        snapshot = ParticleGpuTimingSnapshot{};
    }

    void Record(double milliseconds)
    {
        if (!snapshot.supported ||
            !std::isfinite(milliseconds) ||
            milliseconds < 0.0)
        {
            return;
        }

        snapshot.sampleCount++;
        snapshot.lastMilliseconds = milliseconds;
        snapshot.maxMilliseconds =
            std::max(snapshot.maxMilliseconds, milliseconds);
        snapshot.averageMilliseconds +=
            (milliseconds - snapshot.averageMilliseconds) /
            static_cast<double>(snapshot.sampleCount);
    }

    const ParticleGpuTimingSnapshot& GetSnapshot() const noexcept
    {
        return snapshot;
    }

private:
    ParticleGpuTimingSnapshot snapshot;
};

} // namespace VL
