#pragma once

#include "render/environment/environmentGpuTimer.h"
#include "render/environment/environmentUpdateScheduler.h"

namespace VL
{

// 环境更新期间收集的诊断信息。
// 包含当前更新进度和环境更新工作的 GPU 计时快照。
struct EnvironmentUpdateDiagnostics
{
    EnvironmentUpdateProgress progress;
    EnvironmentGpuTimingSnapshot gpuTiming;
};

} // namespace VL
