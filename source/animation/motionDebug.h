#pragma once
#include <Eigen/Core>
#include <array>
#include <memory>
#include <string>
#include <vector>
#include "motionMatching.h"

namespace VL::Animation
{
// GT 冻结的世界线段/特征副本；UI 只投影，RT 只消费最终 UI 三角形。
struct MotionDebugLine
{
    Eigen::Vector3f begin, end;
    std::array<float, 4> color{0, 1, 1, 1};
};
struct MotionDebugView
{
    bool enabled = false;
    std::vector<MotionDebugLine> lines;
    std::string summary;
    std::vector<std::string> channels;
    std::vector<float> features;
    std::vector<float> normalizedQuery;
    Eigen::Matrix4f queryRoot = Eigen::Matrix4f::Identity();
    std::vector<Eigen::Vector3f> queryFutureRoots;
    bool queryUsesAchievedVelocity = false;
    std::string componentIdentity;
    std::string databaseIdentity;
    std::string payloadIdentity;
    std::string decisionReason;
    bool queryReady = false;
    uint64_t searchCount = 0;
    size_t allowedFrameCount = 0;
    size_t policyRejectedCount = 0;
    std::optional<MotionCandidate> selected;
    std::optional<MotionCandidate> continuation;
    std::vector<MotionCandidate> topCandidates;
    float blendAlpha = 1;
    // 被聚焦控制器的播放状态；playbackTime 是当前播放范围内的相对秒数，
    // UI 时间轴只读，seek/play 由 World 命令执行。
    std::string playbackClipId;
    float playbackTime = 0;
    float playbackDuration = 0;
    bool playbackPlaying = false;
    // 查询回放：World 保留最近若干帧的不可变快照；该视图只标识选中的历史帧。
    bool replayActive = false;
    int replayFrame = 0;
    int replayFrames = 0;
};
}
