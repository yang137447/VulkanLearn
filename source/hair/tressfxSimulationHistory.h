#pragma once

#include <cstdint>

namespace VL::Hair
{
enum class GuideSimulationResetReason
{
    None,
    InitialState,
    FirstPose,
    PlaybackChanged,
    PoseDiscontinuity,
    PoseRecovered,
    // 显式作者/调试请求（Console `hair reset`）。它只影响下一次正 delta 帧的
    // ResetPositions，不改变 pose 历史；与源播放事件区分开，便于 trace 归因。
    Requested,
};

inline const char* GuideSimulationResetReasonName(GuideSimulationResetReason reason)
{
    switch (reason)
    {
    case GuideSimulationResetReason::InitialState: return "initial-state";
    case GuideSimulationResetReason::FirstPose: return "first-pose";
    case GuideSimulationResetReason::PlaybackChanged: return "playback-changed";
    case GuideSimulationResetReason::PoseDiscontinuity: return "pose-discontinuity";
    case GuideSimulationResetReason::PoseRecovered: return "pose-recovered";
    case GuideSimulationResetReason::Requested: return "requested";
    default: return "none";
    }
}

enum class GuideSimulationFrameAction
{
    Skip,
    Reset,
    Simulate,
};

// RT 只消费冻结 pose 的身份与不连续代数，不持有播放器。该计划同时供录制代码与
// 纯 CPU 测试使用；Reset 独占本帧，避免后续风/碰撞把刚清空的历史重新写成动量。
struct GuideSimulationFramePlan
{
    GuideSimulationFrameAction action = GuideSimulationFrameAction::Skip;
    GuideSimulationResetReason reason = GuideSimulationResetReason::None;
};

// 保存最后一次实际录制的 pose 身份；缺失快照只标记观察缺口，不用零身份覆盖历史。
// Plan 不推进状态，只有 Integration 已录制后才 Commit，暂停不会提前消费 seek 事件。
class GuideSimulationHistory
{
public:
    void ObserveMissingPose()
    {
        if (hasPoseHistory)
            poseMissing = true;
    }

    GuideSimulationFramePlan Plan(bool hasPose, uint64_t playbackIdentity,
        uint64_t discontinuity, float timeStep) const
    {
        const GuideSimulationResetReason reason =
            ResetReason(hasPose, playbackIdentity, discontinuity);
        if (reason != GuideSimulationResetReason::None)
            return {GuideSimulationFrameAction::Reset, reason};
        return {timeStep > 0.0f ? GuideSimulationFrameAction::Simulate
                              : GuideSimulationFrameAction::Skip,
            GuideSimulationResetReason::None};
    }

    void Commit(bool hasPose, uint64_t playbackIdentity, uint64_t discontinuity)
    {
        hasRecordedFrame = true;
        if (hasPose)
        {
            hasPoseHistory = true;
            lastPlaybackIdentity = playbackIdentity;
            lastDiscontinuity = discontinuity;
            poseMissing = false;
        }
    }

private:
    GuideSimulationResetReason ResetReason(bool hasPose, uint64_t playbackIdentity,
        uint64_t discontinuity) const
    {
        if (hasPose)
        {
            if (!hasPoseHistory)
                return GuideSimulationResetReason::FirstPose;
            if (playbackIdentity != lastPlaybackIdentity)
                return GuideSimulationResetReason::PlaybackChanged;
            if (discontinuity != lastDiscontinuity)
                return GuideSimulationResetReason::PoseDiscontinuity;
            // 缺失期间根矩阵被保留；恢复时没有连续运动证据，直接复位而不猜测位移阈值。
            if (poseMissing)
                return GuideSimulationResetReason::PoseRecovered;
        }
        return hasRecordedFrame ? GuideSimulationResetReason::None
                                : GuideSimulationResetReason::InitialState;
    }

    uint64_t lastPlaybackIdentity = 0;
    uint64_t lastDiscontinuity = 0;
    bool hasRecordedFrame = false;
    bool hasPoseHistory = false;
    bool poseMissing = false;
};
} // namespace VL::Hair
