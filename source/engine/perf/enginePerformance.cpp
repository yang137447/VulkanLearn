#include "engine/perf/enginePerformance.h"

#include <chrono>
#include <iostream>

#include "core/performanceSnapshot.h"
#include "renderSystem.h"
#include "ui/uiSubsystem.h"
#include "ui/viewModel/runtimeUiSections.h"

namespace VL
{

std::string CapturePerformanceSnapshotJson(const RenderSystem& renderSystem)
{
    return FormatPerformanceJson(renderSystem.CapturePerformanceSnapshot());
}

void TickPerformanceJsonl(const EnginePerformanceContext& context)
{
    if (!context.perfJsonl->IsOpen())
    {
        return;
    }
    const auto now = std::chrono::steady_clock::now();
    if (now < *context.nextPerfJsonlWrite)
    {
        return;
    }
    // 固定墙钟节奏：渲染变慢时不补写、不批量刷行，agent 看到的时间序列
    // 与真实运行时长一一对应。第一行在启动满 1 秒后写出。
    *context.nextPerfJsonlWrite = now + std::chrono::seconds(1);
    context.perfJsonl->Append(CapturePerformanceSnapshotJson(*context.renderSystem));
}

void TickUiSectionStats(const EnginePerformanceContext& context)
{
    if (!context.gpuStatsEnabled || context.uiSubsystem == nullptr)
    {
        return;
    }
    const auto now = std::chrono::steady_clock::now();
    if (now < *context.nextUiSectionStats)
    {
        return;
    }
    *context.nextUiSectionStats = now + std::chrono::seconds(1);
    // 诊断计数只在 --gpu-stats 下读取：证明静止帧没有分节重建/面板重建，
    // 交互帧才产生脏集。计数每秒归零，读出的是“每秒次数”。
    const UiSubsystem::RefreshStats stats = context.uiSubsystem->ConsumeRefreshStats();
    std::cout << "[UI] sections/s=" << stats.sectionPublishes
        << " panelBuilds/s=" << stats.panelBuilds
        << " runtimeViews/s=" << stats.runtimeViewUpdates
        << " bindingDirty/s=" << stats.bindingDirtyWrites
        // 第 3 层诊断：rmlUpdates + skipped ≈ 可见帧数；snapshotPublishes 通常
        // 等于 rmlUpdates，只有 DrawDebug 逐帧变化时才会更多。
        << " rmlUpdates/s=" << stats.rmlUpdates
        << " snapshotPublishes/s=" << stats.snapshotPublishes
        << " skipped/s=" << stats.skippedUpdates;
    for (size_t index = 0; index < UiSectionCount; ++index)
    {
        if (stats.sectionPublishesById[index] == 0)
        {
            continue;
        }
        std::cout << ' ' << UiSectionName(static_cast<UiSectionId>(index)) << ':'
            << stats.sectionPublishesById[index];
    }
    std::cout << std::endl;
}

} // namespace VL
