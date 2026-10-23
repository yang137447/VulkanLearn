#pragma once

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <vector>

#include "core/performanceSnapshot.h"

namespace VL
{

// CPU 帧阶段。顺序就是 Performance 面板与 perf JSON 的输出顺序，与 GPU pass 表
// 一样固定，不按耗时重排。other 不在这里：它是发布时用 cpuMs 减掉已测阶段之和
// 得到的派生项，见 performanceSnapshot.h 的 ComputePerformanceCpuOtherMilliseconds。
enum class CpuFrameStage : std::size_t
{
    Input = 0,
    Scripts,
    Animation,
    HairSimulation,
    WorldSnapshot,
    UiViewModel,
    RenderRecord,
    SubmitPresent,
    Count
};

constexpr std::size_t kCpuFrameStageCount = static_cast<std::size_t>(CpuFrameStage::Count);

// 与 GPU 环形窗口一致：120 帧窗口，不足 120 时按已采集帧平均。
constexpr size_t kCpuFrameTimingWindowFrames = 120;

inline const char* CpuFrameStageName(CpuFrameStage stage)
{
    switch (stage)
    {
    case CpuFrameStage::Input: return "input";
    case CpuFrameStage::Scripts: return "scripts";
    case CpuFrameStage::Animation: return "animation";
    case CpuFrameStage::HairSimulation: return "hairSimulation";
    case CpuFrameStage::WorldSnapshot: return "worldSnapshot";
    case CpuFrameStage::UiViewModel: return "uiViewModel";
    case CpuFrameStage::RenderRecord: return "renderRecord";
    case CpuFrameStage::SubmitPresent: return "submitPresent";
    default: return "unknown";
    }
}

// 逐阶段固定窗口环形平均。纯 CPU 逻辑、定长数组，不持有 Vulkan/World 句柄。
// 每个阶段独立记录已采集样本数与写指针，因此某一帧缺少某阶段（没有有效样本）
// 时只让该阶段暂时为 n/a，不影响其它行的部分窗口平均，也不把缺失当成 0。
class CpuFrameTimingRing
{
public:
    explicit CpuFrameTimingRing(size_t windowFrameCount = kCpuFrameTimingWindowFrames)
        : windowFrameCount(windowFrameCount == 0
              ? 1
              : (windowFrameCount > kCpuFrameTimingWindowFrames
                    ? kCpuFrameTimingWindowFrames
                    : windowFrameCount))
    {
    }

    void AddFrame(const std::array<std::optional<double>, kCpuFrameStageCount>& durations)
    {
        for (size_t stageIndex = 0; stageIndex < kCpuFrameStageCount; ++stageIndex)
        {
            if (!durations[stageIndex].has_value())
            {
                continue;
            }

            std::array<double, kCpuFrameTimingWindowFrames>& buffer =
                samples[stageIndex];
            if (sampleCounts[stageIndex] < windowFrameCount)
            {
                buffer[sampleCounts[stageIndex]] = *durations[stageIndex];
                ++sampleCounts[stageIndex];
            }
            else
            {
                buffer[nextWrite[stageIndex]] = *durations[stageIndex];
                nextWrite[stageIndex] = (nextWrite[stageIndex] + 1) % windowFrameCount;
            }
        }
        ++framesPushed;
    }

    void Clear()
    {
        for (size_t stageIndex = 0; stageIndex < kCpuFrameStageCount; ++stageIndex)
        {
            sampleCounts[stageIndex] = 0;
            nextWrite[stageIndex] = 0;
        }
        framesPushed = 0;
    }

    // 已进入窗口的帧数（不超过容量），是表头的 "avg over N"。
    size_t WindowFrameCount() const
    {
        return framesPushed > windowFrameCount ? windowFrameCount : framesPushed;
    }

    // 窗口内该阶段的有效样本数；0 表示从未采样，输出 n/a。
    uint32_t SampleCount(CpuFrameStage stage) const
    {
        const size_t index = static_cast<size_t>(stage);
        return index < kCpuFrameStageCount ? sampleCounts[index] : 0;
    }

