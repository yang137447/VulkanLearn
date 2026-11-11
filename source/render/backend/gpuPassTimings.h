#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <iomanip>
#include <optional>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

#include "core/performanceSnapshot.h"

namespace VL
{

// 每个统计窗口保留的帧数。--gpu-stats 每 120 帧打印一次，窗口与打印周期一致，
// 因此表里的 avg 就是这 120 帧（不足 120 时是已采集帧）的平均值。
constexpr size_t kGpuPassTimingWindowFrames = 120;

// 每帧最多记录多少对 begin/end timestamp（含合成的 hairSimulation 一项）。
// RenderGraph 当前约 19 个 pass，64 留出充足余量；超出时只记录前 64 项并报一次，
// 通过上限而不是重建 query pool 来保证结果读取绝不会与仍在飞行的查询冲突。
constexpr uint32_t kGpuPassTimingMaxEntries = 64;

struct GpuPassTimingRow
{
    std::string name;
    std::optional<double> averageMilliseconds;
    uint32_t sampleCount = 0;
};

struct GpuPassTimingTable
{
    std::vector<GpuPassTimingRow> rows;
    size_t frameCount = 0;
};

// 两个 timestamp tick 换算为毫秒。end < begin 表示计数器回绕或查询被禁用，
// 返回 nullopt 让调用方输出 n/a；绝不产出负值。period 非正同样是无效输入。
inline std::optional<double> ComputeGpuPassDurationMilliseconds(
    uint64_t beginTicks,
    uint64_t endTicks,
    double timestampPeriodNanoseconds)
{
    if (!(timestampPeriodNanoseconds > 0.0) || endTicks < beginTicks)
    {
        return std::nullopt;
    }

    const uint64_t elapsedTicks = endTicks - beginTicks;
    return static_cast<double>(elapsedTicks) * timestampPeriodNanoseconds / 1000000.0;
}

// 逐 pass 的固定窗口环形平均。纯 CPU 逻辑，不持有 Vulkan 对象，便于单元测试。
// 名字顺序即输出顺序；AddFrame 里的 duration 按同名索引对齐。
class GpuPassTimingRing
{
public:
    explicit GpuPassTimingRing(size_t windowFrameCount = kGpuPassTimingWindowFrames)
        : windowFrameCount(windowFrameCount == 0 ? 1 : windowFrameCount)
    {
    }

    // RenderGraph 重载导致 pass 集合变化时清空窗口；名字相同则保留现有样本。
    void Configure(const std::vector<std::string>& passNames)
    {
        if (passNames == names)
        {
            return;
        }

        names = passNames;
        samples.assign(names.size(), std::vector<double>());
        for (auto& buffer : samples)
        {
            buffer.reserve(windowFrameCount);
        }
        nextWrite.assign(names.size(), 0);
        framesPushed = 0;
    }

    void AddFrame(const std::vector<std::optional<double>>& durations)
    {
        const size_t count = std::min(names.size(), durations.size());
        for (size_t i = 0; i < count; ++i)
        {
            if (!durations[i].has_value())
            {
                continue;
            }

            std::vector<double>& buffer = samples[i];
            if (buffer.size() < windowFrameCount)
            {
                buffer.push_back(*durations[i]);
            }
            else
            {
                buffer[nextWrite[i]] = *durations[i];
                nextWrite[i] = (nextWrite[i] + 1) % windowFrameCount;
            }
        }
        ++framesPushed;
    }

    void Clear()
    {
        for (auto& buffer : samples)
        {
            buffer.clear();
        }
        std::fill(nextWrite.begin(), nextWrite.end(), 0u);
        framesPushed = 0;
    }

    // 已进入窗口的帧数（不超过容量）。用于表头的 "avg over N"。
    size_t WindowFrameCount() const
    {
        return std::min(framesPushed, windowFrameCount);
    }

    const std::vector<std::string>& Names() const
    {
        return names;
    }

    // 并行数组版本供计时器复用缓冲，避免每帧构造带字符串的行。
    void ComputeAverages(
        std::vector<std::optional<double>>& outAverages,
        std::vector<uint32_t>& outSampleCounts) const
    {
        outAverages.assign(names.size(), std::nullopt);
        outSampleCounts.assign(names.size(), 0u);
        for (size_t i = 0; i < names.size(); ++i)
        {
            const std::vector<double>& buffer = samples[i];
            if (buffer.empty())
            {
                continue;
            }

            double sum = 0.0;
            for (double value : buffer)
            {
                sum += value;
            }
            outAverages[i] = sum / static_cast<double>(buffer.size());
            outSampleCounts[i] = static_cast<uint32_t>(buffer.size());
        }
    }

    GpuPassTimingTable Snapshot() const
    {
        GpuPassTimingTable table;
        table.frameCount = WindowFrameCount();

        std::vector<std::optional<double>> averages;
        std::vector<uint32_t> sampleCounts;
        ComputeAverages(averages, sampleCounts);
        table.rows.reserve(names.size());
        for (size_t i = 0; i < names.size(); ++i)
        {
            GpuPassTimingRow row;
            row.name = names[i];
            row.averageMilliseconds = averages[i];
            row.sampleCount = sampleCounts[i];
            table.rows.push_back(std::move(row));
        }
        return table;
    }

private:
    size_t windowFrameCount;
    std::vector<std::string> names;
    std::vector<std::vector<double>> samples;
    std::vector<size_t> nextWrite;
    size_t framesPushed = 0;
};

// 把 Performance 快照还原成日志/控制台使用的表格。
// 这是唯一转换点：UI/JSON 不各自维护第二份 pass 顺序。
inline GpuPassTimingTable ToGpuPassTimingTable(const PerformanceSnapshot& snapshot)
{
    GpuPassTimingTable table;
    table.frameCount = snapshot.windowFrameCount;
    table.rows.reserve(snapshot.passes.size());
    for (const PerformancePassSample& pass : snapshot.passes)
    {
        GpuPassTimingRow row;
        row.name = pass.name;
        row.averageMilliseconds = pass.milliseconds;
        row.sampleCount = pass.sampleCount;
        table.rows.push_back(std::move(row));
    }
    return table;
}

// 定长表格文本。total 是各 pass 平均耗时之和，不是帧时间：
// pass 之间的 barrier、未计时的环境 IBL/UI 以及 CPU 提交间隙都不在其中。
inline std::string FormatGpuPassTimingTable(const GpuPassTimingTable& table)
{
    std::ostringstream output;
    output << "[GPU] frame avg over " << table.frameCount << ": ";

    double totalMilliseconds = 0.0;
    bool hasValidSample = false;
    for (size_t i = 0; i < table.rows.size(); ++i)
    {
        if (i != 0)
        {
            output << " | ";
        }

        const GpuPassTimingRow& row = table.rows[i];
        output << row.name << ' ';
        if (row.averageMilliseconds.has_value())
        {
            output << std::fixed << std::setprecision(2)
                   << *row.averageMilliseconds << " ms";
            totalMilliseconds += *row.averageMilliseconds;
            hasValidSample = true;
        }
        else
        {
            output << "n/a";
        }
    }

    if (!table.rows.empty())
    {
        output << " | ";
    }
    output << "total ";
    if (hasValidSample)
    {
        output << std::fixed << std::setprecision(2) << totalMilliseconds << " ms";
    }
    else
    {
        output << "n/a";
    }
    return output.str();
}

} // namespace VL
