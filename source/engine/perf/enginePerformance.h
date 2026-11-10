#pragma once

#include <chrono>
#include <string>

#include "core/performanceJsonlWriter.h"

class RenderSystem;

namespace VL
{

class UiSubsystem;

// --perf-jsonl / --gpu-stats 的 GT 侧状态。EngineLoop 仍然拥有生命周期，
// 模块只通过显式指针读写，不复制计时器或 writer。
struct EnginePerformanceContext
{
    PerformanceJsonlWriter* perfJsonl = nullptr;
    std::chrono::steady_clock::time_point* nextPerfJsonlWrite = nullptr;
    std::chrono::steady_clock::time_point* nextUiSectionStats = nullptr;
    bool gpuStatsEnabled = false;
    const RenderSystem* renderSystem = nullptr;
    UiSubsystem* uiSubsystem = nullptr;
};

// 与 perf json/JSONL/性能面板共用的同一份冻结快照组装。
std::string CapturePerformanceSnapshotJson(const RenderSystem& renderSystem);

// --perf-jsonl：按墙钟每秒把同一份 Performance 快照追加一行。
void TickPerformanceJsonl(const EnginePerformanceContext& context);

// --gpu-stats：每秒输出分节重建/面板构建计数，证明门禁生效。
void TickUiSectionStats(const EnginePerformanceContext& context);

} // namespace VL