    std::optional<double> Average(CpuFrameStage stage) const
    {
        const size_t index = static_cast<size_t>(stage);
        if (index >= kCpuFrameStageCount || sampleCounts[index] == 0)
        {
            return std::nullopt;
        }

        double sum = 0.0;
        for (size_t sampleIndex = 0; sampleIndex < sampleCounts[index]; ++sampleIndex)
        {
            sum += samples[index][sampleIndex];
        }
        return sum / static_cast<double>(sampleCounts[index]);
    }

private:
    size_t windowFrameCount;
    std::array<std::array<double, kCpuFrameTimingWindowFrames>, kCpuFrameStageCount>
        samples{};
    std::array<size_t, kCpuFrameStageCount> sampleCounts{};
    std::array<size_t, kCpuFrameStageCount> nextWrite{};
    size_t framesPushed = 0;
};

// GT 单写者的 CPU 帧计时器。BeginFrame 清零本帧累加器，ScopedStage 把区间
// 累加进对应阶段（同一阶段允许一帧多段），CommitFrame 把本帧推入 120 帧环形窗口
// 并返回窗口平均后的 Performance 样本。
//
// 所有权：对象由 EngineLoop 独占，只在 GT 的 Tick 生命周期内读写；CommitFrame 之后
// EngineLoop 把冻结样本交给 GpuPassTimer 持有的唯一快照（mutex 保护），面板、
// perf json 与 --perf-jsonl 都只读那份副本，因此不存在两套计时或第二个窗口来源。
class CpuFrameProfiler
{
public:
    CpuFrameProfiler()
    {
        committed.reserve(kCpuFrameStageCount);
        for (size_t index = 0; index < kCpuFrameStageCount; ++index)
        {
            PerformancePassSample sample;
            sample.name = CpuFrameStageName(static_cast<CpuFrameStage>(index));
            committed.push_back(std::move(sample));
        }
    }

    // 丢弃上一帧未提交的累加值，开始新一帧。
    void BeginFrame() noexcept
    {
        currentMilliseconds.fill(0.0);
        currentSampled.fill(false);
    }

    void AddStageMilliseconds(CpuFrameStage stage, double milliseconds) noexcept
    {
        const size_t index = static_cast<size_t>(stage);
        if (index >= kCpuFrameStageCount || !(milliseconds >= 0.0))
        {
            return;
        }
        currentMilliseconds[index] += milliseconds;
        currentSampled[index] = true;
    }

    // 推入当前帧、重算窗口平均，然后把本帧累加器清零。
    const std::vector<PerformancePassSample>& CommitFrame()
    {
        for (size_t index = 0; index < kCpuFrameStageCount; ++index)
        {
            pendingDurations[index] = currentSampled[index]
                ? std::optional<double>(currentMilliseconds[index])
                : std::nullopt;
        }
        ring.AddFrame(pendingDurations);

        for (size_t index = 0; index < kCpuFrameStageCount; ++index)
        {
            committed[index].milliseconds =
                ring.Average(static_cast<CpuFrameStage>(index));
            committed[index].sampleCount =
                ring.SampleCount(static_cast<CpuFrameStage>(index));
        }
        BeginFrame();
        return committed;
    }

    void Reset() noexcept
    {
        ring.Clear();
        BeginFrame();
        for (PerformancePassSample& sample : committed)
        {
            sample.milliseconds.reset();
            sample.sampleCount = 0;
        }
    }

    size_t WindowFrameCount() const
    {
        return ring.WindowFrameCount();
    }

private:
    CpuFrameTimingRing ring;
    std::array<double, kCpuFrameStageCount> currentMilliseconds{};
    std::array<bool, kCpuFrameStageCount> currentSampled{};
    std::array<std::optional<double>, kCpuFrameStageCount> pendingDurations{};
    std::vector<PerformancePassSample> committed;
};

// RAII 区间：构造时 steady_clock::now()，析构时再取一次并累加到同一阶段。
// 只做两次时钟读取和一次加法，不分配、不打日志。允许显式 End() 提前收尾，
// 便于把 Tick 中不连续的同一阶段分成多个区间。
class CpuFrameStageScope
{
public:
    CpuFrameStageScope(CpuFrameProfiler& frameProfiler, CpuFrameStage frameStage) noexcept
        : profiler(&frameProfiler)
        , stage(frameStage)
        , start(std::chrono::steady_clock::now())
    {
    }

    ~CpuFrameStageScope()
    {
        End();
    }

    CpuFrameStageScope(const CpuFrameStageScope&) = delete;
    CpuFrameStageScope& operator=(const CpuFrameStageScope&) = delete;

    void End() noexcept
    {
        if (ended)
        {
            return;
        }
        ended = true;
        const std::chrono::steady_clock::time_point finish =
            std::chrono::steady_clock::now();
        profiler->AddStageMilliseconds(
            stage,
            std::chrono::duration<double, std::milli>(finish - start).count());
    }

private:
    CpuFrameProfiler* profiler = nullptr;
    CpuFrameStage stage = CpuFrameStage::Input;
    std::chrono::steady_clock::time_point start;
    bool ended = false;
};

} // namespace VL
