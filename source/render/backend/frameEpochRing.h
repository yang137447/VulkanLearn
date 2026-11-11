#pragma once

#include <cstdint>
#include <vector>

namespace VL
{

// frameFenceEpochs 按 frameIndex 分槽保存“该槽最后一次提交的 epoch”。
// 只有对应 taskFinishedFence 完成后，BeginFrame 才能把旧 epoch 交给 CPU 消费并清零。
// 这里保持纯 CPU、无 Vulkan 依赖，便于 backend 测试直接覆盖 ring 的交接语义。
inline uint64_t ConsumeCompletedFrameEpoch(
    std::vector<uint64_t>& frameFenceEpochs,
    uint32_t frameIndex) noexcept
{
    if (frameIndex >= frameFenceEpochs.size())
        return 0;
    const uint64_t completedEpoch = frameFenceEpochs[frameIndex];
    frameFenceEpochs[frameIndex] = 0;
    return completedEpoch;
}

inline void RecordSubmittedFrameEpoch(
    std::vector<uint64_t>& frameFenceEpochs,
    uint32_t frameIndex,
    uint64_t submittedEpoch) noexcept
{
    if (frameIndex < frameFenceEpochs.size())
        frameFenceEpochs[frameIndex] = submittedEpoch;
}

} // namespace VL
