#pragma once

#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <iostream>

namespace VL
{
namespace RecordCpuDetail
{
// 仅诊断：录制路径 CPU 分段计时的 120 帧窗口累计值。
struct State
{
    double passLoopSeconds = 0.0;
    double skinDeformSeconds = 0.0;
    uint32_t windowFrames = 0;
};
} // namespace RecordCpuDetail

// 仅在 VL_ROD_RECORD_STAGE_TIMING 设置时启用；未设置时所有入口立即返回，
// 不读取时钟，渲染行为与引入该诊断前完全一致。
inline bool RecordCpuStageTimingEnabled()
{
    static const bool enabled =
        std::getenv("VL_ROD_RECORD_STAGE_TIMING") != nullptr;
    return enabled;
}

// 状态放在 inline 函数的静态局部变量里：renderFrameRecorder.cpp（pass 循环）
// 与 renderSystem.cpp（RecordSkinDeform）共享同一份窗口，才能打在同一行。
inline RecordCpuDetail::State& RecordCpuStageState()
{
    static RecordCpuDetail::State state;
    return state;
}

inline double RecordCpuStageSeconds()
{
    return std::chrono::duration<double>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
}

// RAII 闭包：RecordSkinDeform 有多处提前 return，用析构保证每帧只累计一次，
// 且自动覆盖皮肤 palette 上传与 compute 录制两段。
class RecordCpuStageScope
{
public:
    explicit RecordCpuStageScope(double& target)
        : sink(target)
    {
        timed = RecordCpuStageTimingEnabled();
        if (timed)
        {
            startSeconds = RecordCpuStageSeconds();
        }
    }

    ~RecordCpuStageScope()
    {
        if (timed)
        {
            sink += RecordCpuStageSeconds() - startSeconds;
        }
    }

    RecordCpuStageScope(const RecordCpuStageScope&) = delete;
    RecordCpuStageScope& operator=(const RecordCpuStageScope&) = delete;

private:
    double& sink;
    bool timed = false;
    double startSeconds = 0.0;
};

// 每个录制帧在 pass 循环结束后调用一次；满 120 帧输出窗口均值后清零窗口。
// 输出形态对齐 hairSimulationPass.cpp 的 [HairCPU] 行。
inline void RecordCpuStageFrameEnd()
{
    if (!RecordCpuStageTimingEnabled())
    {
        return;
    }

    RecordCpuDetail::State& state = RecordCpuStageState();
    ++state.windowFrames;
    if (state.windowFrames < 120u)
    {
        return;
    }

    const double frames = static_cast<double>(state.windowFrames);
    const double passLoopMs = state.passLoopSeconds / frames * 1000.0;
    const double skinDeformMs = state.skinDeformSeconds / frames * 1000.0;
    std::cout << "[RecordCPU] frames=" << state.windowFrames
              << " passLoop=" << passLoopMs << " ms/frame"
              << " skinDeform=" << skinDeformMs << " ms/frame"
              << " sum=" << (passLoopMs + skinDeformMs) << " ms/frame"
              << " windowPassLoop=" << (state.passLoopSeconds * 1000.0)
              << " ms windowSkinDeform=" << (state.skinDeformSeconds * 1000.0)
              << " ms windowSum="
              << ((state.passLoopSeconds + state.skinDeformSeconds) * 1000.0)
              << " ms" << std::endl;

    state.passLoopSeconds = 0.0;
    state.skinDeformSeconds = 0.0;
    state.windowFrames = 0;
}
} // namespace VL
